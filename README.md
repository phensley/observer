
observer - Prototype of a lightweight Java thread observation agent.

Goal is to periodically emit information from the running VM that can be used
to help isolate a performance or stability issue.

Specific goals:

 * detect high cpu usage across all jvm threads
 * react quickly to log snapshot of state of high-cpu threads
   - stack depth, top N frames, cpu usage
 * minimize "observer effect" - be as efficient and transparent as possible

Building
-----
    % gradle --info clean observerSharedLibrary

Sample run
-----

    % ./run
    vm start lcation format: 1
    start_native_thread()
    vm init

    There are 5 threads running.
    -------------------
    thread that used most cpu (0.525000 ms):
     name=Finalizer priority=8 daemon=1 frames=4
        Ljava/lang/Object;.wait(J)V
        Ljava/lang/ref/ReferenceQueue;.remove(J)Ljava/lang/ref/Reference;
        Ljava/lang/ref/ReferenceQueue;.remove()Ljava/lang/ref/Reference;
        Ljava/lang/ref/Finalizer$FinalizerThread;.run()V

    thread scan took 5.024000 ms

    There are 1505 threads running.
    -------------------
    thread that used most cpu (196.286000 ms):
     name=main priority=5 daemon=0 frames=2
        Ljava/lang/Thread;.sleep(J)V
        LTest;.main([Ljava/lang/String;)V

    thread scan took 8.031000 ms

    There are 1505 threads running.
    -------------------
    thread that used most cpu (15.015000 ms):
     name=client-101 priority=5 daemon=0 frames=8
        Ljava/lang/Thread;.sleep(J)V
        LTest$Busy;.sleep()V
        LTest$Busy;.runQuux()V
        LTest$Busy;.runBaz()V
        LTest$Busy;.runBar()V
        LTest$Busy;.runFoo()V
        LTest$Busy;.run()V
        Ljava/lang/Thread;.run()V

    thread scan took 6.289000 ms

    There are 1505 threads running.
    -------------------
    thread that used most cpu (15.807000 ms):
     name=client-701 priority=5 daemon=0 frames=8
        Ljava/lang/Thread;.sleep(J)V
        LTest$Busy;.sleep()V
        LTest$Busy;.runQuux()V
        LTest$Busy;.runBaz()V
        LTest$Busy;.runBar()V
        LTest$Busy;.runFoo()V
        LTest$Busy;.run()V
        Ljava/lang/Thread;.run()V

    thread scan took 7.489000 ms

    There are 1334 threads running.
    -------------------
    thread that used most cpu (197.111000 ms):
     name=DestroyJavaVM priority=5 daemon=0 frames=0

    thread scan took 6.119000 ms
    vm death
    observer unloaded

