# runtime/core/kernel

Horizon kernel: handles, waits and threads, the virtual cores, fibers and system calls (SVCs).

| File | What it is |
|---|---|
| `kernel.c`, `kernel.h`, `kernel_int.h` | Horizon kernel: handles, waits, mutexes and threads (`kernel.c`); `kernel_int.h`, what its files share |
| `kernel_cores.c`, `kernel_preempt.c` | Switch 2's 6 virtual cores (one game thread per core, by priority; without them the game crashes, `MKW_FREE_CORES=1`; `MKW_CORE_NUM_GATE`, only for the threads that read their core number) and their asynchronous preemption |
| `kernel_svc.c`, `kernel_fiber.c`, `kernel_diag.c` | Thread and synchronization SVCs; fibers; scheduler diagnostics (`MKW_TRACE=sched`, `MKW_CRIT`, `MKW_CORE_DIAG`) |
| `svc.c` | System calls (SVCs); the service message ones (`0x1F`, `0x21`, `0x22`) stop with their name: there are no console services, the SDK is replaced by name |
