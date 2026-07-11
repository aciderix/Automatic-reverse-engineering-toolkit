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
#include <malloc.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#ifndef __wasm__
#include <sys/statvfs.h>   /* no filesystem statvfs under wasm32-wasi */
#endif
#include <sys/ioctl.h>

/* G2b (doc 72): a *visible* window is presented via SDL2 (portable: Linux/macOS,
 * and WASM via Emscripten later). SDL2 is linked ONLY when the program creates a
 * window (builder gates `-DARET_HAVE_SDL` on a window-creating import + pkg-config
 * sdl2). When absent, the whole window layer stays display-free (sound no-op
 * drawing), exactly as before. The SDL layer is strictly *additive*: it never
 * injects a message that Wine would not, so the deterministic message/geometry
 * oracles stay bit-identical. */
#ifdef ARET_HAVE_SDL
#include <SDL.h>
#endif

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

/* Fill a SYSTEMTIME (8 WORDs: year,month,dow,day,hour,min,sec,ms) from a broken-
 * down time. Shared by GetSystemTime (UTC) and GetLocalTime. */
static void aret_fill_systemtime(uint32_t p, int local) {
    if (!p) return;
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec; struct tm tmv;
    if (local) localtime_r(&t, &tmv); else gmtime_r(&t, &tmv);
    uint16_t *w = (uint16_t *)(uintptr_t)p;
    w[0] = (uint16_t)(tmv.tm_year + 1900);
    w[1] = (uint16_t)(tmv.tm_mon + 1);
    w[2] = (uint16_t)tmv.tm_wday;
    w[3] = (uint16_t)tmv.tm_mday;
    w[4] = (uint16_t)tmv.tm_hour;
    w[5] = (uint16_t)tmv.tm_min;
    w[6] = (uint16_t)tmv.tm_sec;
    w[7] = (uint16_t)(tv.tv_usec / 1000);
}
uint32_t aret_GetSystemTime(uint32_t esp) { aret_fill_systemtime(WU(0), 0); return 0; }
uint32_t aret_GetLocalTime(uint32_t esp) { aret_fill_systemtime(WU(0), 1); return 0; }

/* SystemTimeToFileTime(const SYSTEMTIME*, FILETIME*) -> 100ns ticks since 1601. */
uint32_t aret_SystemTimeToFileTime(uint32_t esp) {
    const uint16_t *w = (const uint16_t *)(uintptr_t)WU(0);
    uint32_t out = WU(1);
    if (!w || !out) return 0;
    struct tm tmv; memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = w[0] - 1900; tmv.tm_mon = w[1] - 1; tmv.tm_mday = w[3];
    tmv.tm_hour = w[4]; tmv.tm_min = w[5]; tmv.tm_sec = w[6];
    time_t t = timegm(&tmv);
    uint64_t ft = ((uint64_t)t * 10000000ull) + ((uint64_t)w[7] * 10000ull) + 116444736000000000ull;
    ((uint32_t *)(uintptr_t)out)[0] = (uint32_t)ft;
    ((uint32_t *)(uintptr_t)out)[1] = (uint32_t)(ft >> 32);
    return 1;
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
/* GetEnvironmentVariableW(name(wide), buf(wide), size(WCHARs)) — wide sibling of
 * the A variant. Narrows the UTF-16 name (ASCII/Latin-1, the CLI case) to look up
 * via getenv, widens the value back. Returns WCHARs written excl. NUL, the
 * required size incl. NUL if buf is too small, or 0 (not found). */
uint32_t aret_GetEnvironmentVariableW(uint32_t esp) {
    const uint16_t *wname = (const uint16_t *)WP(0);
    uint16_t *buf = (uint16_t *)WP(1);
    uint32_t size = WU(2);
    if (!wname) return 0;
    char name[256]; size_t i = 0;
    for (; wname[i] && i + 1 < sizeof name; i++) name[i] = (char)(wname[i] & 0xffu);
    name[i] = 0;
    const char *v = getenv(name);
    if (!v) return 0;
    uint32_t len = (uint32_t)strlen(v);
    if (buf && size > len) {
        for (uint32_t j = 0; j < len; j++) buf[j] = (uint16_t)(unsigned char)v[j];
        buf[len] = 0;
        return len;
    }
    return len + 1; /* required size incl. NUL */
}
uint32_t aret_SetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0), *val = WCS(1);
    if (!name) return 0;
    return (uint32_t)(val ? (setenv(name, val, 1) == 0) : (unsetenv(name) == 0));
}
/* GetEnvironmentStringsW/A() -> a freshly allocated, double-NUL-terminated block
 * of "VAR=VALUE\0" strings built from the host environment (wide for W, bytes for
 * A). Programs that read the whole environment (PuTTY/plink at startup) walk this
 * block; the old stub returned NULL and the caller dereferenced it -> segfault.
 * Freed by FreeEnvironmentStrings{W,A}. */
extern char **environ;
uint32_t aret_GetEnvironmentStringsW(uint32_t esp) {
    (void)esp;
    size_t n = 1; /* the block's trailing NUL */
    for (char **e = environ; e && *e; e++) n += strlen(*e) + 1;
    uint16_t *block = (uint16_t *)malloc(n * sizeof(uint16_t));
    if (!block) return 0;
    uint16_t *p = block;
    for (char **e = environ; e && *e; e++) {
        for (const char *s = *e; *s; s++) *p++ = (uint16_t)(unsigned char)*s;
        *p++ = 0;
    }
    *p = 0; /* block terminator (empty string) */
    return (uint32_t)(uintptr_t)block;
}
uint32_t aret_GetEnvironmentStringsA(uint32_t esp) {
    (void)esp;
    size_t n = 1;
    for (char **e = environ; e && *e; e++) n += strlen(*e) + 1;
    char *block = (char *)malloc(n);
    if (!block) return 0;
    char *p = block;
    for (char **e = environ; e && *e; e++) {
        size_t l = strlen(*e) + 1;
        memcpy(p, *e, l);
        p += l;
    }
    *p = 0;
    return (uint32_t)(uintptr_t)block;
}
/* GetEnvironmentStrings (no suffix) is the ANSI entry on Win32. */
uint32_t aret_GetEnvironmentStrings(uint32_t esp) { return aret_GetEnvironmentStringsA(esp); }
uint32_t aret_FreeEnvironmentStringsW(uint32_t esp) { free(WP(0)); return 1; }
uint32_t aret_FreeEnvironmentStringsA(uint32_t esp) { free(WP(0)); return 1; }

/* ---- Registry: a sound, EMPTY, read-only hive ----------------------------
 * We do not emulate the Windows registry. Rather than lie that a key opened (the
 * old stub returned ERROR_SUCCESS with an uninitialised HKEY, which callers then
 * query/close), model an empty read-only hive: opens and value queries report
 * "not found", key enumeration is empty, and writes fail honestly instead of
 * silently dropping data. A program probing the registry for optional config
 * (PuTTY/plink at startup: jump-list, saved sessions) takes its default path; one
 * that truly needs a value fails loud, never silently wrong. LSTATUS codes:
 * SUCCESS=0, FILE_NOT_FOUND=2, ACCESS_DENIED=5, NO_MORE_ITEMS=259. */
uint32_t aret_RegOpenKeyExA(uint32_t esp) {
    uint32_t *phk = (uint32_t *)WP(4); /* phkResult */
    if (phk) *phk = 0;
    return 2; /* ERROR_FILE_NOT_FOUND — no such key */
}
uint32_t aret_RegCreateKeyExA(uint32_t esp) {
    uint32_t *phk = (uint32_t *)WP(7); /* phkResult (8th arg) */
    if (phk) *phk = 0;
    return 5; /* ERROR_ACCESS_DENIED — read-only hive, cannot create */
}
uint32_t aret_RegQueryValueExA(uint32_t esp) { (void)esp; return 2; }   /* not found */
uint32_t aret_RegSetValueExA(uint32_t esp) { (void)esp; return 5; }     /* denied */
uint32_t aret_RegEnumKeyA(uint32_t esp) { (void)esp; return 259; }      /* no more items */
uint32_t aret_RegCloseKey(uint32_t esp) { (void)esp; return 0; }        /* always OK */
/* ExpandEnvironmentStringsA(src, dst, size): substitute %NAME% with getenv(NAME),
 * copy literals through. Returns the length written including the NUL (or the
 * required size if dst is too small / NULL), matching the Win32 contract. */
uint32_t aret_ExpandEnvironmentStringsA(uint32_t esp) {
    const char *src = WCS(0); char *dst = WS(1); uint32_t size = WU(2);
    if (!src) return 0;
    char out[4096]; size_t o = 0;
    for (const char *p = src; *p && o < sizeof out - 1;) {
        const char *e;
        if (*p == '%' && (e = strchr(p + 1, '%'))) {
            size_t nl = (size_t)(e - p - 1);
            char name[256];
            if (nl < sizeof name) {
                memcpy(name, p + 1, nl); name[nl] = 0;
                const char *v = getenv(name);
                if (v) { size_t vl = strlen(v); if (o + vl < sizeof out - 1) { memcpy(out + o, v, vl); o += vl; } }
                p = e + 1; continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    uint32_t need = (uint32_t)o + 1;
    if (dst && size >= need) memcpy(dst, out, need);
    return need;
}
/* GetFullPathNameA(name, len, buf, *filePart) -> length. Resolves against the
 * cwd; sets *filePart to the last component within buf. The full prefix (cwd)
 * differs by host, but callers use filePart and the basename, which are stable. */
uint32_t aret_GetFullPathNameA(uint32_t esp) {
    const char *name = WCS(0);
    uint32_t buflen = WU(1);
    char *buf = WS(2);
    uint32_t *filepart = (uint32_t *)(uintptr_t)WU(3);
    char tmp[4096];
    if (name && (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':'))) {
        snprintf(tmp, sizeof tmp, "%s", name);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, name ? name : "");
    }
    uint32_t len = (uint32_t)strlen(tmp);
    if (!buf || buflen <= len) return len + 1; /* required size incl. NUL */
    memcpy(buf, tmp, len + 1);
    if (filepart) {
        char *sep = NULL;
        for (char *p = buf; *p; p++) if (*p == '/' || *p == '\\') sep = p;
        *filepart = (uint32_t)(uintptr_t)(sep ? sep + 1 : buf);
    }
    return len;
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
    const char *e = getenv("TMPDIR"); if (!e || !*e) e = "/tmp";
    char t[4096]; size_t len = strlen(e);
    if (len + 2 > sizeof t) return 0;
    memcpy(t, e, len);
    if (t[len - 1] != '/') t[len++] = '/';   /* Windows GetTempPath always ends in a sep */
    t[len] = 0;
    if (buf && size > len) { memcpy(buf, t, len + 1); return (uint32_t)len; }
    return (uint32_t)len + 1;
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
/* HeapSize(hHeap, dwFlags, lpMem) — the usable size of an allocation. sqlite's
 * Windows allocator (`winMemSize`) uses it to track memory; returning 0 made it
 * believe every block was empty and abort with "out of memory". */
uint32_t aret_HeapSize(uint32_t esp) {
    void *p = WP(2);
    return p ? (uint32_t)malloc_usable_size(p) : 0;
}
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
/* GlobalHandle(pMem) -> HGLOBAL. Our heap is fixed (handle == pointer, GlobalLock
 * is identity), so the handle for a locked pointer is the pointer itself. */
uint32_t aret_GlobalHandle(uint32_t esp) { return WU(0); }
uint32_t aret_LocalHandle(uint32_t esp) { return WU(0); }

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

/* GetVersionExA/W(LPOSVERSIONINFO): report a real Windows version and return
 * TRUE, exactly as Windows/Wine do (Wine 9 reports 6.2.9200 NT). The caller sets
 * dwOSVersionInfoSize (v[0]); we fill the version fields. szCSDVersion is left
 * zeroed (empty service pack — a valid state the program handles). Faithful
 * values matter: the CRT/VFS pick their code path from the reported version. */
uint32_t aret_GetVersionExA(uint32_t esp) {
    uint32_t *v = (uint32_t *)WP(0);
    if (v) {
        v[1] = 6;      /* dwMajorVersion */
        v[2] = 2;      /* dwMinorVersion (6.2 = Windows 8 / Server 2012) */
        v[3] = 9200;   /* dwBuildNumber */
        v[4] = 2;      /* dwPlatformId = VER_PLATFORM_WIN32_NT */
        memset((char *)&v[5], 0, 128); /* szCSDVersion[128] (ANSI) */
    }
    return 1; /* TRUE */
}
uint32_t aret_GetVersionExW(uint32_t esp) {
    uint32_t *v = (uint32_t *)WP(0);
    if (v) {
        v[1] = 6; v[2] = 2; v[3] = 9200; v[4] = 2;
        memset((char *)&v[5], 0, 256); /* szCSDVersion[128] (WCHAR) */
    }
    return 1;
}
/* GetVersion(): the legacy packed form, kept consistent with GetVersionEx above
 * (6.2.9200, NT). LOBYTE(LOWORD)=major, HIBYTE(LOWORD)=minor; bit 31 = 0 for NT,
 * and then HIWORD = build number. Old programs branch on this, so it must agree
 * with GetVersionEx (Wine's two version APIs agree too). 6 | 2<<8 | 9200<<16. */
uint32_t aret_GetVersion(uint32_t esp) {
    (void)esp;
    return 6u | (2u << 8) | (9200u << 16); /* 0x23F00206 — 6.2.9200, NT */
}

/* RtlMoveMemory(dst, src, len) -> void. Overlap-safe copy = memmove. */
uint32_t aret_RtlMoveMemory(uint32_t esp) {
    void *d = WP(0);
    const void *s = (const void *)WP(1);
    if (d && s) memmove(d, s, WU(2));
    return 0;
}

/* Days since 1970-01-01 for a proleptic-Gregorian date (Howard Hinnant's
 * algorithm) — portable (no timegm; works under wasi too). */
static int64_t aret_days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
/* DosDateTimeToFileTime(wFatDate, wFatTime, LPFILETIME) -> BOOL. MS-DOS packed
 * date/time -> a 64-bit FILETIME (100ns ticks since 1601), no timezone shift —
 * the canonical field->FILETIME calendar computation (matches Wine). FAT date:
 * bits 0-4 day, 5-8 month, 9-15 year-since-1980. FAT time: 0-4 sec/2, 5-10 min,
 * 11-15 hour. */
uint32_t aret_DosDateTimeToFileTime(uint32_t esp) {
    uint32_t d = WU(0) & 0xffff, t = WU(1) & 0xffff;
    uint32_t *ft = (uint32_t *)WP(2);
    int year = (int)((d >> 9) & 0x7f) + 1980;
    unsigned mon = (d >> 5) & 0x0f, day = d & 0x1f;
    unsigned hour = (t >> 11) & 0x1f, min = (t >> 5) & 0x3f, sec = (t & 0x1f) * 2;
    if (mon < 1) mon = 1;
    if (day < 1) day = 1;
    int64_t days = aret_days_from_civil(year, mon, day);
    int64_t secs = days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
    uint64_t ticks = ((uint64_t)(secs + 11644473600LL)) * 10000000ULL;
    if (ft) { ft[0] = (uint32_t)ticks; ft[1] = (uint32_t)(ticks >> 32); }
    return 1;
}

/* AreFileApisANSI(): the process uses the ANSI code page for narrow file APIs,
 * the Windows default (Wine returns TRUE). */
uint32_t aret_AreFileApisANSI(uint32_t esp) { (void)esp; return 1; }

uint32_t aret_GetACP(uint32_t esp)   { (void)esp; return 1252; } /* Windows-1252 */
uint32_t aret_GetOEMCP(uint32_t esp) { (void)esp; return 437; }
/* Locale IDs: report en-US (LCID 0x0409) — the CRT reads GetThreadLocale for
 * locale-dependent classification (GNU m4/grep query it at startup; a 0 stub made
 * their locale setup misbehave and swallow output). */
uint32_t aret_GetThreadLocale(uint32_t esp)          { (void)esp; return 0x0409; }
uint32_t aret_GetUserDefaultLCID(uint32_t esp)       { (void)esp; return 0x0409; }
uint32_t aret_GetSystemDefaultLCID(uint32_t esp)     { (void)esp; return 0x0409; }
uint32_t aret_GetUserDefaultUILanguage(uint32_t esp) { (void)esp; return 0x0409; }
uint32_t aret_SetThreadLocale(uint32_t esp)          { (void)esp; return 0x0409; }
/* PeekNamedPipe(h, buf, size, *read, *avail, *left): CLI tools call it to see
 * whether stdin has data / is a pipe. Our handle is a POSIX fd; use FIONREAD for
 * the available-byte count. Non-pipe fds (a file/tty) fail as on Windows. A true
 * peek (reading without consuming) is not needed by the callers — they only test
 * `*avail`; report 0 read. Returns BOOL. */
uint32_t aret_PeekNamedPipe(uint32_t esp) {
    int fd = WI(0);
    uint32_t *bytes_read = (uint32_t *)WP(3);
    uint32_t *total_avail = (uint32_t *)WP(4);
    uint32_t *left = (uint32_t *)WP(5);
    int navail = 0;
    if (ioctl(fd, FIONREAD, &navail) != 0) return 0; /* not a pipe -> FALSE */
    if (bytes_read) *bytes_read = 0;
    if (total_avail) *total_avail = (uint32_t)navail;
    if (left) *left = (uint32_t)navail;
    return 1;
}
uint32_t aret_IsValidCodePage(uint32_t esp) {
    switch (WU(0)) {
        case 437: case 850: case 852: case 866: case 874: case 932: case 936:
        case 949: case 950: case 1200: case 1201: case 1250: case 1251: case 1252:
        case 1253: case 1254: case 1255: case 1256: case 1257: case 1258:
        case 65000: case 65001: return 1;
        default: return 0;
    }
}
/* GetCPInfo: the code pages we report are single-byte (MaxCharSize 1); default
 * char '?', no DBCS lead-byte ranges. CPINFO = {UINT MaxCharSize; BYTE Default[2];
 * BYTE LeadByte[12];}. */
uint32_t aret_GetCPInfo(uint32_t esp) {
    uint8_t *ci = (uint8_t *)(uintptr_t)WU(1);
    if (!ci) return 0;
    *(uint32_t *)(ci + 0) = 1;
    ci[4] = '?'; ci[5] = 0;
    memset(ci + 6, 0, 12);
    return 1;
}
/* GetStringTypeW(CT_CTYPE1, src, count, out): classify each WCHAR into the C1_*
 * character-type bits (ASCII subset via ctype). */
uint32_t aret_GetStringTypeW(uint32_t esp) {
    if (WU(0) != 1) return 0; /* only CT_CTYPE1 modelled */
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(1);
    int n = (int)WU(2);
    uint16_t *out = (uint16_t *)(uintptr_t)WU(3);
    if (!s || !out) return 0;
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (int i = 0; i < n; i++) {
        unsigned c = s[i];
        uint16_t t = 0;
        if (c < 256) {
            if (isupper((int)c)) t |= 0x0001 | 0x0100; /* UPPER | ALPHA */
            if (islower((int)c)) t |= 0x0002 | 0x0100; /* LOWER | ALPHA */
            if (isdigit((int)c)) t |= 0x0004;          /* DIGIT */
            if (isspace((int)c)) t |= 0x0008;          /* SPACE */
            if (ispunct((int)c)) t |= 0x0010;          /* PUNCT */
            if (iscntrl((int)c)) t |= 0x0020;          /* CNTRL */
            if (isblank((int)c)) t |= 0x0040;          /* BLANK */
            if (isxdigit((int)c)) t |= 0x0080;         /* XDIGIT */
            if (t) t |= 0x0200;                        /* DEFINED */
        }
        out[i] = t;
    }
    return 1;
}
/* LCMapStringW: the upper/lower-case mappings (the common CRT use); other flags
 * pass the string through unchanged. */
uint32_t aret_LCMapStringW(uint32_t esp) {
    uint32_t flags = WU(1);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(2);
    int srclen = (int)WU(3);
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(4);
    int dstlen = (int)WU(5);
    if (!src) return 0;
    if (srclen < 0) { srclen = 0; while (src[srclen]) srclen++; srclen++; }
    if (dstlen == 0 || !dst) return (uint32_t)srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        unsigned c = src[i];
        if (c < 256) {
            if (flags & 0x00000200u) c = (unsigned)toupper((int)c); /* LCMAP_UPPERCASE */
            else if (flags & 0x00000100u) c = (unsigned)tolower((int)c); /* LCMAP_LOWERCASE */
        }
        dst[i] = (uint16_t)c;
    }
    return (uint32_t)n;
}
/* GetStringTypeA(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType) — ANSI twin of
 * GetStringTypeW (note the extra leading Locale arg). Single-byte ctype. */
uint32_t aret_GetStringTypeA(uint32_t esp) {
    if (WU(1) != 1) return 0;                    /* CT_CTYPE1 only */
    const unsigned char *s = (const unsigned char *)WP(2);
    int n = WI(3);
    uint16_t *out = (uint16_t *)WP(4);
    if (!s || !out) return 0;
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (int i = 0; i < n; i++) {
        unsigned c = s[i]; uint16_t t = 0;
        if (isupper((int)c)) t |= 0x0001 | 0x0100;
        if (islower((int)c)) t |= 0x0002 | 0x0100;
        if (isdigit((int)c)) t |= 0x0004;
        if (isspace((int)c)) t |= 0x0008;
        if (ispunct((int)c)) t |= 0x0010;
        if (iscntrl((int)c)) t |= 0x0020;
        if (isblank((int)c)) t |= 0x0040;
        if (isxdigit((int)c)) t |= 0x0080;
        if (t) t |= 0x0200;
        out[i] = t;
    }
    return 1;
}
/* LCMapStringA — ANSI twin of LCMapStringW (upper/lower-case; else pass-through). */
uint32_t aret_LCMapStringA(uint32_t esp) {
    uint32_t flags = WU(1);
    const unsigned char *src = (const unsigned char *)WP(2);
    int srclen = WI(3);
    char *dst = (char *)WP(4);
    int dstlen = WI(5);
    if (!src) return 0;
    if (srclen < 0) { srclen = 0; while (src[srclen]) srclen++; srclen++; }
    if (dstlen == 0 || !dst) return (uint32_t)srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        unsigned c = src[i];
        if (flags & 0x00000200u) c = (unsigned)toupper((int)c);        /* LCMAP_UPPERCASE */
        else if (flags & 0x00000100u) c = (unsigned)tolower((int)c);   /* LCMAP_LOWERCASE */
        dst[i] = (char)c;
    }
    return (uint32_t)n;
}
/* CompareStringA/W(Locale, dwCmpFlags, s1, n1, s2, n2) -> CSTR_LESS_THAN(1)/
 * EQUAL(2)/GREATER_THAN(3). Ordinal-ish (matches Wine for ASCII/en-US); honours
 * NORM_IGNORECASE. Deep locale collation of accented text is not modelled. */
