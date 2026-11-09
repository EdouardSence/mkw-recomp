// What the parts of the Windows platform layer share (win32.c, win32_fs.c, win32_mem.c, win32_sys.c, win32_load.c).
#pragma once
#include <stdint.h>
#include <windows.h>

#define WPATH 2048   // wide-character path buffers

int widen(const char *s, wchar_t *w, int cap);   // win32_fs.c: UTF-8 to UTF-16; 0 (and an empty w) when it fails

static inline uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }
