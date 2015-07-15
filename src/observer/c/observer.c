
// observer agent

// sketching a java jvmti agent to extract lightweight telemetry
// that can be used to help identify and isolate issues. - phensley

// [follows the linux kernel coding style]

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif
 
 
#include <jvmti.h>
#include <jni.h>


/**
 
TODO:
 - tag threads with a pointer to the information struct
 - delta cpu time between intervals

 */


// TODO: undocumented jvm api for getting stack trace asynchronously
// - need to determine whether its safe to use this, or if we can
// gather information from the public api, which can only capture a
// stack trace during a safe point.

/* 
typedef struct {
 jint lineno;
 jmethodID method_id;
} JVMPI_CallFrame;

typedef struct {
 JNIEnv *env_id;
 jint num_frames;
 JVMPI_CallFrame *frames;
} JVMPI_CallTrace;

typedef void (*ASGCTType)(JVMPI_CallTrace *, jint, void *);

extern void 
AsyncGetCallTrace(JVMPI_CallTrace *trace, jint depth, void* ucontext) 
__attribute__ ((weak));

*/

typedef struct {
	JavaVM *jvm;
	jvmtiEnv *jvmti;
	jrawMonitorID lock;
	JavaVMAttachArgs vm_attach_args;
} observer_t;

typedef struct {
	jlong cpu_time;
} observer_thread_t;

static observer_t observer;
volatile bool liveness_flag = 0;

// TODO: revisit the mac clock routines..
void nanotime(struct timespec *ts)
{
#ifdef __MACH__
	clock_serv_t cclock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
#else
	clock_gettime(CLOCK_REALTIME, ts);
#endif
}

// TODO: support recoverable failures?
static inline void
check_error(jvmtiError error, const char *message)
{
        switch (error) {
	case JVMTI_ERROR_NONE: 
		return;
	default: 
		fprintf(stderr, "[observer] jvmti error: %d %s\n", error, message);
		abort();
        }
}

/*
 * Get exclusive ownership of the raw monitor.
 */
static inline void
critical_section_enter()
{
	jvmtiEnv *jvmti = observer.jvmti;
	jvmtiError error;

	error = (*jvmti)->RawMonitorEnter(jvmti, observer.lock);
	check_error(error, "Failed to enter monitor.");
}

/*
 * Release ownership of the raw monitor.
 */ 
static inline void
critical_section_exit()
{
	jvmtiEnv *jvmti = observer.jvmti;
	jvmtiError error;

	error = (*jvmti)->RawMonitorExit(jvmti, observer.lock);
	check_error(error, "failed to exit monitor.");
}

/*
 * Attach the current thread to the VM as a daemon thread.
 *
 * In order to make calls to a VM from a thread we must first
 * attach it to the VM.  The VM can exit once all non-daemon
 * threads have exited.
 */
static inline void
attach_current_thread()
{
	jvmtiError error;
	JavaVM *jvm = observer.jvm;
	JNIEnv *jni = NULL;

	error = (*jvm)->AttachCurrentThreadAsDaemon(jvm, (void **)&jni, 
		&observer.vm_attach_args);
	check_error(error, "failed to attach observer thread to vm.");
}

/*
 * Detach the current thread from the VM.
 */
static inline void
detach_current_thread()
{
	JavaVM *jvm = observer.jvm;
	jvmtiError error;

	error = (*jvm)->DetachCurrentThread(jvm);
	check_error(error, "failed to detach observer thread from vm.");
}

/*
 * Obtain a VM reference to the current thread.
 */
static inline jthread
get_current_thread()
{
	jvmtiEnv *jvmti = observer.jvmti;
	jvmtiError error = 0;
	jthread thread = 0;

	error = (*jvmti)->GetCurrentThread(jvmti, &thread);
	check_error(error, "Failed to get current thread reference.");
	return thread;
}

/*
 * Return the name of a thread.
 *
 * Note: the caller must Deallocate() the returned string.
 */
static char *
get_thread_name(jvmtiEnv *jvmti, jthread thread)
{
	jvmtiThreadInfo info;
	jvmtiError error;

	error = (*jvmti)->GetThreadInfo(jvmti, thread, &info);
	check_error(error, "Failed to get thread info.");

	return info.name;
}

static void
display_method(jvmtiEnv *jvmti, jmethodID method)
{
	jvmtiError error = 0;
	jclass class;
	char *class_sig;
	char *method_name;
	char *method_sig;

        error = (*jvmti)->GetMethodDeclaringClass(jvmti, method, &class);
        check_error(error, "failed to get declaring class");

        error = (*jvmti)->GetClassSignature(jvmti, class, &class_sig, NULL);
        check_error(error, "failed to get class signature");

        error = (*jvmti)->GetMethodName(jvmti, method, &method_name, &method_sig, NULL);
        check_error(error, "failed to get method name.");

	fprintf(stderr, "\t%s.%s%s\n", class_sig, method_name, method_sig);

	(*jvmti)->Deallocate(jvmti, (void *)class_sig);
	(*jvmti)->Deallocate(jvmti, (void *)method_name);
	(*jvmti)->Deallocate(jvmti, (void *)method_sig);
}