uint32_t aret_CompareStringA(uint32_t esp) {
    uint32_t flags = WU(1);
    const char *a = (const char *)WP(2); int na = WI(3);
    const char *b = (const char *)WP(4); int nb = WI(5);
    if (!a || !b) return 0;
    if (na < 0) na = (int)strlen(a);
    if (nb < 0) nb = (int)strlen(b);
    int ci = (flags & 0x00000001u) != 0;         /* NORM_IGNORECASE */
    int i = 0;
    for (; i < na && i < nb; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ci) { ca = tolower(ca); cb = tolower(cb); }
        if (ca != cb) return ca < cb ? 1u : 3u;
    }
    return na == nb ? 2u : (na < nb ? 1u : 3u);
}
uint32_t aret_CompareStringW(uint32_t esp) {
    uint32_t flags = WU(1);
    const uint16_t *a = (const uint16_t *)WP(2); int na = WI(3);
    const uint16_t *b = (const uint16_t *)WP(4); int nb = WI(5);
    if (!a || !b) return 0;
    if (na < 0) { na = 0; while (a[na]) na++; }
    if (nb < 0) { nb = 0; while (b[nb]) nb++; }
    int ci = (flags & 0x00000001u) != 0;
    int i = 0;
    for (; i < na && i < nb; i++) {
        int ca = a[i], cb = b[i];
        if (ci && ca < 256 && cb < 256) { ca = tolower(ca); cb = tolower(cb); }
        if (ca != cb) return ca < cb ? 1u : 3u;
    }
    return na == nb ? 2u : (na < nb ? 1u : 3u);
}
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
uint32_t aret_GetModuleFileNameW(uint32_t esp) {
    const char *path = "C:\\program.exe";
    uint16_t *buf = (uint16_t *)(uintptr_t)WU(1); uint32_t size = WU(2);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { for (uint32_t i = 0; i <= len; i++) buf[i] = (unsigned char)path[i]; return len; }
    return 0;
}
uint32_t aret_GetModuleHandleExW(uint32_t esp) {
    uint32_t *out = (uint32_t *)(uintptr_t)WU(2);
    if (out) *out = 0x00400000u;
    return 1;
}
/* Pseudo-handles: GetCurrentProcess()/Thread() are the constants -1 / -2. */
uint32_t aret_GetCurrentProcess(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }

/* GetExitCodeProcess(hProcess, lpExitCode) -> BOOL. We create no child processes
 * (CreateProcess is a sound failure), so any handle a program holds is the
 * current-process pseudo-handle — report STILL_ACTIVE (259), exactly as a running
 * process does (matches Wine for GetCurrentProcess). */
uint32_t aret_GetExitCodeProcess(uint32_t esp) {
    uint32_t *code = (uint32_t *)WP(1);
    if (code) *code = 259u; /* STILL_ACTIVE */
    return 1;
}

/* GetDiskFreeSpaceA(lpRootPath, lpSectorsPerCluster, lpBytesPerSector,
 * lpFreeClusters, lpTotalClusters) -> BOOL. Host `statvfs` with a fixed geometry
 * (512-byte sectors, 8 sectors/cluster = 4 KiB), free/total clusters from the real
 * filesystem. A Windows root ("C:\\", NULL) maps to the host cwd. */
uint32_t aret_GetDiskFreeSpaceA(uint32_t esp) {
#ifdef __wasm__
    /* wasm32-wasi has no filesystem statvfs. Report failure (BOOL FALSE) rather
     * than a fabricated geometry — sound: no silent wrong disk size. */
    (void)esp; return 0;
#else
    struct statvfs vfs;
    if (statvfs(".", &vfs) != 0) { return 0; }
    uint32_t bps = 512, spc = 8;                 /* 4 KiB cluster */
    uint64_t cluster = (uint64_t)bps * spc;
    uint64_t total = ((uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize) / cluster;
    uint64_t avail = ((uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize) / cluster;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull; /* 32-bit cluster count */
    if (avail > total) avail = total;
    uint32_t *p;
    if ((p = (uint32_t *)WP(1))) *p = spc;
    if ((p = (uint32_t *)WP(2))) *p = bps;
    if ((p = (uint32_t *)WP(3))) *p = (uint32_t)avail;
    if ((p = (uint32_t *)WP(4))) *p = (uint32_t)total;
    return 1;
#endif
}
uint32_t aret_GetCurrentThread(uint32_t esp)  { (void)esp; return 0xFFFFFFFEu; }
/* GetStartupInfoW: a console process with no inherited startup customization —
 * zero the STARTUPINFOW (68 bytes on 32-bit) and set cb; dwFlags stays 0. */
uint32_t aret_GetStartupInfoW(uint32_t esp) {
    uint8_t *si = (uint8_t *)(uintptr_t)WU(0);
    if (si) { memset(si, 0, 68); *(uint32_t *)si = 68; }
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
/* File byte-range locks (LockFile/LockFileEx and their Unlock counterparts):
 * grant unconditionally. We run a single translated process against its own
 * files, so there is no other locker to contend with; sqlite's Win32 VFS calls
 * LockFile before every read/write and reports "database is locked" if it
 * returns 0 (the weak stub's value), so a real success is required to write. */
uint32_t aret_LockFile(uint32_t esp)            { (void)esp; return 1; }
uint32_t aret_LockFileEx(uint32_t esp)          { (void)esp; return 1; }
uint32_t aret_UnlockFile(uint32_t esp)          { (void)esp; return 1; }
uint32_t aret_UnlockFileEx(uint32_t esp)        { (void)esp; return 1; }
uint32_t aret_SetHandleCount(uint32_t esp)      { return WU(0); }
/* GetFileType(handle): map the fd's kind to the Win32 constant so a CLI tool can
 * tell a TTY (CHAR) from a pipe/redirect (PIPE) from a file (DISK). */
uint32_t aret_GetFileType(uint32_t esp) {
    struct stat st;
    if (fstat((int)WU(0), &st) != 0) return 0; /* FILE_TYPE_UNKNOWN */
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return 3; /* FILE_TYPE_PIPE */
    if (S_ISCHR(st.st_mode)) return 2;                          /* FILE_TYPE_CHAR */
    return 1;                                                   /* FILE_TYPE_DISK */
}

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
/* Console code pages: a process with no console (output redirected to a pipe/
 * file, as under the differential harness) reports 0, like Windows/Wine; a real
 * console reports a code page (the US OEM default). */
uint32_t aret_GetConsoleCP(uint32_t esp) {
    (void)esp;
    return (isatty(0) || isatty(1) || isatty(2)) ? 437u : 0u;
}
uint32_t aret_GetConsoleOutputCP(uint32_t esp) { return aret_GetConsoleCP(esp); }
/* WriteConsoleW writes only to a real console; on a redirected handle it fails
 * (the caller then falls back to WriteFile) — match that so output isn't doubled. */
uint32_t aret_WriteConsoleW(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(1);
    uint32_t n = WU(2);
    char tmp[8192];
    uint32_t i = 0;
    for (; i < n && i < sizeof tmp; i++) tmp[i] = (char)(s[i] & 0xff);
    ssize_t w = write(fd, tmp, i);
    uint32_t *pw = (uint32_t *)(uintptr_t)WU(3);
    if (pw) *pw = w > 0 ? (uint32_t)w : 0;
    return w >= 0 ? 1 : 0;
}
uint32_t aret_GetConsoleMode(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console */
    if (WU(1)) *(uint32_t *)(uintptr_t)WU(1) = 0x3; /* ENABLE_PROCESSED|LINE_INPUT */
    return 1;
}
/* SetConsoleMode: on a real console Windows accepts the mode and returns TRUE; on
 * a redirected handle (pipe/file — every non-interactive run, incl. the winetest
 * harness and the differential harness) it fails and returns FALSE. We mirror
 * GetConsoleMode exactly (console iff isatty). We do not enforce the termios
 * changes a mode implies — consistent with GetConsoleMode reporting a fixed mode;
 * the measured/tested path is always the non-console FALSE branch. */
uint32_t aret_SetConsoleMode(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console -> FALSE */
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

/* ---- oleaut32 BSTR (contractual ABI, not an approximation) -----------------
 * A BSTR is a pointer to a NUL-terminated UTF-16 array preceded by a 4-byte
 * byte-length prefix: [u32 byteLen][wchar[len]][u16 0]. SysAllocString returns a
 * pointer PAST the prefix; SysStringLen/ByteLen read it back; SysFreeString frees
 * from the prefix. This layout is documented and fixed, so a thin native impl is
 * exact. */
uint32_t aret_SysAllocStringLen(uint32_t esp) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(0);
    uint32_t len = WU(1);                                   /* char count */
    uint32_t *base = (uint32_t *)malloc(4 + (size_t)(len + 1) * 2);
    if (!base) return 0;
    base[0] = len * 2;                                      /* byte length */
    uint16_t *s = (uint16_t *)(base + 1);
    for (uint32_t i = 0; i < len; i++) s[i] = src ? src[i] : 0;
    s[len] = 0;
    return (uint32_t)(uintptr_t)s;
}
uint32_t aret_SysAllocString(uint32_t esp) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(0);
    if (!src) return 0;
    uint32_t len = 0;
    while (src[len]) len++;
    uint32_t *base = (uint32_t *)malloc(4 + (size_t)(len + 1) * 2);
    if (!base) return 0;
    base[0] = len * 2;
    uint16_t *s = (uint16_t *)(base + 1);
    for (uint32_t i = 0; i < len; i++) s[i] = src[i];
    s[len] = 0;
    return (uint32_t)(uintptr_t)s;
}
uint32_t aret_SysFreeString(uint32_t esp) {
    uint32_t b = WU(0);
    if (b) free((void *)(uintptr_t)(b - 4));
    return 0;
}
uint32_t aret_SysStringLen(uint32_t esp) {
    uint32_t b = WU(0);
    return b ? *(uint32_t *)(uintptr_t)(b - 4) / 2 : 0;
}
uint32_t aret_SysStringByteLen(uint32_t esp) {
    uint32_t b = WU(0);
    return b ? *(uint32_t *)(uintptr_t)(b - 4) : 0;
}

/* ---- ole32 minimal (thin: init + task allocator, no object model) ----------
 * CoInitialize(Ex) returns S_OK on the first init of a thread and S_FALSE (1) on
 * a nested one — the contract callers check; CoUninitialize unwinds it. The task
 * allocator is malloc/free. Exact for code that only inits COM and (de)allocates
 * task memory. A real CoCreateInstance stays long-tail (needs an object model). */
static int aret_co_init_depth = 0;
uint32_t aret_CoInitialize(uint32_t esp)   { (void)esp; return aret_co_init_depth++ ? 1u : 0u; }
uint32_t aret_CoInitializeEx(uint32_t esp) { (void)esp; return aret_co_init_depth++ ? 1u : 0u; }
uint32_t aret_CoUninitialize(uint32_t esp) { (void)esp; if (aret_co_init_depth) aret_co_init_depth--; return 0; }
uint32_t aret_CoTaskMemAlloc(uint32_t esp)   { return (uint32_t)(uintptr_t)malloc((size_t)WU(0)); }
uint32_t aret_CoTaskMemRealloc(uint32_t esp) { return (uint32_t)(uintptr_t)realloc((void *)(uintptr_t)WU(0), (size_t)WU(1)); }
uint32_t aret_CoTaskMemFree(uint32_t esp)    { free((void *)(uintptr_t)WU(0)); return 0; }

/* ---- advapi32 minimal: the legacy CryptoAPI RNG path -----------------------
 * BusyBox-w32 (and much MSVC/mingw code) seeds its PRNG at startup via the
 * classic CryptAcquireContext + CryptGenRandom + CryptReleaseContext triple.
 * We model exactly what those callers use — a non-NULL provider token and a
 * buffer filled with random bytes — not a real cryptographic provider (no key
 * containers, no algorithms; a program needing real crypto strength needs a
 * real provider, which stays long-tail). Without this, BusyBox's first
 * applet-dispatch aborts ("applet not found") because RNG seeding fails at
 * startup, so *every* applet is unreachable.
 *
 * The bytes come from a self-contained xorshift seeded from host entropy — good
 * enough for seeding a CLI PRNG (this is not a security boundary). */
static uint64_t aret_rng_next(void) {
    static uint64_t s = 0;
    if (!s) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        s = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec)
          ^ ((uint64_t)(uintptr_t)&s << 16) ^ (uint64_t)getpid();
        if (!s) s = 0x9e3779b97f4a7c15ull;
    }
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

/* CryptAcquireContextA(phProv, container, provider, provtype, flags) -> BOOL.
 * Return a non-NULL pseudo-handle; the only contract callers check is success
 * plus a usable handle to pass on to CryptGenRandom/CryptReleaseContext. */
uint32_t aret_CryptAcquireContextA(uint32_t esp) {
    uint32_t *phProv = (uint32_t *)WP(0);
    if (phProv) *phProv = 0x43525950u; /* 'CRYP' — an opaque non-zero token */
    return 1;
}
/* CryptGenRandom(hProv, dwLen, pbBuffer) -> BOOL. Fill the buffer with bytes. */
uint32_t aret_CryptGenRandom(uint32_t esp) {
    uint32_t len = WU(1);
    unsigned char *buf = (unsigned char *)WP(2);
    if (!buf) return 0;
    for (uint32_t i = 0; i < len; i++) buf[i] = (unsigned char)(aret_rng_next() & 0xffu);
    return 1;
}
/* CryptReleaseContext(hProv, flags) -> BOOL. Nothing to release. */
uint32_t aret_CryptReleaseContext(uint32_t esp) { (void)esp; return 1; }

/* ================================================================== */
/* MessageBox — display-free sound fallback                            */
/* ================================================================== */
/* A modal message box needs a display to show and a user to dismiss. In the
 * display-free tier (no SDL yet) there is neither, so we return the same thing
 * the ground truth does with no display: Wine's MessageBoxA with DISPLAY unset
 * returns -1 (0xFFFFFFFF) immediately, without blocking — verified empirically.
 * That is the honest "no display available" answer (not a guessed button): a
 * program that ignores the result proceeds, one that checks it sees the same
 * failure as under headless Wine. A real dialog (SDL_ShowSimpleMessageBox) will
 * replace this when a visible display is available (G2b). */
uint32_t aret_MessageBoxA(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxW(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxExA(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxExW(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }

/* ================================================================== */
/* PE resources (.rsrc) — Find/Load/Sizeof/Lock/Free + LoadString      */
/* ================================================================== */
/* The PE headers and the .rsrc section are mapped at their image VAs (the
 * Memory Layout Mapper), so we walk the real IMAGE_RESOURCE_DIRECTORY tree in
 * place — no data is fabricated. Display-free and portable: this is pure data
 * indexing (LoadString, custom RCDATA, dialog/menu templates for later GDI/dialog
 * work). Sound: an absent resource returns NULL/0 (exactly as Win32), never a
 * guess. RT_* IDs: RT_STRING=6, RT_RCDATA=10, RT_VERSION=16, RT_DIALOG=5, … */
extern uint32_t aret_image_lo, aret_image_hi;

/* Locate the resource directory root in mapped memory: parse the PE headers at
 * the image base (aret_image_lo). Returns the root IMAGE_RESOURCE_DIRECTORY and,
 * via *out_base, the image base (leaf data RVAs are relative to it). NULL if the
 * image has no resource directory (or headers are unavailable). */
static const uint8_t *u32_rsrc_root(uint32_t *out_base) {
    uint32_t lo = aret_image_lo;
    if (!lo) return NULL;
    const uint8_t *img = (const uint8_t *)(uintptr_t)lo;
    if (img[0] != 'M' || img[1] != 'Z') return NULL;             /* DOS header */
    uint32_t e_lfanew = *(const uint32_t *)(img + 0x3C);
    if ((uint64_t)lo + e_lfanew + 0x100 > aret_image_hi) return NULL;
    const uint8_t *nt = img + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] || nt[3]) return NULL; /* "PE\0\0" */
    const uint8_t *opt = nt + 24;                                /* skip sig(4)+COFF(20) */
    if (*(const uint16_t *)opt != 0x010B) return NULL;           /* PE32 only (our target) */
    uint32_t rsrc_rva = *(const uint32_t *)(opt + 112);          /* DataDirectory[2].VirtualAddress */
    if (!rsrc_rva) return NULL;
    if (out_base) *out_base = lo;
    return img + rsrc_rva;
}

/* Case-insensitive compare of an ANSI name to a UTF-16 IMAGE_RESOURCE_DIR_STRING_U
 * (WORD length prefix + that many WCHARs, no NUL). Win32 resource names are ASCII
 * and matched case-insensitively. */
static int u32_rsrc_name_eq(const uint8_t *s, const char *name) {
    uint16_t slen = *(const uint16_t *)s;
    const uint8_t *ws = s + 2;
    uint16_t k = 0;
    for (; k < slen && name[k]; k++) {
        uint16_t wc = *(const uint16_t *)(ws + 2 * k);
        char a = name[k];
        char la = (a >= 'A' && a <= 'Z') ? a + 32 : a;
        char lb = ((char)wc >= 'A' && (char)wc <= 'Z') ? (char)wc + 32 : (char)wc;
        if (wc > 0xFF || la != lb) return 0;
    }
    return k == slen && name[k] == 0;
}

/* Find the entry in a resource directory `dir` (rsrc base `rb`) whose key matches
 * an integer id (name==NULL) or an ANSI name. Returns the entry's OffsetToData
 * field (high bit = subdirectory, else data-entry offset), or 0 if not found. */
static uint32_t u32_rsrc_entry(const uint8_t *rb, const uint8_t *dir, uint32_t id, const char *name) {
    uint16_t nnamed = *(const uint16_t *)(dir + 12);
    uint16_t nid = *(const uint16_t *)(dir + 14);
    const uint8_t *e = dir + 16;
    int total = (int)nnamed + (int)nid;
    for (int i = 0; i < total; i++, e += 8) {
        uint32_t nm = *(const uint32_t *)e;
        if (name) {
            if (!(nm & 0x80000000u)) continue;                   /* an ID, not a name */
            if (u32_rsrc_name_eq(rb + (nm & 0x7FFFFFFFu), name)) return *(const uint32_t *)(e + 4);
        } else {
            if (nm & 0x80000000u) continue;                      /* a name, not an ID */
            if (nm == id) return *(const uint32_t *)(e + 4);
        }
    }
    return 0;
}

/* A resource reference is either an ANSI string pointer or a MAKEINTRESOURCE id
 * (the whole value < 0x10000). Split it. */
static void u32_rsrc_ref(uint32_t ref, uint32_t *id, const char **name) {
    if (ref >= 0x10000u) { *name = (const char *)(uintptr_t)ref; *id = 0; }
    else                 { *name = NULL; *id = ref; }
}

/* Walk type -> name -> language(first) and return the IMAGE_RESOURCE_DATA_ENTRY,
 * or NULL. `type`/`name` are MAKEINTRESOURCE-or-string refs. */
static const uint8_t *u32_rsrc_data_entry(uint32_t type_ref, uint32_t name_ref) {
    uint32_t base;
    const uint8_t *rb = u32_rsrc_root(&base);
    if (!rb) return NULL;
    uint32_t tid, nid; const char *tname, *nname;
    u32_rsrc_ref(type_ref, &tid, &tname);
    u32_rsrc_ref(name_ref, &nid, &nname);
    uint32_t off = u32_rsrc_entry(rb, rb, tid, tname);           /* type level */
    if (!(off & 0x80000000u)) return NULL;
    off = u32_rsrc_entry(rb, rb + (off & 0x7FFFFFFFu), nid, nname); /* name level */
    if (!(off & 0x80000000u)) return NULL;
    const uint8_t *lang = rb + (off & 0x7FFFFFFFu);              /* language level */
    /* Take the first language entry (its OffsetToData points at the leaf). */
    uint16_t nnamed = *(const uint16_t *)(lang + 12), nidc = *(const uint16_t *)(lang + 14);
    if ((int)nnamed + (int)nidc < 1) return NULL;
    uint32_t leaf = *(const uint32_t *)(lang + 16 + 4);          /* first entry OffsetToData */
    if (leaf & 0x80000000u) return NULL;                        /* must be a data entry, not a dir */
    return rb + leaf;
}

/* FindResourceA(hModule, lpName, lpType) -> HRSRC. Returns the DATA_ENTRY pointer
 * as an opaque handle (LoadResource/SizeofResource take it back). */
uint32_t aret_FindResourceA(uint32_t esp) {
    const uint8_t *de = u32_rsrc_data_entry(WU(2) /* type */, WU(1) /* name */);
    return (uint32_t)(uintptr_t)de;
}
/* LoadResource(hModule, hResInfo) -> HGLOBAL. The bytes live at image_base+RVA. */
uint32_t aret_LoadResource(uint32_t esp) {
    const uint8_t *de = (const uint8_t *)(uintptr_t)WU(1);
    if (!de) return 0;
    return aret_image_lo + *(const uint32_t *)de;               /* OffsetToData is an image RVA */
}
/* LockResource(hResData) -> LPVOID. Already a pointer to the bytes. */
uint32_t aret_LockResource(uint32_t esp) { return WU(0); }
/* SizeofResource(hModule, hResInfo) -> DWORD (bytes). */
uint32_t aret_SizeofResource(uint32_t esp) {
    const uint8_t *de = (const uint8_t *)(uintptr_t)WU(1);
    return de ? *(const uint32_t *)(de + 4) : 0;                /* DATA_ENTRY.Size */
}
/* FreeResource(hResData) -> BOOL. A no-op in Win32 (returns FALSE = 0). */
uint32_t aret_FreeResource(uint32_t esp) { (void)esp; return 0; }

/* LoadStringA(hInstance, uID, lpBuffer, cchBufferMax) -> chars copied (excl NUL).
 * RT_STRING resources bundle 16 strings per block: block id = uID/16 + 1, index
 * = uID%16; each entry is a WORD length (WCHARs) + that many WCHARs (no NUL). */
uint32_t aret_LoadStringA(uint32_t esp) {
    uint32_t uID = WU(1);
    char *buf = (char *)WP(2);
    uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    const uint8_t *de = u32_rsrc_data_entry(6 /* RT_STRING */, uID / 16 + 1);
    if (!de) { buf[0] = 0; return 0; }
    const uint16_t *p = (const uint16_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
    uint32_t idx = uID % 16;
    for (uint32_t i = 0; i < idx; i++) p += 1 + *p;             /* skip to the target entry */
    uint16_t len = *p++;                                        /* WCHAR count */
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t i = 0; i < n; i++) buf[i] = (char)(p[i] & 0xFF);
    buf[n] = 0;
    return n;
}

/* ================================================================== */
/* USER32 — message-only window subsystem (no display, fully portable) */
/* ================================================================== */
/* A message-only window (HWND_MESSAGE) has no pixels: it is purely an internal
 * message sink bound to a WNDPROC. Tcl's notifier — and many console programs —
 * create one to drive an event loop. We model it soundly: a window-class
 * registry, a window table, a single (mono-thread) message FIFO, and timers.
 * Every dispatch is a real callback into the lifted WNDPROC via aret_call. No
 * X11, no GDI, no host threads: this stays standalone and WASM-portable. Real
 * *visible* multi-window GUI is a later milestone (SDL2-backed); this is the
 * message plumbing only. Mono-thread, matching the rest of the runtime model. */

#define U32_WM_QUIT   0x0012u
#define U32_WM_TIMER  0x0113u
#define U32_PM_REMOVE 0x0001u
#define U32_WM_PAINT         0x000Fu
#define U32_WM_ERASEBKGND    0x0014u
#define U32_WM_SETTEXT       0x000Cu
#define U32_WM_GETTEXT       0x000Du
#define U32_WM_GETTEXTLENGTH 0x000Eu

/* A pseudo-HWND for the desktop window (GetDesktopWindow). Kept well outside the
 * 1..U32_MAX_WIN handle range so it never aliases a real window. Its "rect" is the
 * virtual screen — a defined ARET desktop size (like the host-backed values of
 * GetDiskFreeSpace), not a guess: display-dependent raw values are tested by
 * invariant, never bit-compared to Wine (doc 72 §4.5). */
#define U32_DESKTOP  0x00010000u
#define U32_SCREEN_W 1024
#define U32_SCREEN_H 768

#define U32_MAX_CLASSES 64
static struct { uint16_t name[128]; uint32_t wndproc, hbr_bg; int used; } g_u32_class[U32_MAX_CLASSES];

#define U32_MAX_WIN 256
/* A window object. For message-only windows only wndproc/parent matter; a visible
 * (top-level) window also tracks its rect/style/text so the geometry & text APIs
 * (GetWindowRect/SetWindowPos/Set/GetWindowText/…) round-trip. The actual pixels
 * (an SDL_Window) come in G2b — this model layer is display-free and portable. */
static struct {
    uint32_t wndproc, parent;
    int used;
    int x, y, w, h;          /* window rect, screen coords */
    uint32_t style, exstyle;
    int visible, enabled;
    uint32_t userdata;       /* GWL_USERDATA */
    char title[256];         /* window text (ANSI) */
    int ctrl_id;             /* dialog control id (0 = not a control) */
    char classname[64];      /* registered class name (for GetClassName) */
    int needs_paint;         /* update region non-empty -> owes a WM_PAINT */
    int needs_erase;         /* update region marked for erase -> WM_ERASEBKGND */
    uint32_t bg_brush;       /* class hbrBackground (HBRUSH) for the default erase */
    int unicode;             /* created via a W API (IsWindowUnicode) */
    int check_state;         /* dialog button check state (BM_GETCHECK/CheckDlgButton) */
#ifdef ARET_HAVE_SDL
    void *sdl_win, *sdl_ren, *sdl_tex;  /* SDL window/renderer/streaming texture */
    uint32_t client_bmp;     /* HBITMAP of the client-area framebuffer (GDI draws here) */
    int cw, ch;              /* client framebuffer size (pixels) */
#endif
} g_u32_win[U32_MAX_WIN];

#ifdef ARET_HAVE_SDL
/* G2b window-presentation helpers (defined after the GDI object model, which they
 * use for the client framebuffer). Show creates the real SDL window on first
 * visibility; present blits the client framebuffer; pump drains SDL input events
 * into WM_* messages. All are no-ops when there is no usable display. */
static void sdl_window_show(int i);
static void sdl_window_present(int i);
static void sdl_window_destroy(int i);
static void sdl_pump(void);
static int  sdl_win_idx_from_id(uint32_t winid);
static int  sdl_any_window(void);
#endif

#define U32_MAX_MSG 8192
static struct { uint32_t hwnd, message, wParam, lParam, time, ptx, pty; } g_u32_q[U32_MAX_MSG];
static int g_u32_qh, g_u32_qt;          /* ring head/tail; empty when equal */

static int g_u32_quit;                  /* PostQuitMessage seen */
static int g_u32_quit_code;

#define U32_MAX_TIMER 64
static struct { uint32_t hwnd, id, elapse, proc; uint64_t due_ns; int used; } g_u32_timer[U32_MAX_TIMER];
static uint32_t g_u32_next_timer_id = 0xF000u; /* ids for hwnd==NULL SetTimer */

/* width-16 (Windows wchar_t) string helpers */
static int u32_weq(const uint16_t *a, const uint16_t *b) {
    for (int i = 0;; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
}
static void u32_wcpy(uint16_t *d, const uint16_t *s, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}
/* Widen a narrow (ANSI) class/window name to 16-bit — class names are ASCII in
 * practice, so a byte→u16 widening is exact. Lets the ANSI (A) window APIs share
 * the one wide class registry with the W APIs (Windows shares the atom table). */
static void u32_a2w(const char *s, uint16_t *d, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = (uint16_t)(unsigned char)s[i]; d[i] = 0;
}
/* Narrow a 16-bit (Windows wchar_t) string to ANSI — the reverse of u32_a2w. The
 * window model stores text as ANSI; the W text APIs narrow on the way in and widen
 * on the way out (exact for ASCII, which is what the content oracle exercises). */
static void u32_w2n(const uint16_t *s, char *d, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = (char)(s[i] & 0xFF); d[i] = 0;
}
/* Register a class (shared A/W core): store wndproc + background brush + wide
 * name, return the atom. */
static uint32_t u32_class_register(uint32_t wndproc, uint32_t hbr_bg, const uint16_t *wname) {
    for (int i = 0; i < U32_MAX_CLASSES; i++) {
        if (!g_u32_class[i].used) {
            g_u32_class[i].used = 1;
            g_u32_class[i].wndproc = wndproc;
            g_u32_class[i].hbr_bg = hbr_bg;
            u32_wcpy(g_u32_class[i].name, wname, 128);
            return 0xC000u + (uint32_t)i;
        }
    }
    return 0;
}
/* Resolve a class reference (atom or name pointer) to its background brush, or 0. */
static uint32_t u32_class_brush(uint32_t cref) {
    if (cref == 0) return 0;
    if (cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) return g_u32_class[idx].hbr_bg;
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) return g_u32_class[i].hbr_bg;
    return 0;
}
/* CW_USEDEFAULT: the caller leaves placement to the OS. We pick a fixed default so
 * geometry is deterministic; explicit coords (the common, oracle-tested case) pass
 * through unchanged. */
static int u32_coord(uint32_t v, int dflt) {
    return (v == 0x80000000u) ? dflt : (int)(int32_t)v;
}
/* A predefined USER32 control class (BUTTON/EDIT/…) has no app WNDPROC — the system
 * provides it. We model these as data-only control windows (state tracked, no
 * pixels): enough for GetDlgItem / Get-SetDlgItemText / CheckDlgButton to work on
 * a CreateWindowEx-created control, which real dialogs rely on. */
static int u32_is_ctrl_class(uint32_t cref, int wide) {
    if (cref < 0x10000u) return 0;                 /* atom = a registered class */
    char n[64];
    if (wide) u32_w2n((const uint16_t *)(uintptr_t)cref, n, sizeof n);
    else { const char *s = (const char *)(uintptr_t)cref; int k = 0; for (; s[k] && k < 63; k++) n[k] = s[k]; n[k] = 0; }
    static const char *const ctrls[] = { "button", "edit", "static", "listbox", "combobox",
        "scrollbar", "mdiclient", "richedit", "richedit20a", "richedit20w", "syslistview32",
        "systreeview32", "msctls_statusbar32", "msctls_updown32", "toolbarwindow32", "tooltips_class32", 0 };
    for (int i = 0; ctrls[i]; i++) {
        int eq = 1;
        for (int j = 0;; j++) {
            char a = n[j], b = ctrls[i][j];
            char la = (a >= 'A' && a <= 'Z') ? a + 32 : a;
            if (la != b) { eq = 0; break; }
            if (!a) break;
        }
        if (eq) return 1;
    }
    return 0;
}
/* Create a window (shared A/W core): bind wndproc + capture rect/style/text.
 * `is_ctrl` permits a data-only control window with no WNDPROC (predefined class). */
static uint32_t u32_window_create(uint32_t wndproc, uint32_t exstyle, uint32_t style,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  uint32_t parent, const char *title, int is_ctrl) {
    if (!wndproc && !is_ctrl) return 0;
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) {
            g_u32_win[i].used = 1;
            g_u32_win[i].wndproc = wndproc;
            g_u32_win[i].parent = parent;
            g_u32_win[i].exstyle = exstyle;
            g_u32_win[i].style = style;
            g_u32_win[i].x = u32_coord(x, 0);
            g_u32_win[i].y = u32_coord(y, 0);
            g_u32_win[i].w = u32_coord(w, 0);
            g_u32_win[i].h = u32_coord(h, 0);
            g_u32_win[i].visible = (style & 0x10000000u /* WS_VISIBLE */) ? 1 : 0;
            g_u32_win[i].enabled = (style & 0x08000000u /* WS_DISABLED */) ? 0 : 1;
            g_u32_win[i].userdata = 0;
            g_u32_win[i].ctrl_id = 0;
            g_u32_win[i].classname[0] = 0;
            g_u32_win[i].needs_paint = 0;
            g_u32_win[i].needs_erase = 0;
            g_u32_win[i].bg_brush = 0;
            int k = 0; if (title) for (; title[k] && k < 255; k++) g_u32_win[i].title[k] = title[k];
            g_u32_win[i].title[k] = 0;
            /* A visible top-level window (no parent, not a child control) starts
             * with its whole client invalid -> owes a WM_PAINT (erase included),
             * and gets a real SDL window. Child/message-only windows never do. */
            if (g_u32_win[i].visible && parent == 0 && !(style & 0x40000000u /* WS_CHILD */)) {
                g_u32_win[i].needs_paint = 1;
                g_u32_win[i].needs_erase = 1;
#ifdef ARET_HAVE_SDL
                sdl_window_show(i);
#endif
            }
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

/* Resolve a class reference (an ATOM from RegisterClass, or a wide-name pointer)
 * to its registered WNDPROC, or 0 if unknown. */
static uint32_t u32_class_wndproc(uint32_t cref) {
    if (cref == 0) return 0;
    if (cref < 0x10000u) {                 /* ATOM */
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) return g_u32_class[idx].wndproc;
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) return g_u32_class[i].wndproc;
    return 0;
}

static uint32_t u32_win_wndproc(uint32_t hwnd) {
    if (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) return g_u32_win[hwnd - 1].wndproc;
    return 0;
}

/* Call a lifted WNDPROC(hwnd,msg,wParam,lParam) — a stdcall callback into guest
 * code. Lay the frame just below the live machine esp (reentrant: a WNDPROC may
 * itself SendMessage), exactly like a real call: [esp+0]=retaddr, [esp+4..]=args. */
static uint32_t u32_call_wndproc(uint32_t esp, uint32_t wndproc,
                                 uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t frame = (esp - 64) & ~15u;
    uint32_t *f = (uint32_t *)(uintptr_t)frame;
    f[0] = 0; f[1] = hwnd; f[2] = msg; f[3] = wp; f[4] = lp;
    return (uint32_t)aret_call(wndproc, frame, 0, 0, 0, 0);
}

static int  u32_q_empty(void) { return g_u32_qh == g_u32_qt; }
static int  u32_q_push(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int nt = (g_u32_qt + 1) % U32_MAX_MSG;
    if (nt == g_u32_qh) return 0;          /* full */
    g_u32_q[g_u32_qt].hwnd = hwnd; g_u32_q[g_u32_qt].message = msg;
    g_u32_q[g_u32_qt].wParam = wp; g_u32_q[g_u32_qt].lParam = lp;
    g_u32_q[g_u32_qt].time = (uint32_t)(mono_ns() / 1000000ull);
    g_u32_q[g_u32_qt].ptx = 0; g_u32_q[g_u32_qt].pty = 0;
    g_u32_qt = nt; return 1;
}
/* Copy the queue head into a guest MSG (7 dwords), optionally removing it. */
static void u32_q_peek_copy(uint32_t *m, int remove) {
    if (m) {
        m[0] = g_u32_q[g_u32_qh].hwnd;   m[1] = g_u32_q[g_u32_qh].message;
        m[2] = g_u32_q[g_u32_qh].wParam; m[3] = g_u32_q[g_u32_qh].lParam;
        m[4] = g_u32_q[g_u32_qh].time;   m[5] = g_u32_q[g_u32_qh].ptx; m[6] = g_u32_q[g_u32_qh].pty;
    }
    if (remove) g_u32_qh = (g_u32_qh + 1) % U32_MAX_MSG;
}
static void u32_fill_quit(uint32_t *m) {
    if (!m) return;
    m[0] = 0; m[1] = U32_WM_QUIT; m[2] = (uint32_t)g_u32_quit_code; m[3] = 0;
    m[4] = (uint32_t)(mono_ns() / 1000000ull); m[5] = 0; m[6] = 0;
}
static void u32_fill_paint(uint32_t *m, int i) {
    if (!m) return;
    m[0] = (uint32_t)(i + 1); m[1] = U32_WM_PAINT; m[2] = 0; m[3] = 0;
    m[4] = (uint32_t)(mono_ns() / 1000000ull); m[5] = 0; m[6] = 0;
}
static int u32_any_timer(void) {
    for (int i = 0; i < U32_MAX_TIMER; i++) if (g_u32_timer[i].used) return 1;
    return 0;
}
/* Post a WM_TIMER for every timer whose due time has passed, and reschedule it.
 * Uses the real monotonic clock; a batch program that never loops never fires
 * one, so this stays deterministic in practice. */
static void u32_pump_timers(void) {
    uint64_t now = mono_ns();
    for (int i = 0; i < U32_MAX_TIMER; i++) {
        if (g_u32_timer[i].used && now >= g_u32_timer[i].due_ns) {
            u32_q_push(g_u32_timer[i].hwnd, U32_WM_TIMER, g_u32_timer[i].id, g_u32_timer[i].proc);
            g_u32_timer[i].due_ns = now + (uint64_t)g_u32_timer[i].elapse * 1000000ull;
        }
    }
}

/* --- Paint model (WM_PAINT / invalidation) ---------------------------------
 * A visible top-level window with a non-empty update region "owes" a WM_PAINT.
 * WM_PAINT is not queued: it is generated on demand (lowest priority, after the
 * posted queue and WM_QUIT) so a program's paint handler runs and draws — this is
 * what makes a visible window actually show content. Display-free & sound: the
 * message flow is correct Win32 behaviour whether or not a real screen (SDL) is
 * present. Coalesced to a whole-client dirty flag (WM_PAINT unions regions). */
static int u32_win_paints(int i) {
    return i >= 0 && i < U32_MAX_WIN && g_u32_win[i].used
        && g_u32_win[i].visible && g_u32_win[i].parent == 0;   /* excl. child/message-only */
}
/* The next visible window owing a WM_PAINT, or -1. */
static int u32_next_paint(void) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].needs_paint && u32_win_paints(i)) return i;
    return -1;
}
/* Fill a DC's whole surface with a brush colour (the default WM_ERASEBKGND). No
 * surface (display-free / null brush) -> sound no-op. Defined after the GDI model. */
