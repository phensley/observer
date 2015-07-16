
// observer agent

// sketching a java jvmti agent to extract lightweight telemetry
// that can be used to help identify and isolate issues. - phensley

// [follows the linux kernel coding style]

#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <sys/time.h>

#ifdef __MACH__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif
 
#include <jvmti.h>
#include <jni.h>


// Undocumented API for asynchronous stack traces.

// Enums to interpret ASCGT call failures
enum {
	asgct_no_frame              = 0,
	asgct_no_class_load         = -1,
	asgct_gc_active             = -2,
	asgct_unknown_not_java      = -3,
	asgct_not_walkable_not_java = -4,
	asgct_unknown_java          = -5,
	asgct_not_walkable_java     = -6,
	asgct_unknown_state         = -7,
	asgct_thread_exit           = -8,
	asgct_deoptimization        = -9,
	asgct_safepoint             = -10
};

typedef struct {
	jint lineno;		// bytecode index
	jmethodID method_id;
} JVMPI_CallFrame;

typedef struct {
	JNIEnv *env_id;
	jint num_frames;	// .. or enum value on failure
	JVMPI_CallFrame *frames;
} JVMPI_CallTrace;

// Instead of defining the symbol as an extern, we're going
// to resolve it using dlsym()
typedef void (*ASGCT_t)(JVMPI_CallTrace *, jint, void *);

static ASGCT_t async_get_call_trace;

typedef struct {
	JavaVM *jvm;
	jvmtiEnv *jvmti;
	JNIEnv *jni;
	jrawMonitorID lock;
	JavaVMAttachArgs vm_attach_args;
} observer_t;

typedef struct {
	jlong cpu_time;
	pthread_t real_thread;
} observer_thread_t;

const static uint64_t BILLION = 1000000000;

#ifdef __MACH__
static mach_timebase_info_data_t clock_timebase;
#endif

static observer_t observer;
volatile bool liveness_flag = 0;


static inline uint64_t
nanotime()
{
#ifdef __MACH__
	uint64_t clock;

	clock = mach_absolute_time();
	return clock * (uint64_t)clock_timebase.numer / (uint64_t)clock_timebase.denom;
#else
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (ts.tv_sec * BILLION) + ts.tv_nsec;
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


// NOTES:
// - this needs to be invoked by a thread that received the SIGPROF
//   signal, since AsyncGetCallTrace can only be invoked by the current
//   thread.   thread A cannot asynchronously get the stack of thread B
//   via this function.
//
static void
execute_async_get_call_trace(ucontext_t *ucontext)
{
	int num_frames = 10;
	JVMPI_CallFrame frames[num_frames];
	JVMPI_CallTrace trace;

	// FIXME: memset not async safe...
	memset(frames, num_frames, sizeof(JVMPI_CallTrace));
	memset(&trace, 0, sizeof(JVMPI_CallTrace));

	trace.env_id = observer.jni;
	trace.frames = frames;

	async_get_call_trace(&trace, num_frames, &ucontext);

	if (trace.num_frames <= 0) {
		fprintf(stderr, "async_get_call_trace() error=%d\n", trace.num_frames);
	} else {
		fprintf(stderr, "async_get_call_trace()\n");
		for (int i = 0; i < trace.num_frames; i++) {
			if (trace.frames[i].lineno == -3) {
				fprintf(stderr, "  <native method>\n");
			} else {
				fprintf(stderr, "  frame line-no=%d  method-id=%p\n", 
					trace.frames[i].lineno,  // bytecode index
					trace.frames[i].method_id);
			}
		}
	}
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
	pthread_t max_cpu_real_thread;
	int res = 0;
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
			max_cpu_real_thread = thread_info->real_thread;
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

		// send signal to max cpu thread.
		/*
		fprintf(stderr, "sending SIGPROF to %p\n", max_cpu_real_thread);
		res = pthread_kill(max_cpu_real_thread, SIGPROF);
		if (res != 0) {
			fprintf(stderr, "error sending signal to thread %d\n", res);
		}
		*/
	}
}

