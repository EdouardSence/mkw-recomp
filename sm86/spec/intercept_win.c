// Windows counterpart of intercept.c (DocumentSASS): nvdisasm.exe links its C runtime statically, so there is no
// memcpy import to preload over. spec.py loads this DLL into a suspended nvdisasm.exe; on attach it overwrites the
// exe's own memcpy (MSVC's, which also serves memmove) with a jump to logged_memcpy, which prints what intercept.c
// prints ("\n<dst src size>\n" and the bytes) and then copies.
//
// The exe is the one spec.py pins by sha256 (cuda_nvdisasm 12.8.90); its memcpy sits at RVA MEMCPY_RVA and the patch
// is refused unless its first bytes are the expected ones.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MEMCPY_RVA 0x99670
static const unsigned char kExpected[] = {0x48, 0x8b, 0xc1, 0x4c, 0x8d, 0x15};   // mov rax, rcx; lea r10, [rip+...]

static HANDLE out;
static CRITICAL_SECTION lock;

static void put(const void *p, DWORD n) {
    DWORD done;
    while (n && WriteFile(out, p, n, &done, NULL) && done) p = (const char *)p + done, n -= done;
}

// The same text as intercept.c's printf("\n<%p %p %zu>\n"), with glibc's %p spelling that funnel.py matches.
static void *logged_memcpy(void *dst, const void *src, size_t n) {
    char head[80];
    int len = snprintf(head, sizeof head, "\n<0x%llx 0x%llx %llu>\n", (unsigned long long)(uintptr_t)dst,
                       (unsigned long long)(uintptr_t)src, (unsigned long long)n);
    EnterCriticalSection(&lock);
    put(head, (DWORD)len);
    put(src, (DWORD)n);
    LeaveCriticalSection(&lock);
    return memmove(dst, src, n);   // this DLL's own memmove, not the patched one
}

static BOOL patch(void) {
    unsigned char *at = (unsigned char *)GetModuleHandleW(NULL) + MEMCPY_RVA;
    if (memcmp(at, kExpected, sizeof kExpected)) return FALSE;
    unsigned char jump[12] = {0x48, 0xb8};   // mov rax, imm64; jmp rax
    uint64_t target = (uint64_t)(uintptr_t)logged_memcpy;
    memcpy(jump + 2, &target, sizeof target);
    jump[10] = 0xff, jump[11] = 0xe0;
    DWORD old;
    if (!VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) return FALSE;
    memcpy(at, jump, sizeof jump);
    VirtualProtect(at, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    (void)dll, (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitializeCriticalSection(&lock);
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    return patch();   // FALSE: LoadLibrary fails and spec.py reports it
}