static void u32_fill_dc_brush(uint32_t hdc, uint32_t brush);

/* RegisterClassW(const WNDCLASSW*) -> ATOM. Fields (32-bit): lpfnWndProc @+4,
 * hbrBackground @+28, lpszClassName @+36. Returns a non-zero atom; 0 on failure. */
uint32_t aret_RegisterClassW(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const uint16_t *name = (const uint16_t *)(uintptr_t)wc[9]; /* +36 lpszClassName */
    if (!name) return 0;
    return u32_class_register(wc[1] /* +4 lpfnWndProc */, wc[7] /* +28 hbrBackground */, name);
}
/* RegisterClassA(const WNDCLASSA*) — same 40-byte layout as WNDCLASSW but a narrow
 * class name; widen it and share the one registry. */
uint32_t aret_RegisterClassA(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const char *name = (const char *)(uintptr_t)wc[9];
    if (!name) return 0;
    uint16_t wname[128];
    u32_a2w(name, wname, 128);
    return u32_class_register(wc[1], wc[7], wname);
}
/* RegisterClassExW(const WNDCLASSEXW*) -> ATOM. WNDCLASSEX (32-bit) shifts every
 * field +4 vs WNDCLASS (cbSize @0): lpfnWndProc @+8, hbrBackground @+32,
 * lpszClassName @+40. Modern apps use this form. */
uint32_t aret_RegisterClassExW(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const uint16_t *name = (const uint16_t *)(uintptr_t)wc[10]; /* +40 lpszClassName */
    if (!name) return 0;
    return u32_class_register(wc[2] /* +8 lpfnWndProc */, wc[8] /* +32 hbrBackground */, name);
}
/* RegisterClassExA(const WNDCLASSEXA*) — narrow class name; widen and share. */
uint32_t aret_RegisterClassExA(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const char *name = (const char *)(uintptr_t)wc[10];
    if (!name) return 0;
    uint16_t wname[128];
    u32_a2w(name, wname, 128);
    return u32_class_register(wc[2], wc[8], wname);
}
/* UnregisterClassW(lpClassName, hInstance) -> BOOL. */
uint32_t aret_UnregisterClassW(uint32_t esp) {
    uint32_t cref = WU(0);
    if (cref && cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) { g_u32_class[idx].used = 0; return 1; }
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    if (!name) return 0;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) { g_u32_class[i].used = 0; return 1; }
    return 0;
}
/* CreateWindowExW(exStyle@0, className@1, winName@2, style@3, x@4,y@5,w@6,h@7,
 * parent@8, menu@9, inst@10, param@11) -> HWND. Binds the class WNDPROC and
 * captures geometry/style/text; visible windows get real pixels in G2b. */
/* Resolve a class reference (atom or name pointer) to its registered class name. */
static void u32_class_name(uint32_t cref, int wide, char *out, int cap) {
    out[0] = 0;
    if (cref == 0) return;
    if (cref < 0x10000u) {                           /* ATOM -> registry wide name */
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) u32_w2n(g_u32_class[idx].name, out, cap);
        return;
    }
    if (wide) u32_w2n((const uint16_t *)(uintptr_t)cref, out, cap);
    else { const char *s = (const char *)(uintptr_t)cref; int k = 0; for (; s[k] && k < cap - 1; k++) out[k] = s[k]; out[k] = 0; }
}
uint32_t aret_CreateWindowExW(uint32_t esp) {
    char title[256]; u32_w2n((const uint16_t *)WP(2), title, sizeof title);
    uint32_t h = u32_window_create(u32_class_wndproc(WU(1)), WU(0), WU(3),
                                   WU(4), WU(5), WU(6), WU(7), WU(8), title, u32_is_ctrl_class(WU(1), 1));
    if (h) { u32_class_name(WU(1), 1, g_u32_win[h - 1].classname, sizeof g_u32_win[h - 1].classname);
             g_u32_win[h - 1].bg_brush = u32_class_brush(WU(1));
             g_u32_win[h - 1].unicode = 1;
             if (WU(3) & 0x40000000u) g_u32_win[h - 1].ctrl_id = (int)WU(9);  /* WS_CHILD: hMenu=id */ }
    return h;
}
/* CreateWindowExA — className@1 is a narrow name (or an atom). Widen a name to
 * look it up in the shared registry; atoms (<0x10000) pass through unchanged. The
 * window name @2 is already narrow. */
uint32_t aret_CreateWindowExA(uint32_t esp) {
    uint32_t cref = WU(1);
    uint16_t wbuf[128];
    if (cref >= 0x10000u) {
        u32_a2w((const char *)(uintptr_t)cref, wbuf, 128);
        cref = (uint32_t)(uintptr_t)wbuf;
    }
    uint32_t h = u32_window_create(u32_class_wndproc(cref), WU(0), WU(3),
                                   WU(4), WU(5), WU(6), WU(7), WU(8), WCS(2), u32_is_ctrl_class(WU(1), 0));
    if (h) { u32_class_name(WU(1), 0, g_u32_win[h - 1].classname, sizeof g_u32_win[h - 1].classname);
             g_u32_win[h - 1].bg_brush = u32_class_brush(cref);
             if (WU(3) & 0x40000000u) g_u32_win[h - 1].ctrl_id = (int)WU(9);  /* WS_CHILD: hMenu=id */ }
    return h;
}
/* DestroyWindow(HWND) -> BOOL. */
uint32_t aret_DestroyWindow(uint32_t esp) {
    uint32_t h = WU(0);
    if (h >= 1 && h <= U32_MAX_WIN && g_u32_win[h - 1].used) {
#ifdef ARET_HAVE_SDL
        sdl_window_destroy((int)h - 1);
#endif
        g_u32_win[h - 1].used = 0;
    }
    return 1;
}
/* Default handling for the window-text messages (WM_SETTEXT/GETTEXT/GETTEXTLENGTH),
 * shared by DefWindowProcA (narrow) and DefWindowProcW (wide). Returns 1 and sets
 * *out if it handled the message, 0 to fall through. This is the real Windows path:
 * Set/GetWindowText send these messages, and DefWindowProc is what stores/reports
 * the text — so a subclass that intercepts them behaves exactly as under Wine. */
static int u32_defproc_text(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp,
                            int wide, uint32_t *out) {
    int i = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    switch (msg) {
    case U32_WM_SETTEXT:
        if (i < 0) { *out = 0; return 1; }
        if (wide) u32_w2n((const uint16_t *)(uintptr_t)lp, g_u32_win[i].title, sizeof g_u32_win[i].title);
        else { const char *s = (const char *)(uintptr_t)lp; int k = 0;
               if (s) for (; s[k] && k < 255; k++) g_u32_win[i].title[k] = s[k];
               g_u32_win[i].title[k] = 0; }
        *out = 1; return 1;                 /* TRUE */
    case U32_WM_GETTEXTLENGTH:
        *out = (i < 0) ? 0 : (uint32_t)strlen(g_u32_win[i].title); return 1;
    case U32_WM_GETTEXT: {
        uint32_t n = wp;                    /* buffer capacity in chars, incl. NUL */
        if (i < 0 || n == 0 || lp == 0) { *out = 0; return 1; }
        const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
        uint32_t cc = len < n - 1 ? len : n - 1;
        if (wide) { uint16_t *d = (uint16_t *)(uintptr_t)lp;
                    for (uint32_t j = 0; j < cc; j++) d[j] = (uint16_t)(unsigned char)t[j]; d[cc] = 0; }
        else      { char *d = (char *)(uintptr_t)lp;
                    for (uint32_t j = 0; j < cc; j++) d[j] = t[j]; d[cc] = 0; }
        *out = cc; return 1;
    }
    default: return 0;
    }
}
/* DefWindowProc handling common to A/W, independent of text width: WM_PAINT does
 * a default paint (validates the update region so a program that delegates
 * painting doesn't loop forever on WM_PAINT); WM_CLOSE destroys the window after
 * a WM_DESTROY, so a real close (the SDL window's X button) actually closes. Both
 * fire only on generated/real events, never in the deterministic headless oracle.
 * Returns 1 if handled, with the result in *out. */
static int u32_defproc_common(uint32_t esp, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t *out) {
    int i = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    if (msg == U32_WM_PAINT) { if (i >= 0) g_u32_win[i].needs_paint = 0; *out = 0; return 1; }
    if (msg == U32_WM_ERASEBKGND) {   /* default erase: fill client with class brush */
        if (i >= 0 && g_u32_win[i].bg_brush) u32_fill_dc_brush(wp /* HDC */, g_u32_win[i].bg_brush);
        *out = 1; return 1;           /* TRUE = background erased */
    }
    if (msg == 0x0010u /* WM_CLOSE */) {
        if (i >= 0) {
            uint32_t wp = g_u32_win[i].wndproc;
            if (wp) u32_call_wndproc(esp, wp, hwnd, 0x0002u /* WM_DESTROY */, 0, 0);
#ifdef ARET_HAVE_SDL
            sdl_window_destroy(i);
#endif
            g_u32_win[i].used = 0;
        }
        *out = 0; return 1;
    }
    return 0;
}
/* DefWindowProcW(HWND,UINT,WPARAM,LPARAM) -> LRESULT. Handles the text messages
 * (wide) + the common paint/close defaults; everything else defaults to 0. */
uint32_t aret_DefWindowProcW(uint32_t esp) {
    uint32_t r;
    if (u32_defproc_common(esp, WU(0), WU(1), WU(2), &r)) return r;
    if (u32_defproc_text(WU(0), WU(1), WU(2), WU(3), 1, &r)) return r;
    return 0;
}
/* PostMessageW(HWND,UINT,WPARAM,LPARAM) -> BOOL. Enqueue. */
uint32_t aret_PostMessageW(uint32_t esp) { return (uint32_t)u32_q_push(WU(0), WU(1), WU(2), WU(3)); }
/* SendMessageW(HWND,UINT,WPARAM,LPARAM) -> LRESULT. Synchronous: call the WNDPROC now. */
uint32_t aret_SendMessageW(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) return 0;
    return u32_call_wndproc(esp, wndproc, WU(0), WU(1), WU(2), WU(3));
}
/* DispatchMessageW(const MSG*) -> LRESULT. Route to the window's WNDPROC (or the
 * TIMERPROC in lParam for a WM_TIMER carrying one). */
uint32_t aret_DispatchMessageW(uint32_t esp) {
    const uint32_t *m = (const uint32_t *)WP(0);
    if (!m) return 0;
    uint32_t hwnd = m[0], msg = m[1], wp = m[2], lp = m[3];
    if (msg == U32_WM_TIMER && lp)          /* TIMERPROC callback */
        return u32_call_wndproc(esp, lp, hwnd, msg, wp, (uint32_t)(mono_ns() / 1000000ull));
    uint32_t wndproc = u32_win_wndproc(hwnd);
    if (!wndproc) return 0;
    return u32_call_wndproc(esp, wndproc, hwnd, msg, wp, lp);
}
/* TranslateMessage(const MSG*) -> BOOL. No keyboard input in this model -> no
 * WM_CHAR synthesis; returns 0 (nothing translated), which is correct here. */