static void
display_thread_stack(jvmtiEnv *jvmti, jthread thread)
{
	int max_frames = 20;
	jvmtiThreadInfo info;
	jvmtiFrameInfo frames[max_frames];
	jvmtiError error = 0;
	jint frame_count = 0;
	jint total_frames = 0;

        error = (*jvmti)->GetThreadInfo(jvmti, thread, &info);
        check_error(error, "failed to get thread info");

	error = (*jvmti)->GetFrameCount(jvmti, thread, &total_frames);
	check_error(error, "failed to get frame count");

	error = (*jvmti)->GetStackTrace(jvmti, thread, 0, max_frames, frames, &frame_count);
	check_error(error, "failed to get stack trace");

	fprintf(stderr, " name=%s priority=%d daemon=%d frames=%d\n", info.name, 
		info.priority, info.is_daemon, total_frames);

	// TODO: capture the key stack info but deferr method/class name
	// lookups until after exiting critical section

	for (int i = 0; i < frame_count; i++) {
		display_method(jvmti, frames[i].method);
	}

	(*jvmti)->Deallocate(jvmti, (void *)info.name);
}

static void
display_slowest_thread(int count, jthread *threads)
{
	jvmtiEnv *jvmti = observer.jvmti;
	jvmtiError error = 0;
	observer_thread_t *thread_info = NULL;
	jlong current_cpu_time = 0;
	jlong thread_cpu_time = 0;
	double elapsed = 0;
	double max_cpu_time = 0;
	jthread max_cpu_thread = 0;
	int i = 0;

	for (i = 0; i < count; i++) {
		error = (*jvmti)->GetTag(jvmti, threads[i], (jlong *)&thread_info);
		check_error(error, "Failed to get observer_thread_t from tag.");
		if (thread_info == NULL) {
			continue;
		}

		error = (*jvmti)->GetThreadCpuTime(jvmti, threads[i], &thread_cpu_time);
		check_error(error, "Failed to get thread cpu time.");

		current_cpu_time = thread_cpu_time - thread_info->cpu_time;
		elapsed = current_cpu_time / 1000000.0;

		if (elapsed > max_cpu_time) {
			max_cpu_time = elapsed;
			max_cpu_thread = threads[i];
		}
		thread_info->cpu_time = thread_cpu_time;

		// - compute thread cpu usage as a percentage of sampling
		//   interval
		// - sort threads by cpu usage
		//
		// - GetThreadListStackTraces(list) more efficient than
		//   getting stack traces per thread
		//
		// - dump top N frames from stack traces
		// - display compact histogram of thread cpu usage
	}
	if (max_cpu_thread != 0) {
		fprintf(stderr, "thread that used most cpu (%f ms):\n", max_cpu_time);
		display_thread_stack(jvmti, max_cpu_thread);
	}
}

/*
 * Iterates over all threads in the VM, performing operations.
 */
static void
observer_scan_threads()
{
	jvmtiEnv *jvmti = observer.jvmti;
	jvmtiError error = 0;
	jthread current_thread = 0;
	jthread *threads = NULL;
	struct timespec start;
	struct timespec end;
	int count = 0;
	double elapsed = 0;

	nanotime(&start);

	attach_current_thread();
	current_thread = get_current_thread();

	error = (*jvmti)->GetAllThreads(jvmti, &count, &threads);
	check_error(error, "Failed to get all threads.");

	fprintf(stderr, "\nThere are %d threads running.\n-------------------\n", count);

	critical_section_enter();
	display_slowest_thread(count, threads);
	critical_section_exit();
	detach_current_thread();

	(*jvmti)->Deallocate(jvmti, (void *)threads);

	nanotime(&end);

	elapsed = ((1000000000 * 
		(end.tv_sec - start.tv_sec)) + 
		(end.tv_nsec - start.tv_nsec)) / 1000000.0;
	fprintf(stderr, "\nthread scan took %f ms\n", elapsed);
}

/*
 * The observer thread loop.
 */
static void
observer_thread_start(void* arg)
{
	while (liveness_flag) {
		observer_scan_threads();
		// TODO: configurable sleep interval.
		usleep(1000000);
	}
}

/*
 * Creates the observer thread.
 */
static void 
start_native_thread()
{
	pthread_t thread;
	int r;

	r = pthread_create(&thread, NULL, (void*(*)(void*))observer_thread_start, NULL);
	if (r != 0) {
		fprintf(stderr, "failed to initialize timer thread: %d", r);
	} else {
		fprintf(stderr, "start_native_thread()\n");
	}
}

/*
 * Called when the VM thread starts.
 */
