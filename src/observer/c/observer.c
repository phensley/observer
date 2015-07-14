
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

extern void AsyncGetCallTrace(JVMPI_CallTrace *trace, jint depth, void* ucontext) __attribute__ ((weak));
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


static observer_t observer_data;

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


static inline void
critical_section_enter()
{
	jvmtiEnv *jvmti = observer_data.jvmti;
	jvmtiError error;

	error = (*jvmti)->RawMonitorEnter(jvmti, observer_data.lock);
	check_error(error, "Failed to enter monitor.");
}


static inline void
critical_section_exit()
{
	jvmtiEnv *jvmti = observer_data.jvmti;
	jvmtiError error;

	error = (*jvmti)->RawMonitorExit(jvmti, observer_data.lock);
	check_error(error, "failed to exit monitor.");
}


static inline void
attach_current_thread()
{
	JavaVM *jvm = observer_data.jvm;
	JNIEnv *jni = NULL;

	(*jvm)->AttachCurrentThread(jvm, (void **)&jni, &observer_data.vm_attach_args);
}


static inline void
detach_current_thread()
{
	JavaVM *jvm = observer_data.jvm;

	(*jvm)->DetachCurrentThread(jvm);
}


static inline jthread
get_current_thread()
{
	jvmtiEnv *jvmti = observer_data.jvmti;
	jvmtiError error = 0;
	jthread thread = 0;

	error = (*jvmti)->GetCurrentThread(jvmti, &thread);
	check_error(error, "Failed to get current thread reference.");
	return thread;
}


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
observer_scan_threads()
{
	jvmtiEnv *jvmti = observer_data.jvmti;
	jvmtiError error = 0;
	jthread current_thread = 0;
	jthread *threads = NULL;
	jlong thread_nanos = 0;
	char *name = NULL;
	int count = 0;
	int i = 0;
	observer_thread_t *thread_info;
	struct timespec start;
	struct timespec end;
	double elapsed = 0;

	nanotime(&start);

	attach_current_thread();
	current_thread = get_current_thread();

	error = (*jvmti)->GetAllThreads(jvmti, &count, &threads);
	check_error(error, "Failed to get all threads.");

	fprintf(stderr, "There are %d threads running.\n", count);
	for (i = 0; i < count; i++) {
		error = (*jvmti)->GetTag(jvmti, threads[i], (jlong *)&thread_info);
		check_error(error, "Failed to get observer_thread_t from tag.");
		error = (*jvmti)->GetThreadCpuTime(jvmti, threads[i], &thread_nanos);
		check_error(error, "Failed to get thread cpu time.");
		name = get_thread_name(jvmti, threads[i]);
		fprintf(stderr, "STATS: thread %s %lu %p\n", name, thread_nanos, thread_info);
		(*jvmti)->Deallocate(jvmti, (void *)name);
	}

	detach_current_thread();
	nanotime(&end);

	elapsed = ((1000000000 * (end.tv_sec - start.tv_sec)) + (end.tv_nsec - start.tv_nsec)) / 1000000.0;
	fprintf(stderr, "thread scan took %f ms\n", elapsed);
}


static void
observer_thread_start(void* arg)
{

	// initialize
	//
	// while {
	//
	//   GetAllThreads
	//   for (t : threads) {
	//	GetThreadCpuTime()
	//   }
	//   delta cpu time from prior
	//    - new threads get added and zeroed
	//    - inactive threads get removed
	//   sort threads by cpu time
	//   for (t : busiest) {
	//     get top n frames
	//     log
	//   }
	// }

	while (liveness_flag) {
		observer_scan_threads();
		// TODO: configurable sleep interval.
		usleep(1000000);
	}
}


static void 
start_native_thread()
{
	pthread_t thread;
	int r;

	r = pthread_create(&thread, NULL, (void*(*)(void*))observer_thread_start, NULL);
	if (r != 0) {
		fprintf(stderr, "failed to initialize timer thread: %d", r);
	} else {
		fprintf(stderr, "start_native_thread(): %llu\n", (uint64_t)thread);
	}
}