uint32_t aret_TranslateMessage(uint32_t esp) { (void)esp; return 0; }
/* PeekMessageW(lpMsg, hwnd, min, max, wRemoveMsg) -> BOOL. Non-blocking. */
uint32_t aret_PeekMessageW(uint32_t esp) {
    uint32_t *m = (uint32_t *)WP(0);
    uint32_t remove = WU(4);
#ifdef ARET_HAVE_SDL
    sdl_pump();          /* drain real keyboard/mouse/close events into the queue */
#endif
    u32_pump_timers();
    if (!u32_q_empty()) { u32_q_peek_copy(m, (remove & U32_PM_REMOVE) != 0); return 1; }
    if (g_u32_quit)    { u32_fill_quit(m); return 1; }
    /* WM_PAINT is generated on demand (never queued), lowest priority. Peeking
     * does not clear the update region — BeginPaint/ValidateRect do. */
    { int pi = u32_next_paint(); if (pi >= 0) { u32_fill_paint(m, pi); return 1; } }
    return 0;
}
/* GetMessageW(lpMsg, hwnd, min, max) -> BOOL (0 = WM_QUIT, else 1). Blocks until a
 * message is available. In the mono-thread model the only wakers are the queue
 * (already posted) and timers, so if the queue is empty with no quit and no timer
 * nothing can ever arrive -> we abort loudly rather than hang or fake a quit. */
uint32_t aret_GetMessageW(uint32_t esp) {
    uint32_t *m = (uint32_t *)WP(0);
    for (;;) {
#ifdef ARET_HAVE_SDL
        sdl_pump();      /* drain real keyboard/mouse/close events into the queue */
#endif
        u32_pump_timers();
        if (!u32_q_empty()) { u32_q_peek_copy(m, 1); return 1; }
        if (g_u32_quit)    { u32_fill_quit(m); return 0; }
        /* WM_PAINT: generated on demand, after the posted queue and WM_QUIT. */
        { int pi = u32_next_paint(); if (pi >= 0) { u32_fill_paint(m, pi); return 1; } }
#ifdef ARET_HAVE_SDL
        /* A real visible window is a message source (its close button, input): the
         * queue can wake even with no timer, so block on SDL events instead of
         * aborting. When no window is shown either, fall through to the honest
         * mono-thread abort below. */
        if (sdl_any_window()) { usleep(2000); continue; }
#endif
        if (!u32_any_timer())
            aret_unimpl("GetMessageW: empty queue, no WM_QUIT, no timer (would block forever in mono-thread model)");
        usleep(1000);                        /* wait for the next timer to come due */
    }
}
/* SetTimer(hwnd, nIDEvent, uElapse_ms, TIMERPROC) -> UINT_PTR. */
uint32_t aret_SetTimer(uint32_t esp) {
    uint32_t hwnd = WU(0), id = WU(1), elapse = WU(2), proc = WU(3);
    if (hwnd == 0 && id == 0) id = g_u32_next_timer_id++;   /* window-less timer gets a fresh id */
    int slot = -1;
    for (int i = 0; i < U32_MAX_TIMER; i++) {               /* reuse an existing (hwnd,id) */
        if (g_u32_timer[i].used && g_u32_timer[i].hwnd == hwnd && g_u32_timer[i].id == id) { slot = i; break; }
        if (slot < 0 && !g_u32_timer[i].used) slot = i;
    }
    if (slot < 0) return 0;
    g_u32_timer[slot].used = 1; g_u32_timer[slot].hwnd = hwnd; g_u32_timer[slot].id = id;
    g_u32_timer[slot].elapse = elapse; g_u32_timer[slot].proc = proc;
    g_u32_timer[slot].due_ns = mono_ns() + (uint64_t)elapse * 1000000ull;
    return id;
}
/* KillTimer(hwnd, id) -> BOOL. */
uint32_t aret_KillTimer(uint32_t esp) {
    uint32_t hwnd = WU(0), id = WU(1);
    for (int i = 0; i < U32_MAX_TIMER; i++)
        if (g_u32_timer[i].used && g_u32_timer[i].hwnd == hwnd && g_u32_timer[i].id == id) { g_u32_timer[i].used = 0; return 1; }
    return 0;
}
/* PostQuitMessage(nExitCode) -> void. */
uint32_t aret_PostQuitMessage(uint32_t esp) { g_u32_quit = 1; g_u32_quit_code = WI(0); return 0; }
/* MsgWaitForMultipleObjectsEx(nCount, pHandles, dwMs, dwWakeMask, dwFlags) -> DWORD.
 * A message available -> WAIT_OBJECT_0 + nCount. We do not model signaled guest
 * handles here, so otherwise report WAIT_TIMEOUT (0x102). */
uint32_t aret_MsgWaitForMultipleObjectsEx(uint32_t esp) {
    uint32_t nCount = WU(0);
    u32_pump_timers();
    if (!u32_q_empty() || g_u32_quit) return nCount;   /* WAIT_OBJECT_0 (=0) + nCount */
    return 0x00000102u;                                /* WAIT_TIMEOUT */
}

/* --- ANSI (A) twins. The message ops carry no text at this layer (message-only
 * windows have no keyboard/WM_CHAR translation), so A is byte-identical to W;
 * only the class-name-bearing RegisterClassA/CreateWindowExA and the text-bearing
 * DefWindowProcA/Set-GetWindowTextA differ (ANSI vs wide payloads). --- */
uint32_t aret_DefWindowProcA(uint32_t esp) {
    uint32_t r;
    if (u32_defproc_common(esp, WU(0), WU(1), WU(2), &r)) return r;
    if (u32_defproc_text(WU(0), WU(1), WU(2), WU(3), 0, &r)) return r;  /* narrow */
    return 0;
}
uint32_t aret_GetMessageA(uint32_t esp)      { return aret_GetMessageW(esp); }
uint32_t aret_PeekMessageA(uint32_t esp)     { return aret_PeekMessageW(esp); }
uint32_t aret_DispatchMessageA(uint32_t esp) { return aret_DispatchMessageW(esp); }
uint32_t aret_PostMessageA(uint32_t esp)     { return aret_PostMessageW(esp); }
uint32_t aret_SendMessageA(uint32_t esp)     { return aret_SendMessageW(esp); }
/* UnregisterClassA(lpClassName, hInstance) — widen a narrow name, else atom. */
uint32_t aret_UnregisterClassA(uint32_t esp) {
    uint32_t cref = WU(0);
    if (cref && cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) { g_u32_class[idx].used = 0; return 1; }
        return 0;
    }
    if (!cref) return 0;
    uint16_t wname[128];
    u32_a2w((const char *)(uintptr_t)cref, wname, 128);
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, wname)) { g_u32_class[i].used = 0; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window geometry / state / text (G2a, display-free)                 */
/*                                                                    */
/* This models the window manager's *state* (rect, style, text, show/ */
/* enable) so the geometry & text APIs round-trip against Wine without */
/* a real screen. Actual pixels (SDL_Window) are G2b. Geometry APIs   */
/* read/write the window struct directly (as Windows' manager does);  */
/* text APIs go through WM_SETTEXT/GETTEXT so subclasses see them.     */
/* ------------------------------------------------------------------ */

/* Valid live-window index for hwnd, or -1. */
static int u32_win_idx(uint32_t hwnd) {
    return (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
}

/* GetWindowRect(HWND, RECT*) -> BOOL. Screen coords {left,top,right,bottom} =
 * {x, y, x+w, y+h} (Wine returns exactly the CreateWindow geometry). The desktop
 * pseudo-window reports the virtual screen. */
uint32_t aret_GetWindowRect(uint32_t esp) {
    uint32_t hwnd = WU(0); int32_t *r = (int32_t *)WP(1);
    if (!r) return 0;
    if (hwnd == U32_DESKTOP) { r[0] = 0; r[1] = 0; r[2] = U32_SCREEN_W; r[3] = U32_SCREEN_H; return 1; }
    int i = u32_win_idx(hwnd);
    if (i < 0) return 0;
    r[0] = g_u32_win[i].x; r[1] = g_u32_win[i].y;
    r[2] = g_u32_win[i].x + g_u32_win[i].w; r[3] = g_u32_win[i].y + g_u32_win[i].h;
    return 1;
}
/* SetWindowPos(HWND, hwndInsertAfter, X, Y, cx, cy, uFlags) -> BOOL. Honours
 * SWP_NOMOVE/NOSIZE and SWP_SHOW/HIDEWINDOW; Z-order is a no-op in this model. */
uint32_t aret_SetWindowPos(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    uint32_t f = WU(6);
    if (!(f & 0x0002u)) { g_u32_win[i].x = WI(2); g_u32_win[i].y = WI(3); }  /* !SWP_NOMOVE */
    if (!(f & 0x0001u)) { g_u32_win[i].w = WI(4); g_u32_win[i].h = WI(5); }  /* !SWP_NOSIZE */
    if (f & 0x0040u) g_u32_win[i].visible = 1;                               /* SWP_SHOWWINDOW */
    if (f & 0x0080u) g_u32_win[i].visible = 0;                               /* SWP_HIDEWINDOW */
    return 1;
}
/* MoveWindow(HWND, X, Y, nWidth, nHeight, bRepaint) -> BOOL. */
uint32_t aret_MoveWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    g_u32_win[i].x = WI(1); g_u32_win[i].y = WI(2);
    g_u32_win[i].w = WI(3); g_u32_win[i].h = WI(4);
    return 1;
}
/* ShowWindow(HWND, nCmdShow) -> BOOL (nonzero = was previously visible). SW_HIDE=0
 * hides; any other command shows (minimise/maximise are still "visible" here). */
uint32_t aret_ShowWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    int was = g_u32_win[i].visible;
    g_u32_win[i].visible = (WU(1) == 0 /* SW_HIDE */) ? 0 : 1;
    /* Becoming visible invalidates the whole window -> owes a WM_PAINT (erase). */
    if (g_u32_win[i].visible && !was && g_u32_win[i].parent == 0
        && !(g_u32_win[i].style & 0x40000000u /* WS_CHILD */)) {
        g_u32_win[i].needs_paint = 1;
        g_u32_win[i].needs_erase = 1;
    }
#ifdef ARET_HAVE_SDL
    if (g_u32_win[i].visible && g_u32_win[i].parent == 0 && !(g_u32_win[i].style & 0x40000000u))
        sdl_window_show(i);
    else if (!g_u32_win[i].visible && g_u32_win[i].sdl_win)
        SDL_HideWindow((SDL_Window *)g_u32_win[i].sdl_win);
#endif
    return (uint32_t)was;
}
/* UpdateWindow(HWND) -> BOOL. No invalid region is tracked yet (GDI paint is G6),
 * so there is nothing to repaint; report success for a valid window. */
uint32_t aret_UpdateWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    /* UpdateWindow paints immediately if the update region is non-empty: send
     * WM_PAINT to the WNDPROC now (it Begin/EndPaints, which draws & validates). */
    if (i >= 0 && g_u32_win[i].needs_paint && u32_win_paints(i)) {
        uint32_t wp = g_u32_win[i].wndproc;
        if (wp) u32_call_wndproc(esp, wp, (uint32_t)(i + 1), U32_WM_PAINT, 0, 0);
    }
#ifdef ARET_HAVE_SDL
    if (i >= 0) sdl_window_present(i);   /* flush the client framebuffer to screen */
#endif
    return i >= 0 ? 1u : 0u;
}
/* EnableWindow(HWND, bEnable) -> BOOL (nonzero = was previously DISABLED). */
uint32_t aret_EnableWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    int was_disabled = !g_u32_win[i].enabled;
    g_u32_win[i].enabled = WU(1) ? 1 : 0;
    return (uint32_t)was_disabled;
}
/* GetParent(HWND) -> HWND. Returns the stored parent/owner (0 for a top-level). */
uint32_t aret_GetParent(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return i < 0 ? 0 : g_u32_win[i].parent;
}
/* GetDesktopWindow() -> HWND: the desktop pseudo-window. */
uint32_t aret_GetDesktopWindow(uint32_t esp) { (void)esp; return U32_DESKTOP; }
/* IsWindow(HWND) -> BOOL. */
uint32_t aret_IsWindow(uint32_t esp) {
    uint32_t h = WU(0);
    return (h == U32_DESKTOP || u32_win_idx(h) >= 0) ? 1u : 0u;
}
/* IsWindowVisible(HWND) -> BOOL. */
uint32_t aret_IsWindowVisible(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].visible) ? 1u : 0u;
}
/* IsWindowEnabled(HWND) -> BOOL. */
uint32_t aret_IsWindowEnabled(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].enabled) ? 1u : 0u;
}
/* IsIconic(HWND) -> BOOL. Windows are never minimised in this model. */
uint32_t aret_IsIconic(uint32_t esp) { (void)esp; return 0; }
/* GetWindowLongA(HWND, nIndex) -> LONG. Models the fields we store; other indices
 * (GWL_HINSTANCE/ID we never set) are 0, which is their value for these windows. */
uint32_t aret_GetWindowLongA(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    switch (WI(1)) {
    case -16: return g_u32_win[i].style;     /* GWL_STYLE */
    case -20: return g_u32_win[i].exstyle;   /* GWL_EXSTYLE */
    case -21: return g_u32_win[i].userdata;  /* GWL_USERDATA */
    case -4:  return g_u32_win[i].wndproc;   /* GWL_WNDPROC */
    case -8:  return g_u32_win[i].parent;    /* GWL_HWNDPARENT */
    default:  return 0;
    }
}
uint32_t aret_GetWindowLongW(uint32_t esp) { return aret_GetWindowLongA(esp); }
/* SetWindowLongA(HWND, nIndex, dwNewLong) -> LONG (previous value). GWL_WNDPROC
 * assignment is real subclassing (later messages dispatch to the new proc). */
uint32_t aret_SetWindowLongA(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    uint32_t v = WU(2), old;
    switch (WI(1)) {
    case -16: old = g_u32_win[i].style;    g_u32_win[i].style = v;    return old;
    case -20: old = g_u32_win[i].exstyle;  g_u32_win[i].exstyle = v;  return old;
    case -21: old = g_u32_win[i].userdata; g_u32_win[i].userdata = v; return old;
    case -4:  old = g_u32_win[i].wndproc;  g_u32_win[i].wndproc = v;  return old;
    default:  return 0;
    }
}
uint32_t aret_SetWindowLongW(uint32_t esp) { return aret_SetWindowLongA(esp); }
/* IsWindowUnicode(HWND) -> BOOL. A window created through a W API is Unicode. */
uint32_t aret_IsWindowUnicode(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].unicode) ? 1u : 0u;
}
/* RegisterWindowMessageA/W(lpString) -> UINT. A process-unique message id in the
 * [0xC000, 0xFFFF] range, identical for equal strings (like a global atom). */
#define U32_MAX_RWM 128
static struct { char name[128]; uint32_t id; int used; } g_u32_rwm[U32_MAX_RWM];
static uint32_t g_u32_rwm_next = 0xC000u;
static uint32_t u32_reg_win_msg(const char *s) {
    if (!s || !s[0]) return 0;
    for (int i = 0; i < U32_MAX_RWM; i++)
        if (g_u32_rwm[i].used && strncmp(g_u32_rwm[i].name, s, sizeof g_u32_rwm[i].name) == 0) return g_u32_rwm[i].id;
    for (int i = 0; i < U32_MAX_RWM; i++)
        if (!g_u32_rwm[i].used) {
            g_u32_rwm[i].used = 1;
            int k = 0; for (; s[k] && k < 127; k++) g_u32_rwm[i].name[k] = s[k]; g_u32_rwm[i].name[k] = 0;
            g_u32_rwm[i].id = g_u32_rwm_next++;
            return g_u32_rwm[i].id;
        }
    return 0;
}
uint32_t aret_RegisterWindowMessageA(uint32_t esp) { return u32_reg_win_msg(WCS(0)); }
uint32_t aret_RegisterWindowMessageW(uint32_t esp) {
    char buf[128]; u32_w2n((const uint16_t *)WP(0), buf, sizeof buf);
    return u32_reg_win_msg(buf);
}
/* ExitWindowsEx(uFlags, dwReason) -> BOOL. We never log the user off / shut the
 * host down (sound: a transpiled app must not affect the real session); report
 * success so the app proceeds to its own teardown. Not oracle-compared (a real
 * Wine call would actually try to end the session). */
uint32_t aret_ExitWindowsEx(uint32_t esp) { (void)esp; return 1; }
/* MapWindowPoints(hwndFrom, hwndTo, lpPoints, cPoints) -> DWORD. Translate points
 * between two windows' client spaces. Client origin (no non-client frame modelled)
 * = the window's screen position; a NULL window is the screen (origin 0,0). */
uint32_t aret_MapWindowPoints(uint32_t esp) {
    int32_t *pt = (int32_t *)WP(2);
    uint32_t n = WU(3);
    int fi = u32_win_idx(WU(0)), ti = u32_win_idx(WU(1));
    int fx = fi >= 0 ? g_u32_win[fi].x : 0, fy = fi >= 0 ? g_u32_win[fi].y : 0;
    int tx = ti >= 0 ? g_u32_win[ti].x : 0, ty = ti >= 0 ? g_u32_win[ti].y : 0;
    int dx = fx - tx, dy = fy - ty;
    if (pt) for (uint32_t k = 0; k < n; k++) { pt[k * 2] += dx; pt[k * 2 + 1] += dy; }
    return ((uint32_t)(dy & 0xFFFF) << 16) | (uint32_t)(dx & 0xFFFF);
}
/* CheckDlgButton(hDlg, nIDButton, uCheck) -> BOOL; IsDlgButtonChecked(hDlg,id) ->
 * UINT. The check state lives on the child control window. */
static int u32_dlg_ctrl(uint32_t hdlg, int id) {
    int d = u32_win_idx(hdlg); if (d < 0) return -1;
    uint32_t dh = (uint32_t)(d + 1);
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].parent == dh && g_u32_win[i].ctrl_id == id) return i;
    return -1;
}
uint32_t aret_CheckDlgButton(uint32_t esp) {
    int c = u32_dlg_ctrl(WU(0), WI(1)); if (c < 0) return 0;
    g_u32_win[c].check_state = (int)WU(2);   /* BST_UNCHECKED/CHECKED/INDETERMINATE */
    return 1;
}
uint32_t aret_IsDlgButtonChecked(uint32_t esp) {
    int c = u32_dlg_ctrl(WU(0), WI(1)); return c < 0 ? 0 : (uint32_t)g_u32_win[c].check_state;
}
/* RedrawWindow(hwnd, lprcUpdate, hrgn, flags) -> BOOL. Fold into the paint model:
 * RDW_INVALIDATE marks a WM_PAINT owed, RDW_VALIDATE clears it, RDW_UPDATENOW
 * delivers it now. */
uint32_t aret_RedrawWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    uint32_t flags = WU(3);
    if ((flags & 0x0001u) && u32_win_paints(i)) {          /* RDW_INVALIDATE */
        g_u32_win[i].needs_paint = 1;
        if (flags & 0x0004u) g_u32_win[i].needs_erase = 1; /* RDW_ERASE */
    }
    if (flags & 0x0008u) g_u32_win[i].needs_paint = 0;     /* RDW_VALIDATE */
    if ((flags & 0x0100u) && g_u32_win[i].needs_paint && u32_win_paints(i)) { /* RDW_UPDATENOW */
        uint32_t wp = g_u32_win[i].wndproc;
        if (wp) u32_call_wndproc(esp, wp, (uint32_t)(i + 1), U32_WM_PAINT, 0, 0);
    }
    return 1;
}
/* Deferred window positioning. We apply each move immediately (the final window
 * state is identical to a batched apply; only repaint atomicity — cosmetic —
 * differs), and use the count as an opaque non-zero HDWP. */
/* Window property list: SetProp/GetProp/RemovePropA/W(hwnd, lpString, [hData]).
 * A per-(window,key) value store — used by subclassing/wrapping code to hang
 * per-window data off an HWND. */
#define U32_MAX_PROP 256
static struct { uint32_t hwnd; char key[64]; uint32_t val; int used; } g_u32_prop[U32_MAX_PROP];
static int u32_prop_find(uint32_t hwnd, const char *key) {
    for (int i = 0; i < U32_MAX_PROP; i++)
        if (g_u32_prop[i].used && g_u32_prop[i].hwnd == hwnd && strncmp(g_u32_prop[i].key, key, 64) == 0) return i;
    return -1;
}
static uint32_t u32_set_prop(uint32_t hwnd, const char *key, uint32_t val) {
    if (!key) return 0;
    int i = u32_prop_find(hwnd, key);
    if (i < 0) for (i = 0; i < U32_MAX_PROP; i++) if (!g_u32_prop[i].used) break;
    if (i >= U32_MAX_PROP) return 0;
    g_u32_prop[i].used = 1; g_u32_prop[i].hwnd = hwnd;
    int k = 0; for (; key[k] && k < 63; k++) g_u32_prop[i].key[k] = key[k]; g_u32_prop[i].key[k] = 0;
    g_u32_prop[i].val = val; return 1;
}
static uint32_t u32_get_prop(uint32_t hwnd, const char *key) {
    if (!key) return 0; int i = u32_prop_find(hwnd, key);
    return i < 0 ? 0 : g_u32_prop[i].val;
}
static uint32_t u32_rm_prop(uint32_t hwnd, const char *key) {
    if (!key) return 0; int i = u32_prop_find(hwnd, key);
    if (i < 0) return 0; uint32_t v = g_u32_prop[i].val; g_u32_prop[i].used = 0; return v;
}
uint32_t aret_SetPropA(uint32_t esp) { return u32_set_prop(WU(0), WCS(1), WU(2)); }
uint32_t aret_GetPropA(uint32_t esp) { return u32_get_prop(WU(0), WCS(1)); }
uint32_t aret_RemovePropA(uint32_t esp) { return u32_rm_prop(WU(0), WCS(1)); }
uint32_t aret_SetPropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_set_prop(WU(0), k, WU(2)); }
uint32_t aret_GetPropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_get_prop(WU(0), k); }
uint32_t aret_RemovePropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_rm_prop(WU(0), k); }
/* GetOpenFileNameA/GetSaveFileNameA(OPENFILENAME*) -> BOOL. No file chooser is
 * shown (display-free): report cancellation (FALSE), the sound "user picked
 * nothing" outcome — never a guessed filename. */
