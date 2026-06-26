/* ARET Win32 layer — the "colle Windows" (doc 30, reco #2 / brique [3]).
 *
 * Native, POSIX-backed implementations of the common kernel32 surface that has a
 * clean Linux equivalent (timing, environment, heap, atomics, paths, debug out).
 * Same model as aret_crt.c: each `aret_<Name>` reads its stdcall arguments from
 * the shared machine stack ([esp+0], [esp+4], …) and calls native POSIX. These
 * are strong definitions overriding the builder's weak stubs.
 *
 * This is the unique-to-us glue between lifted code and the host OS — the part
 * neither rev.ng nor RetDec provide. It is *native* (not Wine): a real Windows
 * program's kernel32 calls run directly on Linux primitives.
 *
 * Honest scope: this is the POSIX-mappable subset (no GUI/USER32, no registry,
 * no real PE module machinery). Functions with no clean POSIX analogue stay weak
 * stubs reported at runtime; the full path for those is Winelib (doc 30 §4 [3]).
 */

#include "aret_hle.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

static inline uint32_t w32_arg(uint32_t esp, int i) {
    return ((const uint32_t *)(uintptr_t)esp)[i];
}
#define WI(i)  ((int)w32_arg(esp, (i)))
#define WU(i)  (w32_arg(esp, (i)))
#define WP(i)  ((void *)(uintptr_t)w32_arg(esp, (i)))
#define WS(i)  ((char *)(uintptr_t)w32_arg(esp, (i)))
#define WCS(i) ((const char *)(uintptr_t)w32_arg(esp, (i)))
#define WRP(x) ((uint32_t)(uintptr_t)(x))

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t aret_GetTickCount(uint32_t esp) { (void)esp; return (uint32_t)(mono_ns() / 1000000ull); }

/* QueryPerformanceCounter(LARGE_INTEGER* out) — 64-bit nanosecond counter. */
uint32_t aret_QueryPerformanceCounter(uint32_t esp) {
    uint64_t v = mono_ns();
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = (uint32_t)v; ((uint32_t *)(uintptr_t)p)[1] = (uint32_t)(v >> 32); }
    return 1;
}
/* Frequency = 1e9 (we report the counter in nanoseconds). */
uint32_t aret_QueryPerformanceFrequency(uint32_t esp) {
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = 1000000000u; ((uint32_t *)(uintptr_t)p)[1] = 0; }
    return 1;
}
/* GetSystemTimeAsFileTime(FILETIME*) — 100ns ticks since 1601. */
uint32_t aret_GetSystemTimeAsFileTime(uint32_t esp) {
    struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec * 10000000ull) + ((uint64_t)tv.tv_usec * 10ull)
                + 116444736000000000ull; /* Unix epoch -> 1601 epoch */
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = (uint32_t)ft; ((uint32_t *)(uintptr_t)p)[1] = (uint32_t)(ft >> 32); }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Environment / process identity / paths                             */
/* ------------------------------------------------------------------ */

/* A stable non-zero thread id (single-threaded model: the process id serves). */
uint32_t aret_GetCurrentThreadId(uint32_t esp) { (void)esp; return (uint32_t)getpid(); }

uint32_t aret_GetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0);
    char *buf = WS(1);
    uint32_t size = WU(2);
    const char *v = name ? getenv(name) : NULL;
    if (!v) return 0;
    uint32_t len = (uint32_t)strlen(v);
    if (buf && size > len) { memcpy(buf, v, len + 1); return len; }
    return len + 1; /* required size incl. NUL */
}
uint32_t aret_SetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0), *val = WCS(1);
    if (!name) return 0;
    return (uint32_t)(val ? (setenv(name, val, 1) == 0) : (unsetenv(name) == 0));
}
uint32_t aret_GetCurrentDirectoryA(uint32_t esp) {
    uint32_t size = WU(0); char *buf = WS(1);
    char tmp[4096];
    if (!getcwd(tmp, sizeof tmp)) return 0;
    uint32_t len = (uint32_t)strlen(tmp);
    if (buf && size > len) { memcpy(buf, tmp, len + 1); return len; }
    return len + 1;
}
uint32_t aret_SetCurrentDirectoryA(uint32_t esp) { return (uint32_t)(chdir(WCS(0)) == 0); }
uint32_t aret_GetTempPathA(uint32_t esp) {
    uint32_t size = WU(0); char *buf = WS(1);
    const char *t = getenv("TMPDIR"); if (!t) t = "/tmp/";
    uint32_t len = (uint32_t)strlen(t);
    if (buf && size > len) { memcpy(buf, t, len + 1); return len; }
    return len + 1;
}

