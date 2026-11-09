// What the parts of the Linux platform layer share (linux.c, linux_sync.c, linux_fs.c, linux_mem.c, linux_sys.c).
#pragma once
#include <stdint.h>

int read_u64_file(const char *path, uint64_t *v);   // linux_sys.c: a decimal number from a sysfs or /proc file
void alt_stack(void);   // linux.c: a signal stack for the calling thread (the fault handler in linux_mem.c runs on it)
