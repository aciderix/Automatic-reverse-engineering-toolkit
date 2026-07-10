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
#include <sys/ioctl.h>

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

#define U32_MAX_CLASSES 64
static struct { uint16_t name[128]; uint32_t wndproc; int used; } g_u32_class[U32_MAX_CLASSES];

#define U32_MAX_WIN 256
static struct { uint32_t wndproc; uint32_t parent; int used; } g_u32_win[U32_MAX_WIN];

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

/* RegisterClassW(const WNDCLASSW*) -> ATOM. Fields (32-bit): lpfnWndProc @+4,
 * lpszClassName @+36. Returns a non-zero atom; 0 on failure. */
uint32_t aret_RegisterClassW(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const uint16_t *name = (const uint16_t *)(uintptr_t)wc[9]; /* +36 */
    if (!name) return 0;
    for (int i = 0; i < U32_MAX_CLASSES; i++) {
        if (!g_u32_class[i].used) {
            g_u32_class[i].used = 1;
            g_u32_class[i].wndproc = wc[1];           /* +4 lpfnWndProc */
            u32_wcpy(g_u32_class[i].name, name, 128);
            return 0xC000u + (uint32_t)i;
        }
    }
    return 0;
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
/* CreateWindowExW(exStyle, className@1, winName, style, x,y,w,h, parent@8, menu, inst, param)
 * -> HWND. We support message-only (and any) windows as pure message sinks. */
uint32_t aret_CreateWindowExW(uint32_t esp) {
    uint32_t wndproc = u32_class_wndproc(WU(1));
    if (!wndproc) return 0;                 /* unknown class -> fail (sound) */
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) {
            g_u32_win[i].used = 1; g_u32_win[i].wndproc = wndproc; g_u32_win[i].parent = WU(8);
            return (uint32_t)(i + 1);       /* HWND = index+1 (NULL invalid) */
        }
    }
    return 0;
}
/* DestroyWindow(HWND) -> BOOL. */
uint32_t aret_DestroyWindow(uint32_t esp) {
    uint32_t h = WU(0);
    if (h >= 1 && h <= U32_MAX_WIN && g_u32_win[h - 1].used) g_u32_win[h - 1].used = 0;
    return 1;
}
/* DefWindowProcW(HWND,UINT,WPARAM,LPARAM) -> LRESULT. Message-only default = 0. */
uint32_t aret_DefWindowProcW(uint32_t esp) { (void)esp; return 0; }
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
    u32_pump_timers();
    if (!u32_q_empty()) { u32_q_peek_copy(m, (remove & U32_PM_REMOVE) != 0); return 1; }
    if (g_u32_quit)    { u32_fill_quit(m); return 1; }
    return 0;
}
/* GetMessageW(lpMsg, hwnd, min, max) -> BOOL (0 = WM_QUIT, else 1). Blocks until a
 * message is available. In the mono-thread model the only wakers are the queue
 * (already posted) and timers, so if the queue is empty with no quit and no timer
 * nothing can ever arrive -> we abort loudly rather than hang or fake a quit. */
uint32_t aret_GetMessageW(uint32_t esp) {
    uint32_t *m = (uint32_t *)WP(0);
    for (;;) {
        u32_pump_timers();
        if (!u32_q_empty()) { u32_q_peek_copy(m, 1); return 1; }
        if (g_u32_quit)    { u32_fill_quit(m); return 0; }
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