uint32_t aret_GetOpenFileNameA(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_GetSaveFileNameA(uint32_t esp) { (void)esp; return 0; }
/* WinExec(lpCmdLine, uCmdShow) -> UINT. Launching a child process has no host
 * Windows to run it (same hard boundary as CreateProcess) -> ERROR_FILE_NOT_FOUND
 * (2), a value < 32 meaning failure. Sound: never pretends to have launched. */
uint32_t aret_WinExec(uint32_t esp) { (void)esp; return 2; }
/* GetVolumeInformationA(root, volNameBuf, volNameSz, serial, maxComp, fsFlags,
 * fsNameBuf, fsNameSz) -> BOOL. A defined ARET volume (invariant, like
 * GetDiskFreeSpace): env-dependent raw values are tested by invariant, not
 * bit-compared to Wine. */
uint32_t aret_GetVolumeInformationA(uint32_t esp) {
    char *vn = (char *)WP(1); uint32_t vns = WU(2);
    uint32_t *serial = (uint32_t *)WP(3), *maxc = (uint32_t *)WP(4), *flags = (uint32_t *)WP(5);
    char *fsn = (char *)WP(6); uint32_t fsns = WU(7);
    if (vn && vns) { const char *s = "ARET"; uint32_t n = 0; for (; s[n] && n < vns - 1; n++) vn[n] = s[n]; vn[n] = 0; }
    if (serial) *serial = 0x41524554u;
    if (maxc) *maxc = 255;
    if (flags) *flags = 0x00000002u;          /* FS_CASE_IS_PRESERVED */
    if (fsn && fsns) { const char *s = "NTFS"; uint32_t n = 0; for (; s[n] && n < fsns - 1; n++) fsn[n] = s[n]; fsn[n] = 0; }
    return 1;
}
uint32_t aret_BeginDeferWindowPos(uint32_t esp) { (void)esp; return 0x0DEF0001u; }
uint32_t aret_DeferWindowPos(uint32_t esp) {
    int i = u32_win_idx(WU(1));
    if (i >= 0) {
        uint32_t f = WU(7);
        if (!(f & 0x0002u)) { g_u32_win[i].x = WI(3); g_u32_win[i].y = WI(4); }  /* !SWP_NOMOVE */
        if (!(f & 0x0001u)) { g_u32_win[i].w = WI(5); g_u32_win[i].h = WI(6); }  /* !SWP_NOSIZE */
        if (f & 0x0040u) g_u32_win[i].visible = 1;
        if (f & 0x0080u) g_u32_win[i].visible = 0;
    }
    return WU(0);   /* return the (unchanged) HDWP */
}
uint32_t aret_EndDeferWindowPos(uint32_t esp) { (void)esp; return 1; }

/* Window text: routed through WM_SETTEXT/GETTEXT/GETTEXTLENGTH so a subclassing
 * WNDPROC observes them exactly as under Windows (DefWindowProc stores/reports).
 * A and W share the machinery; the ANSI/wide payload distinction is realised by
 * which DefWindowProc the guest's WNDPROC chains to. */
/* SetWindowTextA(HWND, lpString) -> BOOL. */
uint32_t aret_SetWindowTextA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) return 0;
    u32_call_wndproc(esp, wndproc, WU(0), U32_WM_SETTEXT, 0, WU(1));
    return 1;
}
uint32_t aret_SetWindowTextW(uint32_t esp) { return aret_SetWindowTextA(esp); }
/* GetWindowTextA(HWND, lpString, nMaxCount) -> int (chars copied, excl. NUL). */
uint32_t aret_GetWindowTextA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) return 0;
    return u32_call_wndproc(esp, wndproc, WU(0), U32_WM_GETTEXT, WU(2), WU(1));
}
uint32_t aret_GetWindowTextW(uint32_t esp) { return aret_GetWindowTextA(esp); }
/* GetWindowTextLengthA(HWND) -> int. */
uint32_t aret_GetWindowTextLengthA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) return 0;
    return u32_call_wndproc(esp, wndproc, WU(0), U32_WM_GETTEXTLENGTH, 0, 0);
}
uint32_t aret_GetWindowTextLengthW(uint32_t esp) { return aret_GetWindowTextLengthA(esp); }

/* GetSystemMetrics(nIndex) -> int. Display-independent metrics return their classic
 * fixed values; screen dimensions return ARET's defined virtual-desktop size. Raw
 * screen values are environment-dependent (doc 72 §4.5): tested by invariant, never
 * bit-compared to Wine. Unmodelled indices return 0. */
uint32_t aret_GetSystemMetrics(uint32_t esp) {
    switch (WI(0)) {
    case 0:  case 78: return U32_SCREEN_W;  /* SM_CXSCREEN / SM_CXVIRTUALSCREEN */
    case 1:  case 79: return U32_SCREEN_H;  /* SM_CYSCREEN / SM_CYVIRTUALSCREEN */
    case 16: return U32_SCREEN_W;           /* SM_CXFULLSCREEN (approx) */
    case 17: return U32_SCREEN_H;           /* SM_CYFULLSCREEN (approx) */
    case 2:  return 16;   /* SM_CXVSCROLL */
    case 3:  return 16;   /* SM_CYHSCROLL */
    case 4:  return 19;   /* SM_CYCAPTION */
    case 5:  return 1;    /* SM_CXBORDER */
    case 6:  return 1;    /* SM_CYBORDER */
    case 11: return 32;   /* SM_CXICON */
    case 12: return 32;   /* SM_CYICON */
    case 13: return 32;   /* SM_CXCURSOR */
    case 14: return 32;   /* SM_CYCURSOR */
    case 15: return 19;   /* SM_CYMENU */
    case 30: case 31: return 18; /* SM_CXSIZE / SM_CYSIZE */
    case 32: return 4;    /* SM_CXFRAME / SM_CXSIZEFRAME */
    case 33: return 4;    /* SM_CYFRAME / SM_CYSIZEFRAME */
    case 43: return 3;    /* SM_CMOUSEBUTTONS */
    case 45: return 2;    /* SM_CXEDGE */
    case 46: return 2;    /* SM_CYEDGE */
    case 49: return 16;   /* SM_CXSMICON */
    case 50: return 16;   /* SM_CYSMICON */
    default: return 0;
    }
}

/* ================================================================== */
/* Dialogs — DLGTEMPLATE parse -> controls + modal pump (display-free) */
/* ================================================================== */
/* A dialog is a window (wndproc = the app's DLGPROC) whose child controls are
 * windows too (each with its control id + template text; wndproc 0 = a system
 * control, its text served natively). DialogBoxParamA creates them, sends
 * WM_INITDIALOG, and runs a modal pump; a DLGPROC that EndDialog's during
 * WM_INITDIALOG (the scriptable/headless case) returns at once. If a dialog
 * instead waits for user input there is nothing to pump headless -> abort loudly
 * (never hang or fake a result). Reuses the G2a window table; still display-free
 * (real pixels are G2b). */
#define U32_WM_INITDIALOG 0x0110u

static uint32_t g_u32_modal_hwnd;   /* active modal dialog (0 = none) */
static int      g_u32_modal_ended;
static int      g_u32_modal_result;

/* Resolve a dialog template resource (RT_DIALOG=5) to its bytes, or NULL. */
static const uint8_t *u32_dlg_template(uint32_t name_ref) {
    const uint8_t *de = u32_rsrc_data_entry(5 /* RT_DIALOG */, name_ref);
    if (!de) return NULL;
    return (const uint8_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
}
/* Advance past a template "sz_Or_Ord" field (WORD 0 = empty; 0xFFFF + WORD ordinal;
 * else a NUL-terminated WCHAR string). If `title`, narrow a string form to ANSI. */
static const uint8_t *u32_dt_szord(const uint8_t *p, char *title, int cap) {
    if (title && cap > 0) title[0] = 0;
    uint16_t w = *(const uint16_t *)p;
    if (w == 0x0000) return p + 2;
    if (w == 0xFFFF) return p + 4;                       /* 0xFFFF + ordinal WORD */
    const uint16_t *s = (const uint16_t *)p;
    int i = 0;
    while (s[i]) { if (title && i < cap - 1) title[i] = (char)(s[i] & 0xFF); i++; }
    if (title && cap > 0) title[i < cap ? i : cap - 1] = 0;
    return (const uint8_t *)(s + i + 1);                 /* past the NUL */
}
/* DWORD-align a cursor relative to the template base. */
static const uint8_t *u32_dt_align(const uint8_t *base, const uint8_t *p) {
    size_t off = (size_t)(p - base);
    return base + ((off + 3) & ~(size_t)3);
}
/* Allocate a child-control window (system control: wndproc 0, text stored). */
static uint32_t u32_new_control(uint32_t parent, int ctrl_id, uint32_t style, const char *title) {
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) {
            memset(&g_u32_win[i], 0, sizeof g_u32_win[i]);
            g_u32_win[i].used = 1;
            g_u32_win[i].parent = parent;
            g_u32_win[i].style = style;
            g_u32_win[i].enabled = (style & 0x08000000u) ? 0 : 1;  /* WS_DISABLED */
            g_u32_win[i].visible = (style & 0x10000000u) ? 1 : 0;  /* WS_VISIBLE */
            g_u32_win[i].ctrl_id = ctrl_id;
            int k = 0; if (title) for (; title[k] && k < 255; k++) g_u32_win[i].title[k] = title[k];
            g_u32_win[i].title[k] = 0;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}
/* Parse a DLGTEMPLATE(EX) -> create the dialog window (wndproc = dlgproc) and its
 * child controls. Returns the dialog HWND (0 on failure). Handles both templates. */
static uint32_t u32_dialog_create(const uint8_t *tpl, uint32_t dlgproc, uint32_t parent) {
    const uint8_t *p = tpl;
    int ex = 0;
    uint32_t style; uint16_t cdit;
    if (*(const uint16_t *)p == 1 && *(const uint16_t *)(p + 2) == 0xFFFF) {  /* DLGTEMPLATEEX */
        ex = 1;
        style = *(const uint32_t *)(p + 12);            /* after dlgVer,sig,helpID,exStyle */
        cdit  = *(const uint16_t *)(p + 16);
        p += 26;                                        /* +cDlgItems(2)+x,y,cx,cy(8) -> menu */
    } else {                                            /* classic DLGTEMPLATE */
        style = *(const uint32_t *)p;
        cdit  = *(const uint16_t *)(p + 8);             /* style(4)+exStyle(4) -> cdit */
        p += 18;                                        /* +cdit(2)+x,y,cx,cy(8) -> menu */
    }
    char dtitle[256];
    p = u32_dt_szord(p, NULL, 0);                        /* menu */
    p = u32_dt_szord(p, NULL, 0);                        /* window class */
    p = u32_dt_szord(p, dtitle, sizeof dtitle);         /* caption */
    if (style & 0x40u /* DS_SETFONT */) {
        p += ex ? 6 : 2;                                /* EX: size,weight,italic,charset ; classic: size */
        p = u32_dt_szord(p, NULL, 0);                   /* typeface */
    }
    uint32_t hDlg = u32_window_create(dlgproc, 0, style, 0, 0, 0, 0, parent, dtitle, 0);
    if (!hDlg) return 0;
    for (int c = 0; c < cdit; c++) {
        p = u32_dt_align(tpl, p);
        uint32_t cstyle, cid;
        if (ex) {
            cstyle = *(const uint32_t *)(p + 8);        /* helpID(4)+exStyle(4) -> style */
            p += 20; cid = *(const uint32_t *)p; p += 4; /* +x,y,cx,cy(8) -> id(DWORD) */
        } else {
            cstyle = *(const uint32_t *)p;
            p += 16; cid = *(const uint16_t *)p; p += 2; /* +exStyle(4)+x,y,cx,cy(8) -> id(WORD) */
        }
        char ctitle[256];
        p = u32_dt_szord(p, NULL, 0);                   /* control class (atom or name) */
        p = u32_dt_szord(p, ctitle, sizeof ctitle);     /* control caption */
        uint16_t extra = *(const uint16_t *)p; p += 2;  /* creation-data byte count */
        p += extra;
        u32_new_control(hDlg, (int)cid, cstyle, ctitle);
    }
    return hDlg;
}
/* Destroy a dialog and all its child controls. */
static void u32_dialog_destroy(uint32_t hDlg) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && ((uint32_t)(i + 1) == hDlg || g_u32_win[i].parent == hDlg))
            g_u32_win[i].used = 0;
}
/* Find a dialog control by id (0 if none). */
static uint32_t u32_dlg_item(uint32_t hDlg, int id) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].parent == hDlg && g_u32_win[i].ctrl_id == id)
            return (uint32_t)(i + 1);
    return 0;
}

/* DialogBoxParamA(hInst, lpTemplate, hWndParent, lpDialogFunc, dwInitParam) -> INT_PTR.
 * Modal: create, WM_INITDIALOG, pump until EndDialog, return the EndDialog result. */
uint32_t aret_DialogBoxParamA(uint32_t esp) {
    const uint8_t *tpl = u32_dlg_template(WU(1));
    uint32_t dlgproc = WU(3), param = WU(4), parent = WU(2);
    if (!tpl || !dlgproc) return (uint32_t)-1;
    if (g_u32_modal_hwnd) aret_unimpl("nested modal DialogBox (mono-thread model)");
    uint32_t hDlg = u32_dialog_create(tpl, dlgproc, parent);
    if (!hDlg) return (uint32_t)-1;
    g_u32_modal_hwnd = hDlg; g_u32_modal_ended = 0; g_u32_modal_result = 0;
    u32_call_wndproc(esp, dlgproc, hDlg, U32_WM_INITDIALOG, 0, param);
    while (!g_u32_modal_ended) {
        u32_pump_timers();
        if (!u32_q_empty()) {
            uint32_t m[7]; u32_q_peek_copy(m, 1);
            uint32_t wp = u32_win_wndproc(m[0]);
            if (wp) u32_call_wndproc(esp, wp, m[0], m[1], m[2], m[3]);
        } else {
            aret_unimpl("modal DialogBox: DLGPROC did not EndDialog and no events (headless)");
        }
    }
    int result = g_u32_modal_result;
    u32_dialog_destroy(hDlg);
    g_u32_modal_hwnd = 0;
    return (uint32_t)result;
}
uint32_t aret_DialogBoxParamW(uint32_t esp) { return aret_DialogBoxParamA(esp); }
/* CreateDialogParamA(...) -> HWND. Modeless: create + WM_INITDIALOG, return HWND. */
uint32_t aret_CreateDialogParamA(uint32_t esp) {
    const uint8_t *tpl = u32_dlg_template(WU(1));
    uint32_t dlgproc = WU(3), param = WU(4), parent = WU(2);
    if (!tpl || !dlgproc) return 0;
    uint32_t hDlg = u32_dialog_create(tpl, dlgproc, parent);
    if (!hDlg) return 0;
    u32_call_wndproc(esp, dlgproc, hDlg, U32_WM_INITDIALOG, 0, param);
    return hDlg;
}
uint32_t aret_CreateDialogParamW(uint32_t esp) { return aret_CreateDialogParamA(esp); }
/* EndDialog(hDlg, nResult) -> BOOL. Ends the active modal loop with a result. */
uint32_t aret_EndDialog(uint32_t esp) {
    g_u32_modal_ended = 1; g_u32_modal_result = WI(1);
    return 1;
}
/* GetDlgItem(hDlg, nIDDlgItem) -> HWND. */
uint32_t aret_GetDlgItem(uint32_t esp) { return u32_dlg_item(WU(0), WI(1)); }
/* GetDlgCtrlID(hWnd) -> int (the control's id). */
uint32_t aret_GetDlgCtrlID(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return i < 0 ? 0 : (uint32_t)g_u32_win[i].ctrl_id;
}
/* SetDlgItemTextA(hDlg, id, lpString) -> BOOL. A system control stores its text. */
uint32_t aret_SetDlgItemTextA(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    const char *s = WCS(2); int k = 0;
    if (s) for (; s[k] && k < 255; k++) g_u32_win[i].title[k] = s[k];
    g_u32_win[i].title[k] = 0;
    return 1;
}
/* GetDlgItemTextA(hDlg, id, lpString, cchMax) -> chars copied (excl NUL). */
uint32_t aret_GetDlgItemTextA(uint32_t esp) {
    char *buf = (char *)WP(2); uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = t[j];
    buf[n] = 0;
    return n;
}
/* SetDlgItemTextW(hDlg, id, lpString) — wide string, narrowed into the control. */
uint32_t aret_SetDlgItemTextW(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    u32_w2n((const uint16_t *)WP(2), g_u32_win[i].title, sizeof g_u32_win[i].title);
    return 1;
}
/* GetDlgItemTextW(hDlg, id, lpString, cchMax) — widen the stored ANSI text. */
uint32_t aret_GetDlgItemTextW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(2); uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)t[j];
    buf[n] = 0;
    return n;
}
/* SetDlgItemInt(hDlg, id, uValue, bSigned) -> BOOL. */
uint32_t aret_SetDlgItemInt(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    if (WU(3)) snprintf(g_u32_win[i].title, sizeof g_u32_win[i].title, "%d", WI(2));
    else       snprintf(g_u32_win[i].title, sizeof g_u32_win[i].title, "%u", WU(2));
    return 1;
}
/* GetDlgItemInt(hDlg, id, lpTranslated, bSigned) -> UINT. */
uint32_t aret_GetDlgItemInt(uint32_t esp) {
    uint32_t *ok = (uint32_t *)WP(2);
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { if (ok) *ok = 0; return 0; }
    char *end = NULL; const char *t = g_u32_win[i].title;
    long v = WU(3) ? strtol(t, &end, 10) : (long)strtoul(t, &end, 10);
    if (ok) *ok = (end && end != t && *end == 0) ? 1 : 0;
    return (uint32_t)v;
}
/* SendDlgItemMessageA(hDlg, id, msg, wParam, lParam) -> LRESULT. */
uint32_t aret_SendDlgItemMessageA(uint32_t esp) {
    uint32_t child = u32_dlg_item(WU(0), WI(1));
    if (!child) return 0;
    uint32_t wp = u32_win_wndproc(child);
    if (wp) return u32_call_wndproc(esp, wp, child, WU(2), WU(3), WU(4));
    uint32_t r;
    if (u32_defproc_text(child, WU(2), WU(3), WU(4), 0, &r)) return r;  /* system control */
    return 0;
}
uint32_t aret_SendDlgItemMessageW(uint32_t esp) {
    uint32_t child = u32_dlg_item(WU(0), WI(1));
    if (!child) return 0;
    uint32_t wp = u32_win_wndproc(child);
    if (wp) return u32_call_wndproc(esp, wp, child, WU(2), WU(3), WU(4));
    uint32_t r;
    if (u32_defproc_text(child, WU(2), WU(3), WU(4), 1, &r)) return r;
    return 0;
}

/* ================================================================== */
/* GDI — object/DC model + bit-exact DIB drawing (framebuffer oracle)  */
/* ================================================================== */
/* GDI objects (DCs, bitmaps, brushes, pens, fonts) live in one handle table;
 * handles are opaque (base 0x30000000, distinct from HWND/GDI-stock). Drawing
 * targets a memory DIB section — a 32-bit BGRA buffer we own — so SetPixel/
 * FillRect/PatBlt are exact memory writes that match Wine's DIB byte-for-byte
 * (verified: a 32bpp BI_RGB pixel is [B,G,R,0]). The oracle hashes that buffer.
 * Screen/window DCs have no surface (no display) -> their drawing is a sound
 * no-op; only the offscreen DIB path is pixel-verified. Text (font raster) and
 * pen-edged shapes are NOT here (can't match Wine's rasteriser bit-for-bit) —
 * abort sound until a binary needs a modellable subset. GDI is vast; we stop at
 * the measured, exactly-reproducible core (doc 72 §5). */
#define GDI_MAX  512
#define GDI_BASE 0x30000000u
enum { GDIT_DC = 1, GDIT_BITMAP, GDIT_BRUSH, GDIT_PEN, GDIT_FONT, GDIT_RGN };
static struct gdi_obj {
    int type, used, stock, null_obj;
    uint32_t sel_bitmap, sel_brush, sel_pen, sel_font;   /* DC */
    uint32_t text_color, bk_color; int bk_mode; uint32_t text_align;  /* DC */
    int w, h, topdown, bpp; uint8_t *bits; int owns_bits; /* BITMAP */
    uint32_t color;                                      /* BRUSH/PEN */
    int mapmode, savetop;                                /* DC map mode + save-stack depth */
    struct { uint32_t font, brush, pen, tc, bc; int bm, mm; } sstk[8];  /* SaveDC/RestoreDC */
} g_gdi[GDI_MAX];

static uint32_t gdi_handle(int i) { return GDI_BASE | (uint32_t)i; }
static int gdi_idx(uint32_t h) {
    if ((h & 0xFF000000u) != GDI_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < GDI_MAX && g_gdi[i].used) ? (int)i : -1;
}
static int gdi_alloc(int type) {
    for (int i = 1; i < GDI_MAX; i++)
        if (!g_gdi[i].used) { memset(&g_gdi[i], 0, sizeof g_gdi[i]); g_gdi[i].used = 1; g_gdi[i].type = type; return i; }
    return 0;
}
/* The DIB surface a DC currently draws to (NULL if none / screen DC). */
static struct gdi_obj *gdi_dc_surface(uint32_t hdc) {
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return NULL;
    int b = gdi_idx(g_gdi[d].sel_bitmap);
    if (b < 0 || g_gdi[b].type != GDIT_BITMAP || !g_gdi[b].bits) return NULL;
    return &g_gdi[b];
}
/* Address of pixel (x,y) in a DIB (honours top-down vs bottom-up), or NULL. */
static uint8_t *gdi_px(struct gdi_obj *bm, int x, int y) {
    if (x < 0 || y < 0 || x >= bm->w || y >= bm->h) return NULL;
    int row = bm->topdown ? y : (bm->h - 1 - y);
    return bm->bits + ((size_t)row * bm->w + x) * 4;
}
/* COLORREF (0x00BBGGRR) -> DIB bytes [B,G,R,0], and back. */
static void gdi_put(struct gdi_obj *bm, int x, int y, uint32_t c) {
    uint8_t *p = gdi_px(bm, x, y); if (!p) return;
    p[0] = (c >> 16) & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = c & 0xFF; p[3] = 0;
}
static uint32_t gdi_getpx(struct gdi_obj *bm, int x, int y) {
    uint8_t *p = gdi_px(bm, x, y); if (!p) return 0xFFFFFFFFu; /* CLR_INVALID */
    return (uint32_t)p[2] | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16);
}

/* ================================================================== */
/* G2b — SDL2 window presentation (doc 72). Compiled only when the      */
/* program creates a window (`-DARET_HAVE_SDL`). Each visible top-level  */
/* window gets: (1) a client-area DIB framebuffer that GetDC/BeginPaint  */
/* bind to, so the program's GDI draws land in it; (2) a real           */
/* SDL_Window it is blitted to on UpdateWindow/EndPaint; (3) SDL input   */
/* events pumped into WM_* messages. Everything degrades to a sound      */
/* display-free no-op when there is no usable display (SDL_Init fails).  */
/* ================================================================== */
#ifdef ARET_HAVE_SDL
static int g_sdl_ready = 0;   /* 0 = not tried, 1 = video up, -1 = unavailable */
static int sdl_ensure(void) {
    if (g_sdl_ready) return g_sdl_ready > 0;
    /* Video only; no audio/events subsystems we don't drive. A missing display
     * (no DISPLAY, no dummy driver) is not an error here — we fall back to the
     * display-free path, never abort. */
    g_sdl_ready = (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) ? 1 : -1;
    return g_sdl_ready > 0;
}
/* Create the client framebuffer + real SDL window for window i (idempotent). */
static void sdl_window_show(int i) {
    if (i < 0 || i >= U32_MAX_WIN || !g_u32_win[i].used) return;
    if (g_u32_win[i].sdl_win) { SDL_ShowWindow((SDL_Window *)g_u32_win[i].sdl_win); return; }
    int w = g_u32_win[i].w, h = g_u32_win[i].h;
    if (w <= 0) w = 320; if (h <= 0) h = 240;        /* sane default if unsized */
    if (w > 8192) w = 8192; if (h > 8192) h = 8192;
    /* Client-area framebuffer (a top-down 32bpp DIB) is allocated even if the
     * real window can't be created (headless): GetDC still gives the program a
     * surface to draw into, and the drawing round-trips like Wine's. */
    int b = gdi_alloc(GDIT_BITMAP);
    if (b) {
        g_gdi[b].w = w; g_gdi[b].h = h; g_gdi[b].topdown = 1; g_gdi[b].bpp = 32;
        g_gdi[b].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[b].owns_bits = 1;
        if (!g_gdi[b].bits) { g_gdi[b].used = 0; b = 0; }
    }
    g_u32_win[i].client_bmp = b ? gdi_handle(b) : 0;
    g_u32_win[i].cw = w; g_u32_win[i].ch = h;
    if (!sdl_ensure()) return;                       /* no display: framebuffer only */
    int px = g_u32_win[i].x, py = g_u32_win[i].y;
    SDL_Window *win = SDL_CreateWindow(g_u32_win[i].title[0] ? g_u32_win[i].title : "",
                                       px > 0 ? px : (int)SDL_WINDOWPOS_CENTERED,
                                       py > 0 ? py : (int)SDL_WINDOWPOS_CENTERED,
                                       w, h, SDL_WINDOW_SHOWN);
    if (!win) return;                                /* creation failed: framebuffer only */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture  *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888,
                                                SDL_TEXTUREACCESS_STREAMING, w, h) : NULL;
    g_u32_win[i].sdl_win = win; g_u32_win[i].sdl_ren = ren; g_u32_win[i].sdl_tex = tex;
    sdl_window_present(i);
}
/* Blit the client framebuffer to the SDL window (no-op if not shown). The DIB
 * bytes are [B,G,R,0] which is exactly SDL_PIXELFORMAT_RGB888 memory order. */