static void JNICALL
callback_thread_start(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	jvmtiError error;
	jvmtiPhase phase;
	char *name = NULL;
	observer_thread_t *thread_info = NULL;

	// TODO: enter critical section
	// TODO: add thread id to set

	(*jvmti)->GetPhase(jvmti, &phase);

	switch (phase) {
	case JVMTI_PHASE_LIVE:
	case JVMTI_PHASE_START:
	{
		error = (*jvmti)->Allocate(jvmti, (jlong)sizeof(observer_thread_t), (unsigned char **)&thread_info);
		check_error(error, "Failed to allocate observer_thread_t");
		error = (*jvmti)->SetTag(jvmti, thread, (jlong)(intptr_t)thread_info);
		check_error(error, "Failed to set tag.");

		break;
	}
	default:
		fprintf(stderr, "thread end in wrong phase <%d>\n", phase);
	}

	if (phase == JVMTI_PHASE_LIVE) {
		name = get_thread_name(jvmti, thread);
		fprintf(stderr, "thread start %s %p\n", name, thread_info);
		(*jvmti)->Deallocate(jvmti, (void *)name);
	}
}


static void JNICALL
callback_thread_end(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	jvmtiError error;
	jvmtiPhase phase;
	char *name = NULL;
	jlong thread_info = 0;

	// TODO: enter critical section
	// TODO: remove thread id from set

	(*jvmti)->GetPhase(jvmti, &phase);

	switch (phase) {
	case JVMTI_PHASE_LIVE:
	case JVMTI_PHASE_START:
		error = (*jvmti)->GetTag(jvmti, thread, &thread_info);
		check_error(error, "Failed to get tag.");
		error = (*jvmti)->Deallocate(jvmti, (void *)(observer_thread_t *)thread_info);
		check_error(error, "failed to deallocate observer_thread_t.");

		break;

	default:
		fprintf(stderr, "thread end in wrong phase <%d>\n", phase);
	}

	if (phase == JVMTI_PHASE_LIVE) {
		name = get_thread_name(jvmti, thread);
		fprintf(stderr, "thread end %s %p\n", name, (void *)(intptr_t)thread_info);
		(*jvmti)->Deallocate(jvmti, (void *)name);
	}
}


static void JNICALL
callback_vm_init(jvmtiEnv *jvmti, JNIEnv *jni, jthread thread)
{
	liveness_flag = 1;
	observer_data.jvmti = jvmti;

	memset(&observer_data.vm_attach_args, 0, sizeof(JavaVMAttachArgs));
        observer_data.vm_attach_args.version = JNI_VERSION_1_8;
        observer_data.vm_attach_args.name = "observer-thread";

	// thread to periodically trigger profiling
	start_native_thread();

	fprintf(stderr, "vm init\n");
}


static void JNICALL
callback_vm_death(jvmtiEnv *jvmti, JNIEnv *jni)
{
	liveness_flag = 0;
	fprintf(stderr, "vm death\n");
}


static void JNICALL
callback_vm_start(jvmtiEnv* jvmti, JNIEnv* env)
{
	jvmtiJlocationFormat format;
	(*jvmti)->GetJLocationFormat(jvmti, &format);
	fprintf(stderr, "vm start lcation format: %d\n", format);

}


JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *jvm, char *options, void *reserved)
{
	jvmtiEnv *jvmti;
	jvmtiEventCallbacks callbacks;
	jvmtiCapabilities capabilities;
	jvmtiError error;
	jint res;

	observer_data.jvm = jvm;

	res = (*jvm)->GetEnv(jvm, (void **)&jvmti, JVMTI_VERSION_1_2);
	if (res != JNI_OK) {
		fprintf(stderr, "Unable to obtain reference to JVMTI\n");
		return JNI_ERR;
	}

	memset(&capabilities, 0, sizeof(capabilities));
	capabilities.can_tag_objects = 1;
	capabilities.can_get_thread_cpu_time = 1;
	capabilities.can_get_current_thread_cpu_time = 1;

	error = (*jvmti)->AddCapabilities(jvmti, &capabilities);
	check_error(error, "Unable to get required capabilities.");	

	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.VMInit = &callback_vm_init;
	callbacks.VMDeath = &callback_vm_death;
	callbacks.VMStart = &callback_vm_start;
	callbacks.ThreadStart = &callback_thread_start;
	callbacks.ThreadEnd = &callback_thread_end;

	error = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, (jint)sizeof(callbacks));
	check_error(error, "Unable to register callbacks.");

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


JNIEXPORT void JNICALL 
Agent_OnUnload(JavaVM *vm)
{
	// TODO: destroy
	fprintf(stderr, "observer unloaded\n");
}