/* OutputDebugStringA — route debug text to stderr. */
uint32_t aret_OutputDebugStringA(uint32_t esp) {
    const char *s = WCS(0);
    if (s) { size_t n = strlen(s); ssize_t w = write(2, s, n); (void)w; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* lstr* — kernel32's string helpers are just libc str*                */
/* ------------------------------------------------------------------ */

uint32_t aret_lstrlenA(uint32_t esp) { const char *s = WCS(0); return s ? (uint32_t)strlen(s) : 0; }
uint32_t aret_lstrcpyA(uint32_t esp) { return WRP(strcpy(WS(0), WCS(1))); }
uint32_t aret_lstrcatA(uint32_t esp) { return WRP(strcat(WS(0), WCS(1))); }
uint32_t aret_lstrcmpA(uint32_t esp) { return (uint32_t)(int32_t)strcmp(WCS(0), WCS(1)); }
uint32_t aret_lstrcmpiA(uint32_t esp){ return (uint32_t)(int32_t)strcasecmp(WCS(0), WCS(1)); }

/* ------------------------------------------------------------------ */
/* Heap — kernel32 heap maps onto the C allocator                      */
/* ------------------------------------------------------------------ */

uint32_t aret_GetProcessHeap(uint32_t esp) { (void)esp; return 1; } /* a non-null pseudo-handle */
uint32_t aret_HeapAlloc(uint32_t esp) {
    /* (hHeap, dwFlags, dwBytes) — HEAP_ZERO_MEMORY (0x8) -> calloc. */
    uint32_t flags = WU(1), bytes = WU(2);
    void *p = (flags & 0x8) ? calloc(1, bytes) : malloc(bytes);
    return WRP(p);
}
uint32_t aret_HeapFree(uint32_t esp) { free(WP(2)); return 1; }
uint32_t aret_HeapReAlloc(uint32_t esp) { return WRP(realloc(WP(2), WU(3))); }
uint32_t aret_HeapSize(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_HeapCreate(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_HeapDestroy(uint32_t esp) { (void)esp; return 1; }

/* VirtualAlloc(addr, size, type, protect) — back it with the allocator (we
 * ignore the requested base; the program uses the returned pointer). */
uint32_t aret_VirtualAlloc(uint32_t esp) {
    uint32_t size = WU(1);
    void *p = calloc(1, size ? size : 1);
    return WRP(p);
}
uint32_t aret_VirtualFree(uint32_t esp) { free(WP(0)); return 1; }
uint32_t aret_GlobalAlloc(uint32_t esp) {
    /* (uFlags, dwBytes) — GMEM_ZEROINIT (0x40) -> calloc. */
    uint32_t flags = WU(0), bytes = WU(1);
    return WRP((flags & 0x40) ? calloc(1, bytes) : malloc(bytes));
}
uint32_t aret_GlobalFree(uint32_t esp) { free(WP(0)); return 0; }
uint32_t aret_LocalAlloc(uint32_t esp) { return aret_GlobalAlloc(esp); }
uint32_t aret_LocalFree(uint32_t esp) { free(WP(0)); return 0; }

/* ------------------------------------------------------------------ */
/* Interlocked atomics — map onto GCC __atomic builtins                */
/* ------------------------------------------------------------------ */

uint32_t aret_InterlockedIncrement(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedDecrement(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedExchangeAdd(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_fetch_add(p, WI(1), __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedExchange(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_exchange_n(p, WI(1), __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedCompareExchange(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    int32_t cmp = WI(2), xch = WI(1);
    __atomic_compare_exchange_n(p, &cmp, xch, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return (uint32_t)cmp; /* the prior value */
}

/* ------------------------------------------------------------------ */
/* System info / locale / process metadata                            */
/* ------------------------------------------------------------------ */

/* GetSystemInfo(LPSYSTEM_INFO) — fill a plausible 32-bit layout. */
uint32_t aret_GetSystemInfo(uint32_t esp) {
    uint32_t *si = (uint32_t *)WP(0);
    if (si) {
        si[0] = 0;            /* wProcessorArchitecture/wReserved (x86=0) */
        si[1] = 0x1000;       /* dwPageSize */
        si[2] = 0x10000;      /* lpMinimumApplicationAddress */
        si[3] = 0x7ffeffff;   /* lpMaximumApplicationAddress */
        si[4] = 1;            /* dwActiveProcessorMask */
        si[5] = 1;            /* dwNumberOfProcessors */
        si[6] = 586;          /* dwProcessorType */
        si[7] = 0x10000;      /* dwAllocationGranularity */
        si[8] = (6 << 8) | 1; /* wProcessorLevel/Revision (cosmetic) */
    }
    return 0;
}
uint32_t aret_GetNativeSystemInfo(uint32_t esp) { return aret_GetSystemInfo(esp); }

uint32_t aret_GetACP(uint32_t esp)   { (void)esp; return 1252; } /* Windows-1252 */
uint32_t aret_GetOEMCP(uint32_t esp) { (void)esp; return 437; }
uint32_t aret_IsProcessorFeaturePresent(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_IsDebuggerPresent(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_IsBadReadPtr(uint32_t esp)  { return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadWritePtr(uint32_t esp) { return (uint32_t)(WU(0) == 0); }

/* Copy a fixed Windows-ish path into the caller's buffer. These APIs take
   (LPSTR lpBuffer, UINT uSize) — buffer first, then size. */
static uint32_t fill_path(uint32_t esp, const char *path) {
    char *buf = WS(0); uint32_t size = WU(1);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { memcpy(buf, path, len + 1); return len; }
    return len;
}
uint32_t aret_GetSystemDirectoryA(uint32_t esp)  { return fill_path(esp, "C:\\Windows\\System32"); }
uint32_t aret_GetWindowsDirectoryA(uint32_t esp) { return fill_path(esp, "C:\\Windows"); }

/* GetModuleFileNameA(hModule, buf, size) — report a stable fake image path. */
uint32_t aret_GetModuleFileNameA(uint32_t esp) {
    const char *path = "C:\\program.exe";
    char *buf = WS(1); uint32_t size = WU(2);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { memcpy(buf, path, len + 1); return len; }
    return 0;
}

/* GetStartupInfoA(LPSTARTUPINFOA) — zero it (cb set); benign for CRT startup. */
uint32_t aret_GetStartupInfoA(uint32_t esp) {
    uint32_t *p = (uint32_t *)WP(0);
    if (p) { memset(p, 0, 68); p[0] = 68; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Synchronisation / handles (single-process model)                   */
/* ------------------------------------------------------------------ */

uint32_t aret_CreateMutexA(uint32_t esp)        { (void)esp; return 0x100; } /* fake handle */
uint32_t aret_OpenMutexA(uint32_t esp)          { (void)esp; return 0x100; }
uint32_t aret_ReleaseMutex(uint32_t esp)        { (void)esp; return 1; }
uint32_t aret_CreateEventA(uint32_t esp)        { (void)esp; return 0x101; }
uint32_t aret_SetEvent(uint32_t esp)            { (void)esp; return 1; }
uint32_t aret_ResetEvent(uint32_t esp)          { (void)esp; return 1; }
uint32_t aret_WaitForSingleObject(uint32_t esp) { (void)esp; return 0; } /* WAIT_OBJECT_0 */
uint32_t aret_SetConsoleCtrlHandler(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_FlushFileBuffers(uint32_t esp)    { (void)esp; return 1; }
uint32_t aret_SetHandleCount(uint32_t esp)      { return WU(0); }
uint32_t aret_GetFileType(uint32_t esp)         { (void)esp; return 2; } /* FILE_TYPE_CHAR */

/* Console probes. When stdout is a real terminal these would succeed; when it is
 * a pipe/file they fail on Windows too. Reporting failure (0) makes a console
 * program treat output as non-interactive — the correct, side-effect-free choice
 * (plain output, no cursor/colour control). aret_GetStdHandle returns the fd
 * itself (0/1/2) as the HANDLE, so a console HANDLE is just its fd. */
static int aret_handle_fd(uint32_t h) { return (h <= 2u) ? (int)h : -1; }
/* msvcrt's OS-handle <-> CRT-fd bridge. On Linux a handle and an fd are the same
 * integer, so both are the identity — but they must exist: the weak stub returned
 * -1, and BusyBox added a length to that, dereferenced ~0x1, and crashed. */
uint32_t aret_open_osfhandle(uint32_t esp) { return WU(0); }
uint32_t aret_get_osfhandle(uint32_t esp) { return WU(0); }
uint32_t aret_SetErrorMode(uint32_t esp)                 { (void)esp; return 0; }
uint32_t aret_GetConsoleMode(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console */
    if (WU(1)) *(uint32_t *)(uintptr_t)WU(1) = 0x3; /* ENABLE_PROCESSED|LINE_INPUT */
    return 1;
}
uint32_t aret_GetConsoleScreenBufferInfo(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console */
    int16_t *p = (int16_t *)(uintptr_t)WU(1);
    if (!p) return 0;
    p[0] = 80; p[1] = 25;                /* dwSize           */
    p[2] = 0;  p[3] = 0;                 /* dwCursorPosition */
    p[4] = 7;                            /* wAttributes      */
    p[5] = 0; p[6] = 0; p[7] = 79; p[8] = 24; /* srWindow    */
    p[9] = 80; p[10] = 25;              /* dwMaximumWindowSize */
    return 1;
}