static void sdl_window_present(int i) {
    if (i < 0 || i >= U32_MAX_WIN) return;
    SDL_Renderer *ren = (SDL_Renderer *)g_u32_win[i].sdl_ren;
    SDL_Texture  *tex = (SDL_Texture *)g_u32_win[i].sdl_tex;
    if (!ren || !tex) return;
    int b = gdi_idx(g_u32_win[i].client_bmp);
    if (b < 0 || !g_gdi[b].bits) return;
    SDL_UpdateTexture(tex, NULL, g_gdi[b].bits, g_u32_win[i].cw * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}
static void sdl_window_destroy(int i) {
    if (i < 0 || i >= U32_MAX_WIN) return;
    if (g_u32_win[i].sdl_tex) SDL_DestroyTexture((SDL_Texture *)g_u32_win[i].sdl_tex);
    if (g_u32_win[i].sdl_ren) SDL_DestroyRenderer((SDL_Renderer *)g_u32_win[i].sdl_ren);
    if (g_u32_win[i].sdl_win) SDL_DestroyWindow((SDL_Window *)g_u32_win[i].sdl_win);
    g_u32_win[i].sdl_tex = g_u32_win[i].sdl_ren = g_u32_win[i].sdl_win = NULL;
    int b = gdi_idx(g_u32_win[i].client_bmp);
    if (b >= 0) { if (g_gdi[b].owns_bits) free(g_gdi[b].bits); g_gdi[b].used = 0; }
    g_u32_win[i].client_bmp = 0;
}
static int sdl_any_window(void) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].sdl_win) return 1;
    return 0;
}
static int sdl_win_idx_from_id(uint32_t winid) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].sdl_win &&
            SDL_GetWindowID((SDL_Window *)g_u32_win[i].sdl_win) == winid) return i;
    return -1;
}
/* Map an SDL key symbol to a Win32 virtual-key code (the subset a GUI app tends
 * to test). Letters/digits share the ASCII value with their VK code. */
static uint32_t sdl_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 'a' + 'A');   /* VK_A..VK_Z */
    if (k >= '0' && k <= '9') return (uint32_t)k;                 /* VK_0..VK_9 */
    switch (k) {
    case SDLK_ESCAPE: return 0x1B; case SDLK_RETURN: return 0x0D;
    case SDLK_SPACE:  return 0x20; case SDLK_TAB:    return 0x09;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    default: return (uint32_t)(k & 0xFF);
    }
}
/* Drain SDL input into the Win32 message queue. Only real user input (close,
 * mouse, keyboard) becomes a message; window-manager noise (expose/focus) does
 * NOT synthesise WM_PAINT/WM_ACTIVATE — those stay driven by the Win32
 * invalidation model, so the deterministic message oracle is untouched. */
static void sdl_pump(void) {
    if (g_sdl_ready <= 0) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int wi; uint32_t lp;
        switch (e.type) {
        case SDL_QUIT:
            for (int i = 0; i < U32_MAX_WIN; i++)
                if (g_u32_win[i].used && g_u32_win[i].sdl_win)
                    u32_q_push((uint32_t)(i + 1), 0x0010u /* WM_CLOSE */, 0, 0);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                wi = sdl_win_idx_from_id(e.window.windowID);
                if (wi >= 0) u32_q_push((uint32_t)(wi + 1), 0x0010u /* WM_CLOSE */, 0, 0);
            }
            break;
        case SDL_MOUSEMOTION:
            wi = sdl_win_idx_from_id(e.motion.windowID);
            if (wi >= 0) { lp = ((uint32_t)(e.motion.x & 0xFFFF)) | ((uint32_t)(e.motion.y & 0xFFFF) << 16);
                           u32_q_push((uint32_t)(wi + 1), 0x0200u /* WM_MOUSEMOVE */, 0, lp); }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            wi = sdl_win_idx_from_id(e.button.windowID);
            if (wi < 0) break;
            lp = ((uint32_t)(e.button.x & 0xFFFF)) | ((uint32_t)(e.button.y & 0xFFFF) << 16);
            int down = (e.type == SDL_MOUSEBUTTONDOWN);
            uint32_t msg = (e.button.button == SDL_BUTTON_RIGHT)
                         ? (down ? 0x0204u : 0x0205u)   /* WM_RBUTTONDOWN/UP */
                         : (down ? 0x0201u : 0x0202u);  /* WM_LBUTTONDOWN/UP */
            u32_q_push((uint32_t)(wi + 1), msg, 0, lp);
            break; }
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            wi = sdl_win_idx_from_id(e.key.windowID);
            if (wi >= 0)
                u32_q_push((uint32_t)(wi + 1),
                           e.type == SDL_KEYDOWN ? 0x0100u : 0x0101u /* WM_KEYDOWN/UP */,
                           sdl_vk(e.key.keysym.sym), 0);
            break;
        default: break;
        }
    }
}
/* The client-framebuffer HBITMAP a window DC should draw into, or 0. */
static uint32_t u32_win_client_bmp(uint32_t hwnd) {
    if (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used)
        return g_u32_win[hwnd - 1].client_bmp;
    return 0;
}
#endif  /* ARET_HAVE_SDL */

/* ---- DC lifecycle ---- */
static void u32_dc_defaults(int d);   /* selects the DC's default stock objects */
/* GetDC/GetWindowDC(hwnd) -> HDC. A screen/window DC (no offscreen surface: no
 * display, so its drawing is a sound no-op). */
uint32_t aret_GetDC(uint32_t esp) {
    int i = gdi_alloc(GDIT_DC); if (!i) return 0;
    u32_dc_defaults(i);
#ifdef ARET_HAVE_SDL
    /* A window DC draws into that window's client framebuffer (G2b), so GDI paints
     * land on the visible surface; a screen/NULL DC stays a sound no-op. */
    uint32_t cb = u32_win_client_bmp(WU(0));
    if (cb) g_gdi[i].sel_bitmap = cb;
#endif
    return gdi_handle(i);
}
uint32_t aret_GetWindowDC(uint32_t esp) { return aret_GetDC(esp); }
/* ReleaseDC(hwnd, hdc) -> int (1). Presents the window's framebuffer if a window
 * DC was released (a program often draws to GetDC then expects it on screen). */
uint32_t aret_ReleaseDC(uint32_t esp) {
    int i = gdi_idx(WU(1));
#ifdef ARET_HAVE_SDL
    { int wi = u32_win_idx(WU(0));
      if (wi >= 0) {
          sdl_window_present(wi);
          /* A window DC has the window's shared framebuffer "selected"; free the
           * DC object but never the framebuffer (owned by the window). */
          if (i >= 0 && g_gdi[i].sel_bitmap == g_u32_win[wi].client_bmp) { g_gdi[i].used = 0; return 1; }
      } }
#endif
    if (i >= 0 && !g_gdi[i].sel_bitmap) g_gdi[i].used = 0;
    return 1;
}
/* CreateCompatibleDC(hdc) -> HDC (a memory DC; select a bitmap before drawing). */
uint32_t aret_CreateCompatibleDC(uint32_t esp) { (void)esp; int i = gdi_alloc(GDIT_DC); if (i) u32_dc_defaults(i); return i ? gdi_handle(i) : 0; }
/* DeleteDC(hdc) -> BOOL. */
uint32_t aret_DeleteDC(uint32_t esp) { int i = gdi_idx(WU(0)); if (i < 0) return 0; g_gdi[i].used = 0; return 1; }
/* BeginPaint(hwnd, LPPAINTSTRUCT) -> HDC. Zero the PAINTSTRUCT, hand back a DC. */
uint32_t aret_BeginPaint(uint32_t esp) {
    uint8_t *ps = (uint8_t *)WP(1);
    int i = gdi_alloc(GDIT_DC); if (i) u32_dc_defaults(i);
    uint32_t hdc = i ? gdi_handle(i) : 0;
    int wi = u32_win_idx(WU(0));
#ifdef ARET_HAVE_SDL
    /* Paint into the window's client framebuffer, like GetDC. */
    if (i) { uint32_t cb = u32_win_client_bmp(WU(0)); if (cb) g_gdi[i].sel_bitmap = cb; }
#endif
    /* A pending erase sends WM_ERASEBKGND now (DefWindowProc fills the class
     * brush into the DC we just bound); then the update region is validated.
     * needs_erase is cleared *before* the callback so a re-entrant BeginPaint
     * cannot loop. */
    if (wi >= 0) {
        if (g_u32_win[wi].needs_erase) {
            g_u32_win[wi].needs_erase = 0;
            uint32_t wp = g_u32_win[wi].wndproc;
            if (wp) u32_call_wndproc(esp, wp, WU(0), U32_WM_ERASEBKGND, hdc, 0);
        }
        g_u32_win[wi].needs_paint = 0;
    }
    if (ps) { memset(ps, 0, 64); *(uint32_t *)ps = hdc; }   /* PAINTSTRUCT.hdc @0 */
    return hdc;
}
/* EndPaint(hwnd, const PAINTSTRUCT*) -> BOOL. Present the window, release the DC. */
uint32_t aret_EndPaint(uint32_t esp) {
    const uint8_t *ps = (const uint8_t *)WP(1);
#ifdef ARET_HAVE_SDL
    { int wi = u32_win_idx(WU(0)); if (wi >= 0) sdl_window_present(wi); }
#endif
    if (ps) { int i = gdi_idx(*(const uint32_t *)ps); if (i >= 0) g_gdi[i].used = 0; }
    return 1;
}
uint32_t aret_GdiFlush(uint32_t esp) { (void)esp; return 1; }   /* writes are immediate */

/* ---- bitmaps ---- */
/* CreateDIBSection(hdc, pbmi, usage, ppvBits, hSection, offset) -> HBITMAP.
 * 32bpp BI_RGB only (the exactly-reproducible case); other depths abort sound. */
uint32_t aret_CreateDIBSection(uint32_t esp) {
    const uint8_t *bmi = (const uint8_t *)WP(1);
    uint32_t *ppv = (uint32_t *)WP(3);
    if (ppv) *ppv = 0;
    if (!bmi) return 0;
    int32_t bw = *(const int32_t *)(bmi + 4);
    int32_t bh = *(const int32_t *)(bmi + 8);
    uint16_t bpp = *(const uint16_t *)(bmi + 14);
    if (bpp != 32) { aret_unimpl("CreateDIBSection: only 32bpp BI_RGB modelled"); return 0; }
    int w = bw, h = bh < 0 ? -bh : bh, td = bh < 0;
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = td; g_gdi[i].bpp = 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    if (ppv) *ppv = (uint32_t)(uintptr_t)g_gdi[i].bits;
    return gdi_handle(i);
}
/* CreateCompatibleBitmap(hdc, w, h) -> HBITMAP (32bpp, not app-exposed). */
uint32_t aret_CreateCompatibleBitmap(uint32_t esp) {
    int w = WI(1), h = WI(2);
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = 1; g_gdi[i].bpp = 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    return gdi_handle(i);
}

/* ---- objects ---- */
uint32_t aret_CreateSolidBrush(uint32_t esp) {
    int i = gdi_alloc(GDIT_BRUSH); if (!i) return 0;
    g_gdi[i].color = WU(0) & 0x00FFFFFFu;
    return gdi_handle(i);
}
uint32_t aret_CreatePen(uint32_t esp) {
    int i = gdi_alloc(GDIT_PEN); if (!i) return 0;
    g_gdi[i].color = WU(2) & 0x00FFFFFFu;                 /* (style, width, color) */
    return gdi_handle(i);
}
/* GetStockObject(i) -> HGDIOBJ. Distinct, cached handle per stock id (opaque). */
static uint32_t g_gdi_stock[32];
static uint32_t u32_stock(int id) {
    if (id < 0 || id >= 32) return 0;
    if (!g_gdi_stock[id]) {
        int type = (id <= 5) ? GDIT_BRUSH : (id <= 8) ? GDIT_PEN : GDIT_FONT;
        int i = gdi_alloc(type); if (!i) return 0;
        g_gdi[i].stock = 1;
        switch (id) {                                     /* stock brush colours */
        case 0: g_gdi[i].color = 0xFFFFFF; break;         /* WHITE_BRUSH */
        case 1: g_gdi[i].color = 0xC0C0C0; break;         /* LTGRAY_BRUSH */
        case 2: g_gdi[i].color = 0x808080; break;         /* GRAY_BRUSH */
        case 3: g_gdi[i].color = 0x404040; break;         /* DKGRAY_BRUSH */
        case 4: g_gdi[i].color = 0x000000; break;         /* BLACK_BRUSH */
        case 5: g_gdi[i].null_obj = 1; break;             /* NULL_BRUSH / HOLLOW_BRUSH */
        case 6: g_gdi[i].color = 0xFFFFFF; break;         /* WHITE_PEN */
        case 7: g_gdi[i].color = 0x000000; break;         /* BLACK_PEN */
        case 8: g_gdi[i].null_obj = 1; break;             /* NULL_PEN */
        default: break;                                   /* fonts: opaque */
        }
        g_gdi_stock[id] = gdi_handle(i);
    }
    return g_gdi_stock[id];
}
uint32_t aret_GetStockObject(uint32_t esp) { return u32_stock(WI(0)); }
/* A new DC selects the Windows default objects (so SelectObject round-trips a
 * default back like Wine): SYSTEM_FONT, WHITE_BRUSH, BLACK_PEN. */
static void u32_dc_defaults(int d) {
    if (d < 0) return;
    g_gdi[d].sel_font  = u32_stock(13);   /* SYSTEM_FONT */
    g_gdi[d].sel_brush = u32_stock(0);    /* WHITE_BRUSH */
    g_gdi[d].sel_pen   = u32_stock(7);    /* BLACK_PEN */
    g_gdi[d].mapmode   = 1;               /* MM_TEXT */
}
/* SaveDC(hdc) -> level. RestoreDC(hdc, level) -> BOOL. Get/SetMapMode. GetClipBox. */
uint32_t aret_SaveDC(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC || g_gdi[d].savetop >= 8) return 0;
    int t = g_gdi[d].savetop;
    g_gdi[d].sstk[t].font = g_gdi[d].sel_font; g_gdi[d].sstk[t].brush = g_gdi[d].sel_brush;
    g_gdi[d].sstk[t].pen = g_gdi[d].sel_pen;   g_gdi[d].sstk[t].tc = g_gdi[d].text_color;
    g_gdi[d].sstk[t].bc = g_gdi[d].bk_color;   g_gdi[d].sstk[t].bm = g_gdi[d].bk_mode;
    g_gdi[d].sstk[t].mm = g_gdi[d].mapmode;
    return (uint32_t)(++g_gdi[d].savetop);
}
uint32_t aret_RestoreDC(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int level = WI(1);
    int idx = level < 0 ? g_gdi[d].savetop + level : level - 1;
    if (idx < 0 || idx >= g_gdi[d].savetop) return 0;
    g_gdi[d].sel_font = g_gdi[d].sstk[idx].font; g_gdi[d].sel_brush = g_gdi[d].sstk[idx].brush;
    g_gdi[d].sel_pen = g_gdi[d].sstk[idx].pen;   g_gdi[d].text_color = g_gdi[d].sstk[idx].tc;
    g_gdi[d].bk_color = g_gdi[d].sstk[idx].bc;   g_gdi[d].bk_mode = g_gdi[d].sstk[idx].bm;
    g_gdi[d].mapmode = g_gdi[d].sstk[idx].mm;
    g_gdi[d].savetop = idx;
    return 1;
}
uint32_t aret_SetMapMode(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0; int p = g_gdi[d].mapmode; g_gdi[d].mapmode = WI(1); return (uint32_t)p; }
uint32_t aret_GetMapMode(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0 : (uint32_t)g_gdi[d].mapmode; }
uint32_t aret_GetClipBox(uint32_t esp) {
    int32_t *r = (int32_t *)WP(1); if (!r) return 0;   /* ERROR */
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (bm) { r[0] = 0; r[1] = 0; r[2] = bm->w; r[3] = bm->h; }
    else    { r[0] = 0; r[1] = 0; r[2] = U32_SCREEN_W; r[3] = U32_SCREEN_H; }
    return 2;   /* SIMPLEREGION */
}
/* SelectObject(hdc, hgdiobj) -> previous object of that kind. */
uint32_t aret_SelectObject(uint32_t esp) {
    int d = gdi_idx(WU(0)), o = gdi_idx(WU(1));
    if (d < 0 || g_gdi[d].type != GDIT_DC || o < 0) return 0;
    uint32_t h = WU(1), prev = 0;
    switch (g_gdi[o].type) {
    case GDIT_BITMAP: prev = g_gdi[d].sel_bitmap; g_gdi[d].sel_bitmap = h; break;
    case GDIT_BRUSH:  prev = g_gdi[d].sel_brush;  g_gdi[d].sel_brush = h;  break;
    case GDIT_PEN:    prev = g_gdi[d].sel_pen;    g_gdi[d].sel_pen = h;    break;
    case GDIT_FONT:   prev = g_gdi[d].sel_font;   g_gdi[d].sel_font = h;   break;
    default: return 0;
    }
    return prev;
}
/* DeleteObject(hgdiobj) -> BOOL. Frees owned bits; stock objects are never freed. */
uint32_t aret_DeleteObject(uint32_t esp) {
    int i = gdi_idx(WU(0));
    if (i < 0) return 0;
    if (g_gdi[i].stock) return 1;
    if (g_gdi[i].owns_bits && g_gdi[i].bits) { free(g_gdi[i].bits); g_gdi[i].bits = NULL; }
    g_gdi[i].used = 0;
    return 1;
}

/* Resolve a FillRect "brush": a real brush handle, or a system-colour index+1
 * ((HBRUSH)(COLOR_x+1)). Returns 1 if a colour was produced, 0 for a null brush. */
static uint32_t u32_syscolor(int i);
static int gdi_brush_color(uint32_t hb, uint32_t *out) {
    int b = gdi_idx(hb);
    if (b >= 0 && g_gdi[b].type == GDIT_BRUSH) { if (g_gdi[b].null_obj) return 0; *out = g_gdi[b].color; return 1; }
    if (hb >= 1 && hb <= 31) { *out = u32_syscolor((int)hb - 1); return 1; }  /* COLOR_x + 1 */
    return 0;
}
/* Fill a DC's whole surface with a brush colour — the default WM_ERASEBKGND. */
static void u32_fill_dc_brush(uint32_t hdc, uint32_t brush) {
    struct gdi_obj *bm = gdi_dc_surface(hdc);
    uint32_t c;
    if (!bm || !gdi_brush_color(brush, &c)) return;      /* no surface / null brush */
    for (int y = 0; y < bm->h; y++)
        for (int x = 0; x < bm->w; x++) gdi_put(bm, x, y, c);
}

/* ---- drawing (bit-exact on the offscreen DIB) ---- */
/* SetPixel(hdc, x, y, color) -> COLORREF set (or CLR_INVALID). */
uint32_t aret_SetPixel(uint32_t esp) {
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm) return 0xFFFFFFFFu;
    uint32_t c = WU(3) & 0x00FFFFFFu;
    if (!gdi_px(bm, WI(1), WI(2))) return 0xFFFFFFFFu;
    gdi_put(bm, WI(1), WI(2), c);
    return c;
}
/* SetPixelV(hdc, x, y, color) -> BOOL. */
uint32_t aret_SetPixelV(uint32_t esp) {
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm || !gdi_px(bm, WI(1), WI(2))) return 0;
    gdi_put(bm, WI(1), WI(2), WU(3) & 0x00FFFFFFu);
    return 1;
}
/* GetPixel(hdc, x, y) -> COLORREF (or CLR_INVALID). */
uint32_t aret_GetPixel(uint32_t esp) {
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm) return 0xFFFFFFFFu;
    return gdi_getpx(bm, WI(1), WI(2));
}
/* FillRect(hdc, const RECT*, hbrush) -> int. [left,right) x [top,bottom). */
uint32_t aret_FillRect(uint32_t esp) {
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    uint32_t c;
    if (!gdi_brush_color(WU(2), &c)) return 1;            /* null brush: nothing */
    for (int y = r[1]; y < r[3]; y++)
        for (int x = r[0]; x < r[2]; x++) gdi_put(bm, x, y, c);
    return 1;
}
/* PatBlt(hdc, x, y, w, h, rop) -> BOOL. PATCOPY = fill with the selected brush;
 * BLACKNESS/WHITENESS = solid black/white. Other ROPs abort sound. */
uint32_t aret_PatBlt(uint32_t esp) {
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm) return 0;
    int x0 = WI(1), y0 = WI(2), x1 = x0 + WI(3), y1 = y0 + WI(4);
    uint32_t rop = WU(5), c;
    if (rop == 0x00F00021u /* PATCOPY */) {
        int b = gdi_idx(g_gdi[gdi_idx(WU(0))].sel_brush);
        if (b < 0) return 0;
        if (g_gdi[b].null_obj) return 1;
        c = g_gdi[b].color;
    } else if (rop == 0x00000042u /* BLACKNESS */) c = 0x000000;
    else if (rop == 0x00FF0062u /* WHITENESS */) c = 0xFFFFFF;
    else { aret_unimpl("PatBlt: only PATCOPY/BLACKNESS/WHITENESS modelled"); return 0; }
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) gdi_put(bm, x, y, c);
    return 1;
}
/* BitBlt(hdcDst, x, y, w, h, hdcSrc, x1, y1, rop) -> BOOL. SRCCOPY only. */
uint32_t aret_BitBlt(uint32_t esp) {
    struct gdi_obj *dst = gdi_dc_surface(WU(0));
    if (WU(8) != 0x00CC0020u /* SRCCOPY */) { aret_unimpl("BitBlt: only SRCCOPY modelled"); return 0; }
    struct gdi_obj *src = gdi_dc_surface(WU(5));
    if (!dst || !src) return 0;
    int dx = WI(1), dy = WI(2), w = WI(3), h = WI(4), sx = WI(6), sy = WI(7);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t *s = gdi_px(src, sx + i, sy + j);
            uint8_t *d = gdi_px(dst, dx + i, dy + j);
            if (s && d) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
        }
    return 1;
}