static void JNICALL
callback_thread_start(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	jvmtiError error;
	jvmtiPhase phase;
	char *name = NULL;
	observer_thread_t *thread_info = NULL;

	(*jvmti)->GetPhase(jvmti, &phase);

	switch (phase) {
	case JVMTI_PHASE_LIVE:
	case JVMTI_PHASE_START:
		thread_info = (observer_thread_t *)malloc(sizeof(observer_thread_t));
		memset(thread_info, 0, sizeof(observer_thread_t));
		error = (*jvmti)->SetTag(jvmti, thread, (jlong)(intptr_t)thread_info);
		check_error(error, "Failed to set tag.");
		break;
	default:
		fprintf(stderr, "thread start in wrong phase <%d>\n", phase);
	}

	if (phase == JVMTI_PHASE_LIVE) {
//		name = get_thread_name(jvmti, thread);
//		fprintf(stderr, "thread start %s %p\n", name, thread_info);
//		(*jvmti)->Deallocate(jvmti, (void *)name);
	}
}

/*
 * Called when a VM thread ends.
 */
static void JNICALL
callback_thread_end(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	jvmtiError error;
	jvmtiPhase phase;
	char *name = NULL;
	jlong thread_info = 0;

	(*jvmti)->GetPhase(jvmti, &phase);

	switch (phase) {
	case JVMTI_PHASE_LIVE:
	case JVMTI_PHASE_START:
		critical_section_enter();
		error = (*jvmti)->GetTag(jvmti, thread, &thread_info);
		check_error(error, "Failed to get tag.");
		free((void *)thread_info);
		critical_section_exit();
		break;
	default:
		fprintf(stderr, "thread end in wrong phase <%d>\n", phase);
	}

	if (phase == JVMTI_PHASE_LIVE) {
//		name = get_thread_name(jvmti, thread);
//		fprintf(stderr, "thread end %s %p\n", name, (void *)(intptr_t)thread_info);
//		(*jvmti)->Deallocate(jvmti, (void *)name);
	}
}

/*
 * Called on VM init.
 */
static void JNICALL
callback_vm_init(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	liveness_flag = 1;

	memset(&observer.vm_attach_args, 0, sizeof(JavaVMAttachArgs));
	observer.vm_attach_args.version = JNI_VERSION_1_8;
	observer.vm_attach_args.name = "observer-thread";

	start_native_thread();

	fprintf(stderr, "vm init\n");
}

/*
 * Called on VM death.
 */
static void JNICALL
callback_vm_death(jvmtiEnv *jvmti, JNIEnv *jni)
{
	liveness_flag = 0;
	fprintf(stderr, "vm death\n");
}

/*
 * Called on VM start.
 */
static void JNICALL
callback_vm_start(jvmtiEnv* jvmti, JNIEnv* env)
{
	jvmtiJlocationFormat format;
	(*jvmti)->GetJLocationFormat(jvmti, &format);
	fprintf(stderr, "vm start lcation format: %d\n", format);

}

/*
 * Main entry point for VM to load our agent.
 */
JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *jvm, char *options, void *reserved)
{
	jvmtiEnv *jvmti = NULL;
	jvmtiEventCallbacks callbacks;
	jvmtiCapabilities capabilities;
	jvmtiError error = 0;
	jint res = 0;

	// Obtain a reference to the JVMTI environment
	res = (*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_2);
	if (res != JNI_OK) {
		fprintf(stderr, "Unable to obtain reference to JVMTI\n");
		return JNI_ERR;
	}

	// Save references to the JVM and JVMTI environment
	observer.jvm = jvm;
	observer.jvmti = jvmti;

	error = (*jvmti)->CreateRawMonitor(jvmti, "observer-monitor", &observer.lock);
	check_error(error, "Unable to create raw monitor");

	// Add capabilities to our JVMTI environment
	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.can_tag_objects = 1;
	capabilities.can_get_thread_cpu_time = 1;
	capabilities.can_get_current_thread_cpu_time = 1;
	error = (*jvmti)->AddCapabilities(jvmti, &capabilities);
	check_error(error, "Unable to get required capabilities.");	

	// Provide the JVMTI environment pointers to our callbacks
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.VMInit = &callback_vm_init;
	callbacks.VMDeath = &callback_vm_death;
	callbacks.VMStart = &callback_vm_start;
	callbacks.ThreadStart = &callback_thread_start;
	callbacks.ThreadEnd = &callback_thread_end;
	error = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
	check_error(error, "Unable to register callbacks.");

	// Enable the JVMTI events that will invoke our callbacks
	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_THREAD_START, (jthread)NULL);
	check_error(error, "Unable to register thread start event notifier.");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_THREAD_END, (jthread)NULL);
	check_error(error, "Unable to register thread end event notifier.");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_VM_INIT, (jthread)NULL);
	check_error(error, "Unable to register vm init event notifier.");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_VM_DEATH, (jthread)NULL);
	check_error(error, "Unable to register vm death event notifier.");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_VM_START, (jthread)NULL);
	check_error(error, "Unable to register vm start event notifier.");

	return JNI_OK;
}

/*
 * Called on VM unloading our agent.
 */
JNIEXPORT void JNICALL 
Agent_OnUnload(JavaVM *vm)
{
	jvmtiEnv *jvmti = observer.jvmti;

	// TODO: this is in the wrong phase.. perhaps destroy in vm death?
	(*jvmti)->DestroyRawMonitor(jvmti, observer.lock);

	// TODO: clean up.
	fprintf(stderr, "observer unloaded\n");
}


