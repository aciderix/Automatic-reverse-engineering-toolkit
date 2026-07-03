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
uint32_t aret_SetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0), *val = WCS(1);
    if (!name) return 0;
    return (uint32_t)(val ? (setenv(name, val, 1) == 0) : (unsetenv(name) == 0));
}
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

/* AreFileApisANSI(): the process uses the ANSI code page for narrow file APIs,
 * the Windows default (Wine returns TRUE). */
uint32_t aret_AreFileApisANSI(uint32_t esp) { (void)esp; return 1; }

uint32_t aret_GetACP(uint32_t esp)   { (void)esp; return 1252; } /* Windows-1252 */
uint32_t aret_GetOEMCP(uint32_t esp) { (void)esp; return 437; }
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