/* ================================================================== */
/* Common controls (comctl32) — image list. The data foundation of      */
/* toolbars / list-view / tree-view: a list of same-size 32bpp images.   */
/* Images are stored as a vertical strip (cx wide, count*cy tall); the    */
/* per-pixel alpha byte carries the transparency mask (0 = transparent).  */
/* Draw blits into a DC surface (bit-exact, reuses the GDI DIB model).    */
/* ================================================================== */
#define U32_MAX_IML 64
#define IML_BASE 0x50000000u
static struct { int used, cx, cy, count, cap; uint32_t bkcolor; uint8_t *bits; } g_iml[U32_MAX_IML];
static int iml_idx(uint32_t h) {
    if ((h & 0xFF000000u) != IML_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < U32_MAX_IML && g_iml[i].used) ? (int)i : -1;
}
static uint8_t *iml_px(int m, int img, int x, int y) {
    if (x < 0 || y < 0 || x >= g_iml[m].cx || y >= g_iml[m].cy) return NULL;
    return g_iml[m].bits + ((size_t)(img * g_iml[m].cy + y) * g_iml[m].cx + x) * 4;
}
static int iml_grow(int m, int need) {
    if (need <= g_iml[m].cap) return 1;
    int nc = g_iml[m].cap * 2; if (nc < need) nc = need;
    uint8_t *nb = (uint8_t *)calloc((size_t)g_iml[m].cx * g_iml[m].cy * nc, 4);
    if (!nb) return 0;
    if (g_iml[m].bits) { memcpy(nb, g_iml[m].bits, (size_t)g_iml[m].cx * g_iml[m].cy * g_iml[m].count * 4); free(g_iml[m].bits); }
    g_iml[m].bits = nb; g_iml[m].cap = nc; return 1;
}
uint32_t aret_ImageList_Create(uint32_t esp) {
    int cx = WI(0), cy = WI(1), cInit = WI(3);
    if (cx <= 0 || cy <= 0 || cx > 1024 || cy > 1024) return 0;
    for (int i = 0; i < U32_MAX_IML; i++) if (!g_iml[i].used) {
        memset(&g_iml[i], 0, sizeof g_iml[i]);
        g_iml[i].used = 1; g_iml[i].cx = cx; g_iml[i].cy = cy;
        g_iml[i].bkcolor = 0xFFFFFFFFu;                 /* CLR_NONE */
        g_iml[i].cap = cInit > 0 ? cInit : 4;
        g_iml[i].bits = (uint8_t *)calloc((size_t)cx * cy * g_iml[i].cap, 4);
        if (!g_iml[i].bits) { g_iml[i].used = 0; return 0; }
        return IML_BASE | (uint32_t)i;
    }
    return 0;
}
/* Copy the cx-wide tiles of a source HBITMAP into the list (opaque 32bpp copy).
 * We model 32bpp colour images without a colour-key mask plane, matching Wine's
 * behaviour for an ILC_COLOR32 source built from a flat BI_RGB DIB: the mask a
 * lower-depth list would derive from a colour key is not applied (32bpp images
 * carry their own alpha, which our sources don't). Returns the first new index. */
static int iml_add(int m, uint32_t hbm) {
    int b = gdi_idx(hbm);
    if (b < 0 || g_gdi[b].type != GDIT_BITMAP || !g_gdi[b].bits) return -1;
    int n = g_gdi[b].w / g_iml[m].cx; if (n < 1) n = 1;
    if (!iml_grow(m, g_iml[m].count + n)) return -1;
    int first = g_iml[m].count;
    for (int k = 0; k < n; k++) {
        int img = first + k;
        for (int y = 0; y < g_iml[m].cy; y++) for (int x = 0; x < g_iml[m].cx; x++) {
            uint8_t *s = gdi_px(&g_gdi[b], k * g_iml[m].cx + x, y);
            uint8_t *d = iml_px(m, img, x, y);
            if (!d) continue;
            if (s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0; }
            else  { d[0] = d[1] = d[2] = d[3] = 0; }
        }
    }
    g_iml[m].count = first + n;
    return first;
}
uint32_t aret_ImageList_Add(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? (uint32_t)-1 : (uint32_t)iml_add(m, WU(1)); }
uint32_t aret_ImageList_AddMasked(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? (uint32_t)-1 : (uint32_t)iml_add(m, WU(1)); }
/* ImageList_Draw(himl, i, hdcDst, x, y, fStyle) -> BOOL. An opaque tile copy into
 * the destination surface (see iml_add on the 32bpp mask model). */
uint32_t aret_ImageList_Draw(uint32_t esp) {
    int m = iml_idx(WU(0)), img = WI(1);
    if (m < 0 || img < 0 || img >= g_iml[m].count) return 0;
    struct gdi_obj *dst = gdi_dc_surface(WU(2));
    if (!dst) return 0;
    int x0 = WI(3), y0 = WI(4);
    for (int y = 0; y < g_iml[m].cy; y++) for (int x = 0; x < g_iml[m].cx; x++) {
        uint8_t *s = iml_px(m, img, x, y), *d = gdi_px(dst, x0 + x, y0 + y);
        if (s && d) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0; }
    }
    return 1;
}
uint32_t aret_ImageList_GetImageCount(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? 0 : (uint32_t)g_iml[m].count; }
uint32_t aret_ImageList_SetBkColor(uint32_t esp) { int m = iml_idx(WU(0)); if (m < 0) return 0xFFFFFFFFu; uint32_t o = g_iml[m].bkcolor; g_iml[m].bkcolor = WU(1); return o; }
uint32_t aret_ImageList_GetBkColor(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? 0xFFFFFFFFu : g_iml[m].bkcolor; }
uint32_t aret_ImageList_GetIconSize(uint32_t esp) {
    int m = iml_idx(WU(0)); if (m < 0) return 0;
    int32_t *cx = (int32_t *)WP(1), *cy = (int32_t *)WP(2);
    if (cx) *cx = g_iml[m].cx; if (cy) *cy = g_iml[m].cy; return 1;
}
uint32_t aret_ImageList_Destroy(uint32_t esp) { int m = iml_idx(WU(0)); if (m < 0) return 0; free(g_iml[m].bits); g_iml[m].used = 0; return 1; }
/* InitCommonControls() -> void; InitCommonControlsEx(const INITCOMMONCONTROLSEX*)
 * -> BOOL. Registering the control classes is a no-op in our model. */
uint32_t aret_InitCommonControls(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_InitCommonControlsEx(uint32_t esp) { (void)esp; return 1; }

/* ---- DC attributes ---- */
uint32_t aret_SetTextColor(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].text_color; g_gdi[d].text_color = WU(1) & 0xFFFFFFu; return p; }
uint32_t aret_SetBkColor(uint32_t esp)   { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].bk_color; g_gdi[d].bk_color = WU(1) & 0xFFFFFFu; return p; }
uint32_t aret_SetBkMode(uint32_t esp)    { int d = gdi_idx(WU(0)); if (d < 0) return 0; int p = g_gdi[d].bk_mode; g_gdi[d].bk_mode = WI(1); return (uint32_t)p; }
uint32_t aret_GetTextColor(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].text_color; }
uint32_t aret_GetBkColor(uint32_t esp)   { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].bk_color; }
/* SetTextAlign/GetTextAlign(hdc[, align]) -> UINT. DC state (default TA_LEFT|TA_TOP=0). */
uint32_t aret_SetTextAlign(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].text_align; g_gdi[d].text_align = WU(1); return p; }
uint32_t aret_GetTextAlign(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].text_align; }

/* GetSysColor(nIndex) -> COLORREF. Classic Win32 default scheme (deterministic;
 * the values a the/me-less Wine also serves for the common indices). */
static uint32_t u32_syscolor(int i) {
    switch (i) {
    case 0:  return 0x808080;   /* COLOR_SCROLLBAR */
    case 1:  return 0x000000;   /* COLOR_BACKGROUND / DESKTOP */
    case 2:  return 0x800000;   /* COLOR_ACTIVECAPTION */
    case 3:  return 0x808080;   /* COLOR_INACTIVECAPTION */
    case 4:  return 0xC0C0C0;   /* COLOR_MENU */
    case 5:  return 0xFFFFFF;   /* COLOR_WINDOW */
    case 6:  return 0x000000;   /* COLOR_WINDOWFRAME */
    case 7:  return 0x000000;   /* COLOR_MENUTEXT */
    case 8:  return 0x000000;   /* COLOR_WINDOWTEXT */
    case 9:  return 0xFFFFFF;   /* COLOR_CAPTIONTEXT */
    case 10: return 0xC0C0C0;   /* COLOR_ACTIVEBORDER */
    case 11: return 0xC0C0C0;   /* COLOR_INACTIVEBORDER */
    case 12: return 0x808080;   /* COLOR_APPWORKSPACE */
    case 13: return 0x800000;   /* COLOR_HIGHLIGHT */
    case 14: return 0xFFFFFF;   /* COLOR_HIGHLIGHTTEXT */
    case 15: return 0xC0C0C0;   /* COLOR_BTNFACE / 3DFACE */
    case 16: return 0x808080;   /* COLOR_BTNSHADOW */
    case 17: return 0x808080;   /* COLOR_GRAYTEXT */
    case 18: return 0x000000;   /* COLOR_BTNTEXT */
    case 19: return 0xC0C0C0;   /* COLOR_INACTIVECAPTIONTEXT */
    case 20: return 0xFFFFFF;   /* COLOR_BTNHIGHLIGHT */
    default: return 0x000000;
    }
}
uint32_t aret_GetSysColor(uint32_t esp) { return u32_syscolor(WI(0)); }
/* GetSysColorBrush(nIndex) -> HBRUSH (cached solid brush of that colour). */
static uint32_t g_gdi_syscolorbrush[32];
uint32_t aret_GetSysColorBrush(uint32_t esp) {
    int id = WI(0); if (id < 0 || id >= 32) return 0;
    if (!g_gdi_syscolorbrush[id]) {
        int i = gdi_alloc(GDIT_BRUSH); if (!i) return 0;
        g_gdi[i].stock = 1; g_gdi[i].color = u32_syscolor(id);
        g_gdi_syscolorbrush[id] = gdi_handle(i);
    }
    return g_gdi_syscolorbrush[id];
}
/* GetDeviceCaps(hdc, index) -> int. Fixed device-class metrics exact; the
 * environment-dependent screen extents use ARET's virtual desktop (tested by
 * invariant, not raw, like GetSystemMetrics — doc 72 §4.5). */
uint32_t aret_GetDeviceCaps(uint32_t esp) {
    switch (WI(1)) {
    case 2:  return 1;            /* TECHNOLOGY = DT_RASDISPLAY */
    case 8:  return U32_SCREEN_W; /* HORZRES */
    case 10: return U32_SCREEN_H; /* VERTRES */
    case 12: return 32;           /* BITSPIXEL */
    case 14: return 1;            /* PLANES */
    case 88: return 96;           /* LOGPIXELSX */
    case 90: return 96;           /* LOGPIXELSY */
    case 24: return -1;           /* NUMCOLORS (truecolor) */
    case 104: return 1;           /* SIZEPALETTE-ish / COLORMGMTCAPS default */
    case 4:  return 320;          /* HORZSIZE (mm, ~96dpi) */
    case 6:  return 240;          /* VERTSIZE (mm) */
    default: return 0;
    }
}

/* ================================================================== */
/* USER32 window helpers (display-free, measured against Wine)         */
/* ================================================================== */
static uint32_t g_u32_focus, g_u32_active;

/* GetClientRect(hwnd, RECT*) -> BOOL. Client area = {0,0,w,h}. Our windows have
 * no non-client decoration (display-free), so client == window extent — exact for
 * borderless windows (Wine: WS_POPUP client = {0,0,w,h}). */
uint32_t aret_GetClientRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(1);
    if (!r) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { r[0] = r[1] = r[2] = r[3] = 0; return 0; }
    r[0] = 0; r[1] = 0; r[2] = g_u32_win[i].w; r[3] = g_u32_win[i].h;
    return 1;
}
/* AdjustWindowRect(RECT*, style, bMenu) / …Ex -> BOOL. No non-client area is
 * modelled, so the rect is left unchanged (exact for borderless; Wine leaves a
 * WS_POPUP rect unchanged). */
uint32_t aret_AdjustWindowRect(uint32_t esp)   { return WP(0) ? 1u : 0u; }
uint32_t aret_AdjustWindowRectEx(uint32_t esp) { return WP(0) ? 1u : 0u; }

/* Focus / activation: tracked so Get* round-trips Set*; no real input focus. */
uint32_t aret_SetFocus(uint32_t esp)         { uint32_t p = g_u32_focus;  g_u32_focus = WU(0);  return p; }
uint32_t aret_GetFocus(uint32_t esp)         { (void)esp; return g_u32_focus; }
uint32_t aret_SetActiveWindow(uint32_t esp)  { uint32_t p = g_u32_active; g_u32_active = WU(0); return p; }
uint32_t aret_GetActiveWindow(uint32_t esp)  { (void)esp; return g_u32_active; }
uint32_t aret_SetForegroundWindow(uint32_t esp) { return u32_win_idx(WU(0)) >= 0 ? 1u : 0u; }
uint32_t aret_GetForegroundWindow(uint32_t esp) { (void)esp; return g_u32_active; }
uint32_t aret_BringWindowToTop(uint32_t esp)    { return u32_win_idx(WU(0)) >= 0 ? 1u : 0u; }

/* Paint invalidation. The update region is coalesced to a whole-client dirty flag
 * (WM_PAINT unions regions anyway); a partial rect still yields one WM_PAINT.
 * InvalidateRect(NULL,...) invalidates every top-level window. */
uint32_t aret_InvalidateRect(uint32_t esp) {
    uint32_t hwnd = WU(0);
    int erase = WU(2) != 0;                        /* bErase */
    if (hwnd == 0) { for (int i = 0; i < U32_MAX_WIN; i++) if (u32_win_paints(i)) { g_u32_win[i].needs_paint = 1; if (erase) g_u32_win[i].needs_erase = 1; } return 1; }
    int i = u32_win_idx(hwnd); if (i >= 0 && u32_win_paints(i)) { g_u32_win[i].needs_paint = 1; if (erase) g_u32_win[i].needs_erase = 1; }
    return 1;
}
uint32_t aret_InvalidateRgn(uint32_t esp)  { return aret_InvalidateRect(esp); }
uint32_t aret_ValidateRect(uint32_t esp)   {
    int i = u32_win_idx(WU(0)); if (i >= 0) g_u32_win[i].needs_paint = 0;
    return 1;
}
uint32_t aret_ValidateRgn(uint32_t esp)    { return aret_ValidateRect(esp); }
/* MessageBeep(uType) -> BOOL. No audio device; the call succeeds. */
uint32_t aret_MessageBeep(uint32_t esp) { (void)esp; return 1; }

/* CallWindowProcA/W(lpPrevWndFunc, hWnd, Msg, wParam, lParam) -> LRESULT. Invoke a
 * (lifted) window procedure directly — the subclassing idiom's call to the original
 * proc. */
uint32_t aret_CallWindowProcA(uint32_t esp) {
    uint32_t proc = WU(0);
    if (!proc) return 0;
    return u32_call_wndproc(esp, proc, WU(1), WU(2), WU(3), WU(4));
}
uint32_t aret_CallWindowProcW(uint32_t esp) { return aret_CallWindowProcA(esp); }

/* LoadCursorA/W, LoadIconA/W(hInstance, lpName) -> opaque non-null handle. Cursors
 * and icons are display-only; a program uses the handle as an opaque token (a
 * WNDCLASS field, SetClassLong), never dereferencing it headless. Distinct per
 * (kind, name) and non-null (matches Wine's "found" invariant). */
uint32_t aret_LoadCursorA(uint32_t esp) { return 0xCC000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadCursorW(uint32_t esp) { return 0xCC000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadIconA(uint32_t esp)   { return 0xCD000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadIconW(uint32_t esp)   { return 0xCD000000u | (WU(1) & 0x00FFFFFFu); }

/* MsgWaitForMultipleObjects(nCount, pHandles, bWaitAll, dwMs, dwWakeMask) -> DWORD.
 * A message available -> WAIT_OBJECT_0 + nCount; else WAIT_TIMEOUT (0x102). Same
 * model as the Ex variant (we read only nCount). */
uint32_t aret_MsgWaitForMultipleObjects(uint32_t esp) {
    uint32_t nCount = WU(0);
    u32_pump_timers();
    if (!u32_q_empty() || g_u32_quit) return nCount;
    return 0x00000102u;   /* WAIT_TIMEOUT */
}

/* ================================================================== */
/* USER32 menus — data model (display-free)                           */
/* ================================================================== */
/* A menu is a list of items (id, flags, optional submenu, text). No pixels: the
 * model backs the state APIs (Enable/Check/GetMenuState/GetMenuString) that a
 * program drives; painting a menu bar is display work (later). Handles: base
 * 0x40000000 (distinct from window/GDI/menu ranges). MF_* flags: GRAYED=1,
 * DISABLED=2, BITMAP=4, CHECKED=8, POPUP=0x10, OWNERDRAW=0x100, BYPOSITION=0x400,
 * SEPARATOR=0x800. */
#define U32_MENU_BASE 0x40000000u
#define U32_MAX_MENUS 128
#define U32_MAX_MITEMS 64
static struct u32_menu {
    int used, count;
    struct { uint32_t id, flags, submenu; char text[128]; } it[U32_MAX_MITEMS];
} g_u32_menu[U32_MAX_MENUS];
static uint32_t g_u32_wmenu[U32_MAX_WIN], g_u32_wsysmenu[U32_MAX_WIN];

static int u32_menu_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_MENU_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < U32_MAX_MENUS && g_u32_menu[i].used) ? (int)i : -1;
}
static uint32_t u32_menu_new(void) {
    for (int i = 1; i < U32_MAX_MENUS; i++)
        if (!g_u32_menu[i].used) { memset(&g_u32_menu[i], 0, sizeof g_u32_menu[i]); g_u32_menu[i].used = 1; return U32_MENU_BASE | (uint32_t)i; }
    return 0;
}
/* Find an item by command id (default) or by position (MF_BYPOSITION). */
static int u32_menu_find(struct u32_menu *m, uint32_t item, uint32_t flags) {
    if (flags & 0x400u) return (item < (uint32_t)m->count) ? (int)item : -1;
    for (int k = 0; k < m->count; k++) if (m->it[k].id == item) return k;
    return -1;
}
static void u32_menu_setitem(int mi, int slot, uint32_t flags, uint32_t idOrSub, const char *text) {
    struct u32_menu *m = &g_u32_menu[mi];
    m->it[slot].flags = flags & ~0x400u;                 /* MF_BYPOSITION is a lookup bit */
    m->it[slot].id = idOrSub;
    m->it[slot].submenu = (flags & 0x10u) ? idOrSub : 0; /* MF_POPUP: id is the submenu */
    m->it[slot].text[0] = 0;
    if (text && !(flags & (0x800u | 0x4u | 0x100u))) {   /* not separator/bitmap/ownerdraw */
        int k = 0; for (; text[k] && k < 127; k++) m->it[slot].text[k] = text[k];
        m->it[slot].text[k] = 0;
    }
}

uint32_t aret_CreateMenu(uint32_t esp)      { (void)esp; return u32_menu_new(); }
uint32_t aret_CreatePopupMenu(uint32_t esp) { (void)esp; return u32_menu_new(); }
uint32_t aret_DestroyMenu(uint32_t esp)     { int i = u32_menu_idx(WU(0)); if (i < 0) return 0; g_u32_menu[i].used = 0; return 1; }

uint32_t aret_AppendMenuA(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    int s = g_u32_menu[i].count++;
    u32_menu_setitem(i, s, WU(1), WU(2), WCS(3));
    return 1;
}
uint32_t aret_AppendMenuW(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    char t[128]; t[0] = 0;
    if (!(WU(1) & (0x800u | 0x4u | 0x100u))) u32_w2n((const uint16_t *)WP(3), t, sizeof t);
    int s = g_u32_menu[i].count++;
    u32_menu_setitem(i, s, WU(1), WU(2), t);
    return 1;
}
uint32_t aret_InsertMenuA(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    struct u32_menu *m = &g_u32_menu[i];
    int at = u32_menu_find(m, WU(1), WU(2)); if (at < 0) at = m->count;
    for (int k = m->count; k > at; k--) m->it[k] = m->it[k - 1];
    m->count++;
    u32_menu_setitem(i, at, WU(2), WU(3), WCS(4));
    return 1;
}
uint32_t aret_DeleteMenu(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    struct u32_menu *m = &g_u32_menu[i];
    int at = u32_menu_find(m, WU(1), WU(2)); if (at < 0) return 0;
    for (int k = at; k < m->count - 1; k++) m->it[k] = m->it[k + 1];
    m->count--;
    return 1;
}
uint32_t aret_RemoveMenu(uint32_t esp) { return aret_DeleteMenu(esp); }
uint32_t aret_GetMenuItemCount(uint32_t esp) { int i = u32_menu_idx(WU(0)); return i < 0 ? 0xFFFFFFFFu : (uint32_t)g_u32_menu[i].count; }

uint32_t aret_EnableMenuItem(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t prev = g_u32_menu[i].it[s].flags & 0x3u;    /* MF_GRAYED|MF_DISABLED */
    g_u32_menu[i].it[s].flags = (g_u32_menu[i].it[s].flags & ~0x3u) | (WU(2) & 0x3u);
    return prev;
}
uint32_t aret_CheckMenuItem(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t prev = g_u32_menu[i].it[s].flags & 0x8u;    /* MF_CHECKED */
    g_u32_menu[i].it[s].flags = (g_u32_menu[i].it[s].flags & ~0x8u) | (WU(2) & 0x8u);
    return prev;
}
uint32_t aret_GetMenuState(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t f = g_u32_menu[i].it[s].flags;
    if (f & 0x10u) {                                     /* popup: hiword = submenu count */
        int sub = u32_menu_idx(g_u32_menu[i].it[s].submenu);
        uint32_t cnt = sub >= 0 ? (uint32_t)g_u32_menu[sub].count : 0;
        return (cnt << 16) | (f & 0xFFFFu);
    }
    return f & 0xFFFFu;
}
uint32_t aret_GetMenuStringA(uint32_t esp) {
    char *buf = (char *)WP(2); uint32_t cch = WU(3);
    int i = u32_menu_idx(WU(0)); if (i < 0) { if (buf && cch) buf[0] = 0; return 0; }
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(4)); if (s < 0) { if (buf && cch) buf[0] = 0; return 0; }
    const char *t = g_u32_menu[i].it[s].text; uint32_t len = (uint32_t)strlen(t);
    if (!buf || cch == 0) return len;
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = t[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetMenuStringW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(2); uint32_t cch = WU(3);
    int i = u32_menu_idx(WU(0)); if (i < 0) { if (buf && cch) buf[0] = 0; return 0; }
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(4)); if (s < 0) { if (buf && cch) buf[0] = 0; return 0; }
    const char *t = g_u32_menu[i].it[s].text; uint32_t len = (uint32_t)strlen(t);
    if (!buf || cch == 0) return len;
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)t[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetSubMenu(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    int p = WI(1); if (p < 0 || p >= g_u32_menu[i].count) return 0;
    return (g_u32_menu[i].it[p].flags & 0x10u) ? g_u32_menu[i].it[p].submenu : 0;
}
uint32_t aret_GetMenuItemID(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int p = WI(1); if (p < 0 || p >= g_u32_menu[i].count) return 0xFFFFFFFFu;
    if (g_u32_menu[i].it[p].flags & 0x10u) return 0xFFFFFFFFu;  /* popup -> -1 */
    return g_u32_menu[i].it[p].id;
}

/* Window menu bar + system menu. */
uint32_t aret_GetMenu(uint32_t esp) { int i = u32_win_idx(WU(0)); return i < 0 ? 0 : g_u32_wmenu[i]; }
uint32_t aret_SetMenu(uint32_t esp) { int i = u32_win_idx(WU(0)); if (i < 0) return 0; g_u32_wmenu[i] = WU(1); return 1; }
uint32_t aret_GetSystemMenu(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    if (WU(1)) {                                         /* bRevert: reset, return NULL */
        if (g_u32_wsysmenu[i]) { int mi = u32_menu_idx(g_u32_wsysmenu[i]); if (mi >= 0) g_u32_menu[mi].used = 0; g_u32_wsysmenu[i] = 0; }
        return 0;
    }
    if (!g_u32_wsysmenu[i]) {
        uint32_t h = u32_menu_new(); int mi = u32_menu_idx(h);
        if (mi >= 0) {
            static const struct { uint32_t id; const char *t; } sc[] = {
                {0xF120u, "&Restore"}, {0xF010u, "&Move"}, {0xF000u, "&Size"},
                {0xF020u, "Mi&nimize"}, {0xF030u, "Ma&ximize"}, {0xF060u, "&Close"} };
            for (int k = 0; k < 6; k++) { int s = g_u32_menu[mi].count++; u32_menu_setitem(mi, s, 0, sc[k].id, sc[k].t); }
        }
        g_u32_wsysmenu[i] = h;
    }
    return g_u32_wsysmenu[i];
}
/* TrackPopupMenu(Ex): a modal popup needs a display + user; headless there is no
 * selection -> return 0 (with TPM_RETURNCMD, 0 = "no item chosen"). */
uint32_t aret_TrackPopupMenu(uint32_t esp)   { (void)esp; return 0; }
uint32_t aret_TrackPopupMenuEx(uint32_t esp) { (void)esp; return 0; }

/* ================================================================== */
/* Misc: Global/Local lock+realloc, GetObject, FindWindow, TZ, StdHandle */
/* ================================================================== */
/* GMEM_FIXED handles are the malloc pointer itself (see GlobalAlloc), so Lock is
 * identity and Unlock succeeds; ReAlloc is realloc (GMEM_MOVEABLE may move). */
uint32_t aret_GlobalLock(uint32_t esp)    { return WU(0); }
uint32_t aret_GlobalUnlock(uint32_t esp)  { (void)esp; return 1; }
uint32_t aret_GlobalReAlloc(uint32_t esp) { return WRP(realloc(WP(0), WU(1))); }
uint32_t aret_LocalLock(uint32_t esp)     { return WU(0); }
uint32_t aret_LocalUnlock(uint32_t esp)   { (void)esp; return 1; }
uint32_t aret_LocalReAlloc(uint32_t esp)  { return WRP(realloc(WP(0), WU(1))); }

/* GetObjectA/W(hgdiobj, cb, lpv) -> bytes written. Fills BITMAP (24 bytes) for a
 * bitmap, LOGBRUSH (12) for a brush. Other objects: 0 (sound). */
uint32_t aret_GetObjectA(uint32_t esp) {
    int i = gdi_idx(WU(0)); int cb = WI(1); uint8_t *lpv = (uint8_t *)WP(2);
    if (i < 0 || !lpv) return 0;
    if (g_gdi[i].type == GDIT_BITMAP) {
        if (cb < 24) return 0;
        int32_t *w = (int32_t *)lpv;
        w[0] = 0;                                    /* bmType */
        w[1] = g_gdi[i].w;                           /* bmWidth */
        w[2] = g_gdi[i].h;                           /* bmHeight */
        w[3] = ((g_gdi[i].w * g_gdi[i].bpp + 15) / 16) * 2;  /* bmWidthBytes (WORD-aligned) */
        *(uint16_t *)(lpv + 16) = 1;                 /* bmPlanes */
        *(uint16_t *)(lpv + 18) = (uint16_t)g_gdi[i].bpp;    /* bmBitsPixel */
        *(uint32_t *)(lpv + 20) = (uint32_t)(uintptr_t)g_gdi[i].bits;  /* bmBits */
        return 24;
    }
    if (g_gdi[i].type == GDIT_BRUSH) {
        if (cb < 12) return 0;
        *(uint32_t *)(lpv + 0) = g_gdi[i].null_obj ? 1u : 0u;   /* lbStyle: BS_SOLID=0 / BS_NULL=1 */
        *(uint32_t *)(lpv + 4) = g_gdi[i].color;               /* lbColor */
        *(uint32_t *)(lpv + 8) = 0;                            /* lbHatch */
        return 12;
    }
    return 0;
}
uint32_t aret_GetObjectW(uint32_t esp) { return aret_GetObjectA(esp); }

/* FindWindowA/W(lpClassName, lpWindowName) -> HWND. Matched by window title (our
 * windows don't store a class name; a class-only query -> 0, which is also the
 * correct "no such window" for the common single-instance check). */
uint32_t aret_FindWindowA(uint32_t esp) {
    const char *cls = WCS(0); const char *title = WCS(1);
    if (cls && !title) return 0;                     /* class-only: cannot match, none found */
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) continue;
        if (title && strcmp(g_u32_win[i].title, title) != 0) continue;
        return (uint32_t)(i + 1);
    }
    return 0;
}
uint32_t aret_FindWindowW(uint32_t esp) {
    char title[256]; const char *tp = NULL;
    if (WP(1)) { u32_w2n((const uint16_t *)WP(1), title, sizeof title); tp = title; }
    if (WP(0) && !tp) return 0;
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) continue;
        if (tp && strcmp(g_u32_win[i].title, tp) != 0) continue;
        return (uint32_t)(i + 1);
    }
    return 0;
}

