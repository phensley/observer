
observer - Prototype of a lightweight Java thread observation agent.

Goal is to periodically emit information from the running VM that can be used
to help isolate a performance or stability issue.

Specific goals:

 * detect high cpu usage across all jvm threads
 * react quickly to log snapshot of state of high-cpu threads
   - stack depth, top N frames, cpu usage
 * minimize "observer effect" - be as efficient and transparent as possible