static void
observer_signal_func(int signum, siginfo_t *info, void *ucontext)
{
	pthread_t thread = pthread_self();

	fprintf(stderr, "SIGNAL: caught %d on thread %p\n", signum, thread);
	execute_async_get_call_trace(ucontext);
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
	uint64_t start;
	uint64_t end;
	int count = 0;
	double elapsed = 0;

	start = nanotime();

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

	end = nanotime();

	elapsed = (end - start) / 1000000.0;
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

static void JNICALL
callback_on_class_load(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, jclass class)
{

}

static void JNICALL
callback_on_class_prepare(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread, jclass class)
{
	jvmtiError error = 0;
	jint count = 0;
	jmethodID *methods = NULL;
	char *method_name = NULL;
	char *method_sig = NULL;

	error = (*jvmti)->GetClassMethods(jvmti, class, &count, &methods);
	/* DEBUG
	for (int i = 0; i < count; i++) {
		error = (*jvmti)->GetMethodName(jvmti, methods[i], &method_name, &method_sig, NULL);
		check_error(error, "failed to get method name.");
		fprintf(stderr, "method  %p  == %s %s\n", methods[i], method_name, method_sig);
	}
	*/
	// TODO:
}

/*
 * Called when the VM thread starts.
 *
 * This will be called on the newly-created thread, before the thread's
 * method begins executing.
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
		thread_info->real_thread = pthread_self();
		fprintf(stderr, "setting tag for real thread %p\n", thread_info->real_thread);
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
	observer.jni = jni;

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

static void
init_signals()
{
        struct sigaction new_action;
        struct sigaction old_action;
        struct itimerval new_timer;
        struct itimerval old_timer;

        new_action.sa_flags = SA_RESTART | SA_SIGINFO;
        new_action.sa_sigaction = observer_signal_func;
        sigemptyset(&new_action.sa_mask);

        if (sigaction(SIGPROF, &new_action, &old_action) < 0) {
		fprintf(stderr, "FAILED sigaction\n");
	}

        new_timer.it_value.tv_sec = 0;
        new_timer.it_value.tv_usec = 25000;
        new_timer.it_interval.tv_sec = 0;
        new_timer.it_interval.tv_usec = 25000;

	if (setitimer(ITIMER_PROF, &new_timer, &old_timer) < 0) {
		fprintf(stderr, "FAILED setitimer\n");	
	}
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

	mach_timebase_info(&clock_timebase);

	init_signals();

	// Resolve the asynchronous stack trace function dynamically.
	async_get_call_trace = (ASGCT_t) dlsym(RTLD_DEFAULT, "AsyncGetCallTrace");
	if (async_get_call_trace) {
		fprintf(stderr, "resolved AsyncGetCallTrace\n");
	}

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
	capabilities.can_get_constant_pool = 1;
	capabilities.can_generate_all_class_hook_events = 1;
	capabilities.can_get_line_numbers = 1;
	capabilities.can_get_bytecodes = 1;

	error = (*jvmti)->AddCapabilities(jvmti, &capabilities);
	check_error(error, "Unable to get required capabilities.");	

	// Provide the JVMTI environment pointers to our callbacks
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.VMInit = &callback_vm_init;
	callbacks.VMDeath = &callback_vm_death;
	callbacks.VMStart = &callback_vm_start;
	callbacks.ThreadStart = &callback_thread_start;
	callbacks.ThreadEnd = &callback_thread_end;
	callbacks.ClassLoad = &callback_on_class_load;
	callbacks.ClassPrepare = &callback_on_class_prepare;
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

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_CLASS_LOAD, (jthread)NULL);
	check_error(error, "Unable to register class load event notifier.");

	error = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
		JVMTI_EVENT_CLASS_PREPARE, (jthread)NULL);
	check_error(error, "Unable to register class prepare event notifier.");

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