/* GetTimeZoneInformation(TIME_ZONE_INFORMATION*) -> DWORD. ARET runs on a defined
 * UTC clock (tests fix TZ=UTC): zero the struct (Bias=0 => UTC, no DST) and report
 * TIME_ZONE_ID_UNKNOWN. */
uint32_t aret_GetTimeZoneInformation(uint32_t esp) {
    uint8_t *tzi = (uint8_t *)WP(0);
    if (tzi) memset(tzi, 0, 172);                    /* Bias + Std/Daylight name/date/bias */
    return 0;                                        /* TIME_ZONE_ID_UNKNOWN */
}
/* SetStdHandle(nStdHandle, hHandle) -> BOOL. Redirect a std stream (fd model). */
uint32_t aret_SetStdHandle(uint32_t esp) {
    int32_t which = WI(0);
    int slot = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
    if (slot < 0) return 0;
    dup2((int)WU(1), slot);
    return 1;
}

/* ================================================================== */
/* Long-tail: FileTimeToSystemTime, GetDriveType, CharNext/Prev, coords */
/* ================================================================== */
/* FileTimeToSystemTime(const FILETIME*, SYSTEMTIME*) -> BOOL. Reverse of
 * SystemTimeToFileTime; civil date via gmtime (matches Wine). */
uint32_t aret_FileTimeToSystemTime(uint32_t esp) {
    const uint32_t *ft = (const uint32_t *)WP(0);
    uint16_t *w = (uint16_t *)WP(1);
    if (!ft || !w) return 0;
    uint64_t t = ((uint64_t)ft[1] << 32) | ft[0];
    uint32_t ms = (uint32_t)((t / 10000ull) % 1000ull);
    time_t sec = (time_t)(t / 10000000ull) - 11644473600ll;   /* 1601 -> 1970 epoch */
    struct tm tmv; gmtime_r(&sec, &tmv);
    w[0] = (uint16_t)(tmv.tm_year + 1900); w[1] = (uint16_t)(tmv.tm_mon + 1);
    w[2] = (uint16_t)tmv.tm_wday;          w[3] = (uint16_t)tmv.tm_mday;
    w[4] = (uint16_t)tmv.tm_hour;          w[5] = (uint16_t)tmv.tm_min;
    w[6] = (uint16_t)tmv.tm_sec;           w[7] = (uint16_t)ms;
    return 1;
}
/* GetDriveTypeA(lpRootPathName) -> UINT. Everything maps to the host filesystem,
 * so a drive root is DRIVE_FIXED (3) — the common case (C:). */
uint32_t aret_GetDriveTypeA(uint32_t esp) { (void)esp; return 3; }
uint32_t aret_GetDriveTypeW(uint32_t esp) { (void)esp; return 3; }

/* CharNextA/W(psz) -> the next character (single-byte / wide; at NUL, stays). */
uint32_t aret_CharNextA(uint32_t esp) { const char *p = WCS(0); return (!p || !*p) ? WU(0) : WU(0) + 1; }
uint32_t aret_CharNextW(uint32_t esp) { const uint16_t *p = (const uint16_t *)WP(0); return (!p || !*p) ? WU(0) : WU(0) + 2; }
/* CharPrevA/W(lpszStart, lpszCurrent) -> the previous character (bounded at start). */
uint32_t aret_CharPrevA(uint32_t esp) { uint32_t s = WU(0), c = WU(1); return c > s ? c - 1 : s; }
uint32_t aret_CharPrevW(uint32_t esp) { uint32_t s = WU(0), c = WU(1); return c > s + 1 ? c - 2 : s; }

/* ClientToScreen/ScreenToClient(hwnd, POINT*) -> BOOL. Client origin = the window
 * origin (no non-client area modelled), so the mapping is a translate by (x,y). */
uint32_t aret_ClientToScreen(uint32_t esp) {
    int i = u32_win_idx(WU(0)); int32_t *pt = (int32_t *)WP(1);
    if (i < 0 || !pt) return 0;
    pt[0] += g_u32_win[i].x; pt[1] += g_u32_win[i].y;
    return 1;
}
uint32_t aret_ScreenToClient(uint32_t esp) {
    int i = u32_win_idx(WU(0)); int32_t *pt = (int32_t *)WP(1);
    if (i < 0 || !pt) return 0;
    pt[0] -= g_u32_win[i].x; pt[1] -= g_u32_win[i].y;
    return 1;
}

/* ================================================================== */
/* Long-tail tail: CreateBitmap, cursor/popup, registry delete         */
/* ================================================================== */
/* CreateBitmap(w, h, planes, bpp, lpBits) -> HBITMAP. A device-dependent bitmap;
 * we back it with a 32-bit buffer (drawing is only pixel-exact for 32bpp) but
 * report the requested bpp/dims via GetObject. */
uint32_t aret_CreateBitmap(uint32_t esp) {
    int w = WI(0), h = WI(1), bpp = WI(3);
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = 1; g_gdi[i].bpp = bpp ? bpp : 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    return gdi_handle(i);
}

/* Cursor: a display-count model matching Wine (initial 0 with a mouse present).
 * SetCursor returns the previous cursor; the actual cursor image is display work. */
static int g_u32_cursor_show;
static uint32_t g_u32_cursor;
uint32_t aret_ShowCursor(uint32_t esp) { g_u32_cursor_show += WU(0) ? 1 : -1; return (uint32_t)g_u32_cursor_show; }
uint32_t aret_SetCursor(uint32_t esp)  { uint32_t p = g_u32_cursor; g_u32_cursor = WU(0); return p; }
uint32_t aret_GetCursor(uint32_t esp)  { (void)esp; return g_u32_cursor; }
/* GetLastActivePopup(hWnd) -> hWnd (no popup owned -> the window itself). */
uint32_t aret_GetLastActivePopup(uint32_t esp) { return WU(0); }

/* Registry deletes on the empty read-only hive: the value/key never existed ->
 * ERROR_FILE_NOT_FOUND (2), consistent with RegOpenKeyEx/RegQueryValueEx. */
uint32_t aret_RegDeleteValueA(uint32_t esp) { (void)esp; return 2; }
uint32_t aret_RegDeleteValueW(uint32_t esp) { (void)esp; return 2; }
uint32_t aret_RegDeleteKeyA(uint32_t esp)   { (void)esp; return 2; }
uint32_t aret_RegDeleteKeyW(uint32_t esp)   { (void)esp; return 2; }

/* ================================================================== */
/* advapi32 — SID / token model (structural)                          */
/* ================================================================== */
/* A SID is Revision(1) + SubAuthorityCount(1) + IdentifierAuthority(6) +
 * SubAuthority[N]*4. The structural APIs (build/compare/length/subauthorities)
 * are exact and oracle-able; token contents (the user's SID) are environment-
 * dependent, so those calls are modelled soundly but not bit-compared. */
static int u32_sid_len(const uint8_t *sid) { return 8 + 4 * sid[1]; }

uint32_t aret_AllocateAndInitializeSid(uint32_t esp) {
    const uint8_t *auth = (const uint8_t *)WP(0);    /* SID_IDENTIFIER_AUTHORITY */
    uint32_t count = WU(1) & 0xFF;
    uint32_t *pSid = (uint32_t *)WP(10);
    if (!auth || !pSid || count > 8) return 0;
    uint8_t *sid = (uint8_t *)malloc(8 + 4 * count);
    if (!sid) return 0;
    sid[0] = 1; sid[1] = (uint8_t)count;
    memcpy(sid + 2, auth, 6);
    for (uint32_t i = 0; i < count; i++) *(uint32_t *)(sid + 8 + 4 * i) = WU(2 + i);
    *pSid = (uint32_t)(uintptr_t)sid;
    return 1;
}
uint32_t aret_InitializeSid(uint32_t esp) {
    uint8_t *sid = (uint8_t *)WP(0); const uint8_t *auth = (const uint8_t *)WP(1);
    if (!sid || !auth) return 0;
    sid[0] = 1; sid[1] = (uint8_t)(WU(2) & 0xFF); memcpy(sid + 2, auth, 6);
    return 1;
}
uint32_t aret_GetLengthSid(uint32_t esp) { const uint8_t *s = (const uint8_t *)WP(0); return s ? (uint32_t)u32_sid_len(s) : 0; }
uint32_t aret_GetSidLengthRequired(uint32_t esp) { return 8 + 4 * (WU(0) & 0xFF); }
uint32_t aret_IsValidSid(uint32_t esp) { const uint8_t *s = (const uint8_t *)WP(0); return (s && s[0] == 1 && s[1] <= 15) ? 1u : 0u; }
uint32_t aret_EqualSid(uint32_t esp) {
    const uint8_t *a = (const uint8_t *)WP(0), *b = (const uint8_t *)WP(1);
    if (!a || !b) return 0;
    int la = u32_sid_len(a);
    return (la == u32_sid_len(b) && memcmp(a, b, la) == 0) ? 1u : 0u;
}
uint32_t aret_GetSidSubAuthorityCount(uint32_t esp)  { return WU(0) ? WU(0) + 1 : 0; }        /* &sid[1] */
uint32_t aret_GetSidSubAuthority(uint32_t esp)       { return WU(0) ? WU(0) + 8 + 4 * WU(1) : 0; }
uint32_t aret_GetSidIdentifierAuthority(uint32_t esp) { return WU(0) ? WU(0) + 2 : 0; }
uint32_t aret_FreeSid(uint32_t esp) { free(WP(0)); return 0; }
uint32_t aret_CopySid(uint32_t esp) {
    uint32_t cap = WU(0); uint8_t *dst = (uint8_t *)WP(1); const uint8_t *src = (const uint8_t *)WP(2);
    if (!dst || !src) return 0;
    int n = u32_sid_len(src);
    if ((uint32_t)n > cap) return 0;
    memcpy(dst, src, n);
    return 1;
}

/* Tokens: a single-user, non-elevated process model. A token handle is opaque. */
uint32_t aret_OpenProcessToken(uint32_t esp) { uint32_t *pt = (uint32_t *)WP(2); if (pt) *pt = 0x2A2A2A2Au; return 1; }
uint32_t aret_OpenThreadToken(uint32_t esp)  { uint32_t *pt = (uint32_t *)WP(3); if (pt) *pt = 0x2A2A2A2Au; return 1; }
uint32_t aret_AdjustTokenPrivileges(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_LookupPrivilegeValueA(uint32_t esp) {
    uint32_t *luid = (uint32_t *)WP(2);              /* LUID {LowPart, HighPart} */
    if (luid) { luid[0] = 0x13; luid[1] = 0; }
    return 1;
}
uint32_t aret_LookupPrivilegeValueW(uint32_t esp) { return aret_LookupPrivilegeValueA(esp); }
/* GetTokenInformation: model TokenElevation (not elevated — the common UAC check).
 * Unhandled classes return FALSE (honest: we don't model that token data). */
uint32_t aret_GetTokenInformation(uint32_t esp) {
    uint32_t cls = WU(1); uint8_t *buf = (uint8_t *)WP(2); uint32_t len = WU(3); uint32_t *ret = (uint32_t *)WP(4);
    if (cls == 20 /* TokenElevation */) { if (buf && len >= 4) *(uint32_t *)buf = 0; if (ret) *ret = 4; return 1; }
    if (cls == 18 /* TokenElevationType */) { if (buf && len >= 4) *(uint32_t *)buf = 1; if (ret) *ret = 4; return 1; } /* Default */
    return 0;                                        /* class not modelled */
}

/* ================================================================== */
/* Rect math, char case, pointer/input/path stubs                     */
/* ================================================================== */
uint32_t aret_SetRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); if (!r) return 0;
    r[0] = WI(1); r[1] = WI(2); r[2] = WI(3); r[3] = WI(4); return 1;
}
uint32_t aret_SetRectEmpty(uint32_t esp) { int32_t *r = (int32_t *)WP(0); if (!r) return 0; r[0]=r[1]=r[2]=r[3]=0; return 1; }
uint32_t aret_CopyRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *s = (const int32_t *)WP(1);
    if (!d || !s) return 0; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return 1;
}
uint32_t aret_IsRectEmpty(uint32_t esp) {
    const int32_t *r = (const int32_t *)WP(0);
    return (!r || r[0] >= r[2] || r[1] >= r[3]) ? 1u : 0u;
}
uint32_t aret_EqualRect(uint32_t esp) {
    const int32_t *a = (const int32_t *)WP(0), *b = (const int32_t *)WP(1);
    if (!a || !b) return 0;
    return (a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]) ? 1u : 0u;
}
uint32_t aret_PtInRect(uint32_t esp) {
    const int32_t *r = (const int32_t *)WP(0); int32_t x = WI(1), y = WI(2);
    if (!r) return 0;
    return (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) ? 1u : 0u;
}
uint32_t aret_OffsetRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); int32_t dx = WI(1), dy = WI(2);
    if (!r) return 0; r[0]+=dx; r[2]+=dx; r[1]+=dy; r[3]+=dy; return 1;
}
uint32_t aret_InflateRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); int32_t dx = WI(1), dy = WI(2);
    if (!r) return 0; r[0]-=dx; r[2]+=dx; r[1]-=dy; r[3]+=dy; return 1;
}
uint32_t aret_IntersectRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *a = (const int32_t *)WP(1), *b = (const int32_t *)WP(2);
    if (!d || !a || !b) return 0;
    int32_t l = a[0] > b[0] ? a[0] : b[0], t = a[1] > b[1] ? a[1] : b[1];
    int32_t r = a[2] < b[2] ? a[2] : b[2], bo = a[3] < b[3] ? a[3] : b[3];
    if (l >= r || t >= bo) { d[0]=d[1]=d[2]=d[3]=0; return 0; }
    d[0]=l; d[1]=t; d[2]=r; d[3]=bo; return 1;
}
uint32_t aret_UnionRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *a = (const int32_t *)WP(1), *b = (const int32_t *)WP(2);
    if (!d || !a || !b) return 0;
    int ae = (a[0] >= a[2] || a[1] >= a[3]), be = (b[0] >= b[2] || b[1] >= b[3]);
    if (ae && be) { d[0]=d[1]=d[2]=d[3]=0; return 0; }
    if (ae) { d[0]=b[0]; d[1]=b[1]; d[2]=b[2]; d[3]=b[3]; return 1; }
    if (be) { d[0]=a[0]; d[1]=a[1]; d[2]=a[2]; d[3]=a[3]; return 1; }
    d[0] = a[0] < b[0] ? a[0] : b[0]; d[1] = a[1] < b[1] ? a[1] : b[1];
    d[2] = a[2] > b[2] ? a[2] : b[2]; d[3] = a[3] > b[3] ? a[3] : b[3];
    return 1;
}

/* CharUpper/Lower A/W: a string pointer (in place), or — if the high word is 0 —
 * a single character to convert (returned in the low byte). */
uint32_t aret_CharUpperA(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(unsigned char)toupper((int)(p & 0xFF));
    char *s = (char *)(uintptr_t)p; for (; *s; s++) *s = (char)toupper((unsigned char)*s);
    return p;
}
uint32_t aret_CharLowerA(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(unsigned char)tolower((int)(p & 0xFF));
    char *s = (char *)(uintptr_t)p; for (; *s; s++) *s = (char)tolower((unsigned char)*s);
    return p;
}
uint32_t aret_CharUpperW(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(uint16_t)toupper((int)(p & 0xFF));
    uint16_t *s = (uint16_t *)(uintptr_t)p; for (; *s; s++) if (*s < 256) *s = (uint16_t)toupper(*s);
    return p;
}
uint32_t aret_CharLowerW(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(uint16_t)tolower((int)(p & 0xFF));
    uint16_t *s = (uint16_t *)(uintptr_t)p; for (; *s; s++) if (*s < 256) *s = (uint16_t)tolower(*s);
    return p;
}
uint32_t aret_CharUpperBuffA(uint32_t esp) {
    char *s = (char *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) s[i] = (char)toupper((unsigned char)s[i]);
    return n;
}
uint32_t aret_CharLowerBuffA(uint32_t esp) {
    char *s = (char *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) s[i] = (char)tolower((unsigned char)s[i]);
    return n;
}

/* Pointer validation: the guest runs natively with real pointers, so a non-null
 * pointer is valid (0 = "not bad"), like IsBadReadPtr. */
uint32_t aret_IsBadCodePtr(uint32_t esp)   { return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadStringPtrA(uint32_t esp){ return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadStringPtrW(uint32_t esp){ return (uint32_t)(WU(0) == 0); }

/* Input state: headless -> no keys/buttons pressed, cursor at origin. */
uint32_t aret_GetKeyState(uint32_t esp)      { (void)esp; return 0; }
uint32_t aret_GetAsyncKeyState(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_GetMessagePos(uint32_t esp)    { (void)esp; return 0; }
uint32_t aret_GetMessageTime(uint32_t esp)   { (void)esp; return (uint32_t)(mono_ns() / 1000000ull); }
/* GetWindowThreadProcessId(hwnd, *pid) -> tid. Single-process/thread model. */
uint32_t aret_GetWindowThreadProcessId(uint32_t esp) {
    uint32_t *pid = (uint32_t *)WP(1); if (pid) *pid = (uint32_t)getpid();
    return 1;   /* thread id */
}

/* ================================================================== */
/* GetClassName, fonts, IsDialogMessage                               */
/* ================================================================== */
/* GetClassNameA/W(hWnd, lpClassName, nMaxCount) -> chars copied. */
uint32_t aret_GetClassNameA(uint32_t esp) {
    char *buf = (char *)WP(1); uint32_t cch = WU(2);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *c = g_u32_win[i].classname; uint32_t len = (uint32_t)strlen(c);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = c[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetClassNameW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(1); uint32_t cch = WU(2);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *c = g_u32_win[i].classname; uint32_t len = (uint32_t)strlen(c);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)c[j];
    buf[n] = 0;
    return n;
}

/* Fonts: opaque GDI font objects (glyph rendering is display work; a program uses
 * the handle as a token — SelectObject/DeleteObject/GetObject-LOGFONT). */
uint32_t aret_CreateFontA(uint32_t esp)         { (void)esp; int i = gdi_alloc(GDIT_FONT); return i ? gdi_handle(i) : 0; }
uint32_t aret_CreateFontW(uint32_t esp)         { return aret_CreateFontA(esp); }
uint32_t aret_CreateFontIndirectA(uint32_t esp) { (void)esp; int i = gdi_alloc(GDIT_FONT); return i ? gdi_handle(i) : 0; }
uint32_t aret_CreateFontIndirectW(uint32_t esp) { return aret_CreateFontIndirectA(esp); }

/* IsDialogMessageA/W(hDlg, lpMsg) -> BOOL. Headless keyboard navigation has no
 * input to translate, so no message is consumed as a dialog message. */
uint32_t aret_IsDialogMessageA(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_IsDialogMessageW(uint32_t esp) { (void)esp; return 0; }

/* ================================================================== */
/* Registry (older forms) + Windows hooks — sound stubs               */
/* ================================================================== */
/* RegOpenKeyA(hKey, lpSubKey, phkResult) -> ERROR_FILE_NOT_FOUND (empty hive). */
uint32_t aret_RegOpenKeyA(uint32_t esp) { uint32_t *r = (uint32_t *)WP(2); if (r) *r = 0; return 2; }
/* RegQueryInfoKeyA(...) -> ERROR_SUCCESS with an empty key (0 subkeys/values). The
 * count out-params (args 4..) are left as the caller initialised them; we report
 * success so a caller that only checks the return proceeds over an empty key. */
uint32_t aret_RegQueryInfoKeyA(uint32_t esp) { (void)esp; return 0; }

/* Windows hooks: install accepted (opaque handle), CallNextHookEx passes through
 * (no next hook -> 0), unhook succeeds. No real hook chain in this model. */
uint32_t aret_SetWindowsHookExA(uint32_t esp)   { (void)esp; return 0x484F4F4Bu; }  /* opaque HHOOK */
uint32_t aret_SetWindowsHookExW(uint32_t esp)   { (void)esp; return 0x484F4F4Bu; }
uint32_t aret_UnhookWindowsHookEx(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_CallNextHookEx(uint32_t esp)      { (void)esp; return 0; }
