/* ARET HLE runtime — source-OS API shims, implemented natively. See aret_hle.h
 * for the calling model. Each shim receives the modelled stack pointer `esp` and
 * reads its original arguments from the consecutive 32-bit words at esp+0, esp+4,
 * … We implement each call in terms of the native POSIX/libc of the target, so
 * the recompiled program runs natively on Linux. */

#include "aret_hle.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <fnmatch.h>
#ifndef __wasm__
#include <sys/mman.h> /* WASI has no real file mmap; file mapping is native-only */
#endif

/* Read stdcall/cdecl argument `i` (a 32-bit word) from the modelled stack. */
static inline uint32_t arg(uint32_t esp, int i) {
    return ((const uint32_t *)(uintptr_t)esp)[i];
}

/* Two consecutive slots as one 64-bit value (a cdecl varargs long long / double
 * occupies two slots, low word first). */
static inline uint64_t arg64(uint32_t esp, int i) {
    return ((uint64_t)arg(esp, i + 1) << 32) | (uint64_t)arg(esp, i);
}

/* stdio routing (defined with the CRT shims below; the file subsystem above uses
 * them to send writes to a synthetic stream to the right fd). */
static int file_fd(uint32_t file);
static void stdio_write(uint32_t file, const char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* kernel32.dll                                                        */
/* ------------------------------------------------------------------ */

/* Per-fiber while a fiber runs (the cooperative scheduler swaps this global in
 * and out at every context switch — see aret_win32.c). Non-static so the fiber
 * layer can save/restore it. */
uint32_t g_last_error = 0;

/* Map a Windows standard-handle constant to a POSIX file descriptor. */
static int std_fd(uint32_t nStdHandle) {
    switch ((int32_t)nStdHandle) {
        case -10: return 0; /* STD_INPUT_HANDLE  */
        case -11: return 1; /* STD_OUTPUT_HANDLE */
        case -12: return 2; /* STD_ERROR_HANDLE  */
        default:  return (int)nStdHandle;
    }
}

uint32_t aret_GetStdHandle(uint32_t esp) {
    return (uint32_t)std_fd(arg(esp, 0));
}

/* The 64-bit file offset carried by a Win32 OVERLAPPED (Offset @ +8, OffsetHigh
 * @ +12), or -1 when lpOverlapped is NULL (synchronous I/O at the file pointer).
 * sqlite's Win32 VFS (winRead/winWrite) ALWAYS passes the target offset this way
 * rather than via SetFilePointer, so honouring it is required to hit the right
 * page — ignoring it makes every read/write land at the file-pointer position
 * (page 0), so page N reads back page 0's bytes → "file is not a database" /
 * "database disk image is malformed". */
static off_t overlapped_offset(uint32_t lpOverlapped) {
    if (!lpOverlapped) return (off_t)-1;
    const uint32_t *ov = (const uint32_t *)(uintptr_t)lpOverlapped;
    return (off_t)(((uint64_t)ov[3] << 32) | ov[2]); /* OffsetHigh:Offset */
}

static uint32_t write_common(uint32_t esp) {
    int fd = std_fd(arg(esp, 0));
    const void *buf = (const void *)(uintptr_t)arg(esp, 1);
    uint32_t count = arg(esp, 2);
    uint32_t pdone = arg(esp, 3);
    off_t off = overlapped_offset(arg(esp, 4));
    ssize_t n = off >= 0 ? pwrite(fd, buf, count, off) : write(fd, buf, count);
    if (n < 0) {
        g_last_error = 5;
        if (pdone) *(uint32_t *)(uintptr_t)pdone = 0;
        return 0;
    }
    if (pdone) *(uint32_t *)(uintptr_t)pdone = (uint32_t)n;
    return 1;
}

uint32_t aret_WriteFile(uint32_t esp) { return write_common(esp); }
uint32_t aret_WriteConsoleA(uint32_t esp) { return write_common(esp); }

uint32_t aret_ReadFile(uint32_t esp) {
    int fd = std_fd(arg(esp, 0));
    void *buf = (void *)(uintptr_t)arg(esp, 1);
    uint32_t count = arg(esp, 2);
    uint32_t pdone = arg(esp, 3);
    off_t off = overlapped_offset(arg(esp, 4));
    ssize_t n = off >= 0 ? pread(fd, buf, count, off) : read(fd, buf, count);
    if (n < 0) {
        g_last_error = 5;
        if (pdone) *(uint32_t *)(uintptr_t)pdone = 0;
        return 0;
    }
    if (pdone) *(uint32_t *)(uintptr_t)pdone = (uint32_t)n;
    return 1;
}

uint32_t aret_ExitProcess(uint32_t esp) {
    exit((int)arg(esp, 0));
    return 0;
}

uint32_t aret_GetLastError(uint32_t esp) { (void)esp; return g_last_error; }
uint32_t aret_SetLastError(uint32_t esp) { g_last_error = arg(esp, 0); return 0; }

uint32_t aret_Sleep(uint32_t esp) {
    /* With cooperative threads live, Sleep blocks on the deterministic virtual clock
     * (letting other fibers run); aret_fiber_sleep returns 0 when no threads exist,
     * so a single-threaded program keeps the exact real usleep it had before. */
    uint32_t ms = arg(esp, 0);
    if (!aret_fiber_sleep(ms)) usleep((useconds_t)ms * 1000u);
    return 0;
}

uint32_t aret_GetCurrentProcessId(uint32_t esp) { (void)esp; return (uint32_t)getpid(); }

/* ------------------------------------------------------------------ */
/* msvcrt.dll — C runtime (UBT M4)                                     */
/* ------------------------------------------------------------------ */

/* Format `fmt` into `out`, pulling each variadic argument from the 32-bit word
 * array `a` (a[0], a[1], …). Used by printf (args on the shared stack) and by
 * vfprintf (args at the passed va_list). Each conversion is re-formatted with the
 * native `snprintf` using the *same* spec, so flags/width/precision/%lld/%f match
 * a real libc. Returns the number of bytes written to `out`. */
size_t aret_vformat(char *out, size_t cap, const char *fmt, const uint32_t *a) {
    if (!fmt) return 0;
    int ai = 0;
    size_t o = 0;
    char spec[80];
    for (const char *p = fmt; *p && o < cap - 1;) {
        if (*p != '%') { out[o++] = *p++; continue; }
        size_t si = 0;
        spec[si++] = *p++;
        if (*p == '%') { out[o++] = '%'; p++; continue; }
        while (*p && si < sizeof(spec) - 16 && strchr("-+ #0123456789.*", *p)) {
            if (*p == '*') {
                si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", (int)a[ai++]);
                p++;
            } else {
                spec[si++] = *p++;
            }
        }
        int longlong = 0, seen_l = 0;
        /* MSVC size prefixes glibc does not understand — translate rather than copy
         * into `spec`: `I64` -> 64-bit ("ll"), `I32` -> 32-bit int, bare `I` ->
         * pointer width (32-bit on Win32). Without this, `%I64d` falls through to
         * the literal-spec default and prints "%I64d" (seen in busybox `expr`). */
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            longlong = 1;
            if (si < sizeof(spec) - 3) { spec[si++] = 'l'; spec[si++] = 'l'; }
            p += 3;
        } else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') {
            p += 3; /* 32-bit int — no glibc modifier needed */
        } else if (p[0] == 'I') {
            p += 1; /* pointer width == 32-bit int on Win32 */
        }
        while (*p == 'l' || *p == 'h' || *p == 'L' || *p == 'z' || *p == 'j' || *p == 't') {
            if (*p == 'l') { if (seen_l) longlong = 1; seen_l = 1; }
            if (*p == 'L' || *p == 'j') longlong = 1;
            if (si < sizeof(spec) - 2) spec[si++] = *p;
            p++;
        }
        char conv = *p ? *p++ : 0;
        if (!conv) break;
        if (si < sizeof(spec) - 2) spec[si++] = conv;
        spec[si] = 0;
        char tmp[1024];
        int n = 0;
        uint64_t w64;
        switch (conv) {
            case 'd': case 'i':
                if (longlong) { w64 = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2; n = snprintf(tmp, sizeof tmp, spec, (long long)w64); }
                else { n = snprintf(tmp, sizeof tmp, spec, (int)a[ai++]); }
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (longlong) { w64 = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2; n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)w64); }
                else { n = snprintf(tmp, sizeof tmp, spec, (unsigned)a[ai++]); }
                break;
            case 'C':   /* wide char in a narrow printf (16-bit unit) */
            case 'c': {
                if (conv == 'C' || seen_l) {                 /* %lc / %C : 16-bit char */
                    uint16_t wc = (uint16_t)a[ai++];
                    n = snprintf(tmp, sizeof tmp, "%c", (wc && wc < 0x100) ? (int)wc : '?');
                } else {
                    n = snprintf(tmp, sizeof tmp, spec, (int)a[ai++]);
                }
                break;
            }
            case 'p': n = snprintf(tmp, sizeof tmp, spec, (void *)(uintptr_t)a[ai++]); break;
            case 'S':   /* wide string in a narrow printf (Windows convention) */
            case 's': {
                if (conv == 'S' || seen_l) {                 /* %ls / %S : 16-bit string */
                    const uint16_t *w = (const uint16_t *)(uintptr_t)a[ai++];
                    char nb[2048]; size_t nn = 0;
                    if (w) for (; w[nn] && nn < sizeof nb - 1; nn++) nb[nn] = w[nn] < 0x100 ? (char)w[nn] : '?';
                    else { const char *z = "(null)"; while (*z) nb[nn++] = *z++; }
                    nb[nn] = 0;
                    /* narrow spec: same flags/width/precision, length modifiers dropped, conv 's'. */
                    char sp[80]; size_t j = 0;
                    for (size_t k = 0; k < si && j < sizeof sp - 2; k++) {
                        char ch = spec[k];
                        if (ch == 'l' || ch == 'h' || ch == 'L' || ch == 'j' || ch == 'z' || ch == 't') continue;
                        sp[j++] = (ch == 'S') ? 's' : ch;
                    }
                    sp[j] = 0;
                    n = snprintf(tmp, sizeof tmp, sp, nb);
                } else {
                    const char *v = (const char *)(uintptr_t)a[ai++];
                    n = snprintf(tmp, sizeof tmp, spec, v ? v : "(null)");
                }
                break;
            }
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                union { uint64_t u; double d; } u; u.u = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2;
                n = snprintf(tmp, sizeof tmp, spec, u.d);
                break;
            }
            default: n = snprintf(tmp, sizeof tmp, "%s", spec); break;
        }
        if (n < 0) n = 0;
        for (int k = 0; k < n && o < cap - 1; k++) out[o++] = tmp[k];
    }
    out[o] = 0;
    return o;
}

/* Wide (16-bit) formatter — the analog of aret_vformat that produces a Windows
 * wchar_t (16-bit) string. Numeric/char conversions reuse the exact narrow logic
 * (format into a narrow buffer, then widen), so only the string source differs: in
 * a wide printf `%s`/`%ls` = wide string, `%hs`/`%S` = narrow. `cap` counts 16-bit
 * units (including the terminator). */
size_t aret_wvformat(uint16_t *out, size_t cap, const uint16_t *fmt, const uint32_t *a) {
    if (!fmt || cap == 0) { if (cap) out[0] = 0; return 0; }
    int ai = 0;
    size_t o = 0;
    char spec[80];
    for (const uint16_t *p = fmt; *p && o < cap - 1;) {
        if (*p != '%') { out[o++] = *p++; continue; }
        size_t si = 0;
        spec[si++] = '%'; p++;
        if (*p == '%') { out[o++] = '%'; p++; continue; }
        while (*p && *p < 128 && si < sizeof(spec) - 16 && strchr("-+ #0123456789.*", (char)*p)) {
            if (*p == '*') { si += (size_t)snprintf(spec + si, sizeof(spec) - si, "%d", (int)a[ai++]); p++; }
            else spec[si++] = (char)*p++;
        }
        int longlong = 0, seen_l = 0, seen_h = 0;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { longlong = 1; if (si < sizeof(spec) - 3) { spec[si++] = 'l'; spec[si++] = 'l'; } p += 3; }
        else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
        else if (p[0] == 'I') { p += 1; }
        while (*p == 'l' || *p == 'h' || *p == 'L' || *p == 'z' || *p == 'j' || *p == 't') {
            if (*p == 'l') { if (seen_l) longlong = 1; seen_l = 1; }
            if (*p == 'h') seen_h = 1;
            if (*p == 'L' || *p == 'j') longlong = 1;
            if (si < sizeof(spec) - 2) spec[si++] = (char)*p;
            p++;
        }
        char conv = *p ? (char)*p++ : 0;
        if (!conv) break;
        if (si < sizeof(spec) - 2) spec[si++] = conv;
        spec[si] = 0;
        char tmp[2048];
        int n = 0;
        uint64_t w64;
        switch (conv) {
            case 'd': case 'i':
                if (longlong) { w64 = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2; n = snprintf(tmp, sizeof tmp, spec, (long long)w64); }
                else { n = snprintf(tmp, sizeof tmp, spec, (int)a[ai++]); }
                break;
            case 'u': case 'x': case 'X': case 'o':
                if (longlong) { w64 = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2; n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)w64); }
                else { n = snprintf(tmp, sizeof tmp, spec, (unsigned)a[ai++]); }
                break;
            case 'p': n = snprintf(tmp, sizeof tmp, spec, (void *)(uintptr_t)a[ai++]); break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
                union { uint64_t u; double d; } u; u.u = ((uint64_t)a[ai + 1] << 32) | a[ai]; ai += 2;
                n = snprintf(tmp, sizeof tmp, spec, u.d);
                break;
            }
            case 'C': case 'c': {                  /* wide char default; %hc = narrow */
                uint16_t wc = (uint16_t)a[ai++];
                if (conv != 'C' && seen_h) wc = (uint16_t)(uint8_t)wc;
                if (o < cap - 1) out[o++] = wc;
                continue;
            }
            case 'S': case 's': {
                int narrow = (conv == 'S') || seen_h;  /* %hs/%S narrow, %s/%ls wide */
                /* Widen the source into a narrow byte buffer, then apply flags/width/
                 * precision via a narrow `%s` spec, then widen the formatted result. */
                char nb[2048]; size_t nn = 0;
                if (narrow) {
                    const char *v = (const char *)(uintptr_t)a[ai++];
                    if (!v) v = "(null)";
                    for (; *v && nn < sizeof nb - 1; nn++) nb[nn] = *v++;
                } else {
                    const uint16_t *v = (const uint16_t *)(uintptr_t)a[ai++];
                    if (v) for (; v[nn] && nn < sizeof nb - 1; nn++) nb[nn] = v[nn] < 0x100 ? (char)v[nn] : '?';
                    else { const char *z = "(null)"; while (*z) nb[nn++] = *z++; }
                }
                nb[nn] = 0;
                char sp[80]; size_t j = 0;
                for (size_t k = 0; k < si && j < sizeof sp - 2; k++) {
                    char ch = spec[k];
                    if (ch == 'l' || ch == 'h' || ch == 'L' || ch == 'j' || ch == 'z' || ch == 't') continue;
                    sp[j++] = (ch == 'S') ? 's' : ch;
                }
                sp[j] = 0;
                n = snprintf(tmp, sizeof tmp, sp, nb);
                break;
            }
            default: n = snprintf(tmp, sizeof tmp, "%s", spec); break;
        }
        if (n < 0) n = 0;
        for (int k = 0; k < n && o < cap - 1; k++) out[o++] = (uint16_t)(uint8_t)tmp[k];
    }
    out[o] = 0;
    return o;
}

uint32_t aret_printf(uint32_t esp) {
    const char *fmt = (const char *)(uintptr_t)arg(esp, 0);
    const uint32_t *a = &((const uint32_t *)(uintptr_t)esp)[1]; /* args after fmt */
    char out[8192];
    size_t o = aret_vformat(out, sizeof out, fmt, a);
    ssize_t w = write(1, out, o);
    (void)w;
    return (uint32_t)o;
}

/* Synthetic msvcrt `_iob` (stdin/out/err FILE array) + a pool for fopen'd files.
 * Statically-linked msvcrt CRT code *inlines* getc/putc against the FILE's own
 * fields (`--_cnt >= 0 ? *_ptr++ : _filbuf(f)`), so EVERY FILE the guest sees must
 * have the msvcrt 32-bit layout — not a host glibc FILE (whose offset 0 holds the
 * `_IO_MAGIC` flags word 0xfbad…, which the inlined getc would deref as `_ptr` and
 * crash: BusyBox `wc`/`sort`/`head` on a real file). So fopen hands out one of our
 * own msvcrt-layout structs, fd-backed and unbuffered (we keep `_cnt <= 0` so the
 * inlined macro always defers to `_filbuf`/`_flsbuf`, which do a raw fd read/write).
 * msvcrt FILE: { char* _ptr(0); int _cnt(4); char* _base(8); int _flag(12);
 * int _file(16); int _charbuf(20); … }. We repurpose `_charbuf`(20) as a 1-byte
 * ungetc pushback, stored as (char+1) so a zero-initialised slot means "empty". */
#define ARET_FILE_SIZE 32
#define ARET_F_CNT   4
#define ARET_F_FLAG  12   /* _IOEOF=0x10, _IOERR=0x20 */
#define ARET_F_FILE  16
#define ARET_F_UNGOT 20   /* (pushback char + 1); 0 = none */
static uint8_t aret_iob[3 * ARET_FILE_SIZE];
#define ARET_NDYN 64
static uint8_t aret_dynfile[ARET_NDYN * ARET_FILE_SIZE]; /* fopen/tmpfile/fdopen */
static uint8_t aret_dyn_used[ARET_NDYN];

/* fd for a FILE* that is one of our synthetic streams (std _iob or an fopen'd pool
 * entry), else -1. std entries derive the fd from the array index (valid even
 * before `__p__iob` runs); pool entries carry the real fd at `_file`(offset 16). */
static int file_fd(uint32_t file) {
    uintptr_t f = (uintptr_t)file, a = (uintptr_t)aret_iob, b = (uintptr_t)aret_dynfile;
    if (f >= a && f < a + sizeof(aret_iob)) return (int)((f - a) / ARET_FILE_SIZE); /* 0/1/2 */
    if (f >= b && f < b + sizeof(aret_dynfile)) return *(int32_t *)(uintptr_t)(f + ARET_F_FILE);
    return -1;
}

/* Allocate an msvcrt-layout FILE bound to `fd` (unbuffered). 0 if the pool is full. */
static uint32_t alloc_dynfile(int fd) {
    for (int i = 0; i < ARET_NDYN; i++) {
        if (!aret_dyn_used[i]) {
            aret_dyn_used[i] = 1;
            uint8_t *f = aret_dynfile + (size_t)i * ARET_FILE_SIZE;
            memset(f, 0, ARET_FILE_SIZE);
            *(int32_t *)(f + ARET_F_FILE) = fd;
            return (uint32_t)(uintptr_t)f;
        }
    }
    return 0;
}
static void free_dynfile(uint32_t file) {
    uintptr_t f = (uintptr_t)file, b = (uintptr_t)aret_dynfile;
    if (f >= b && f < b + sizeof(aret_dynfile))
        aret_dyn_used[(f - b) / ARET_FILE_SIZE] = 0;
}

/* One byte from a synthetic stream, honouring a pending ungetc pushback; -1 at
 * EOF/error (sets _IOEOF so feof() stays truthful). The single primitive behind
 * getc/fgetc/fgets/_filbuf, so ungetc composes with all of them. */
static int pull_byte(uint32_t file) {
    int fd = file_fd(file);
    if (fd < 0) return -1;
    uint8_t *f = (uint8_t *)(uintptr_t)file;
    int32_t *ung = (int32_t *)(f + ARET_F_UNGOT);
    if (*ung) { int c = *ung - 1; *ung = 0; return c; }
    unsigned char b;
    ssize_t n = read(fd, &b, 1);
    if (n <= 0) { *(int32_t *)(f + ARET_F_FLAG) |= 0x10; return -1; }
    return b;
}

static void stdio_write(uint32_t file, const char *buf, size_t len) {
    int fd = file_fd(file);
    if (fd >= 0) { ssize_t w = write(fd, buf, len); (void)w; }
}

uint32_t aret_vfprintf(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    const char *fmt = (const char *)(uintptr_t)arg(esp, 1);
    const uint32_t *a = (const uint32_t *)(uintptr_t)arg(esp, 2); /* va_list */
    char out[8192];
    size_t o = aret_vformat(out, sizeof out, fmt, a);
    stdio_write(file, out, o);
    return (uint32_t)o;
}

/* msvcrt low-level (POSIX-style) I/O on integer fds — forwarded to the host. The
 * guest's fds are plain Unix fds (0/1/2 for the std streams, others from open).
 * `__p__iob` exposes the synthetic _iob array so `_fileno(stdout)` resolves to 1.
 * Returning 0 from `_write` (the weak stub) made BusyBox spin forever retrying a
 * "short" write — so these must be real. */
/* msvcrt FILE (32-bit): { char* _ptr; int _cnt; char* _base; int _flag; int _file;
 * ... }. Statically-linked CRT stdio reads `_file` (the fd, at offset 16) and the
 * `_flag` (offset 12); an all-zero _iob makes it treat the stream as buffered with
 * a null _base and crash on flush. Initialise each entry: fd in _file, _IONBF so
 * the CRT writes straight through `_write` (we never allocate a buffer). */
uint32_t aret_p__iob(uint32_t esp) {
    (void)esp;
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < 3; i++) {
            uint8_t *f = aret_iob + (size_t)i * ARET_FILE_SIZE;
            *(int32_t *)(f + 12) = 0x40 | 0x02; /* _flag: _IONBF | _IOWRT */
            *(int32_t *)(f + 16) = i;           /* _file = fd            */
        }
        inited = 1;
    }
    return (uint32_t)(uintptr_t)aret_iob;
}
uint32_t aret_fileno(uint32_t esp) {
    int fd = file_fd(arg(esp, 0));
    return (uint32_t)fd;
}
uint32_t aret_write(uint32_t esp) {
    ssize_t r = write((int)arg(esp, 0), (const void *)(uintptr_t)arg(esp, 1), (size_t)arg(esp, 2));
    return (uint32_t)(r < 0 ? -1 : r);
}
uint32_t aret_read(uint32_t esp) {
    ssize_t r = read((int)arg(esp, 0), (void *)(uintptr_t)arg(esp, 1), (size_t)arg(esp, 2));
    return (uint32_t)(r < 0 ? -1 : r);
}
uint32_t aret_close(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    /* Honour close() faithfully, std fds included: the classic Unix idiom
     * `close(0); open(file)` reopens the file *as* fd 0 (lowest free) so a tool
     * can then read it through stdin (BusyBox `uniq FILE`, `tac`, …). Guarding
     * fd<=2 made close(0) a no-op, so the reopen got fd 3 and the tool kept
     * reading the real (empty) stdin. fclose() still protects the _iob structs
     * (aret_fclose only closes fd>2); this is the raw descriptor call. */
    if (fd < 0) return (uint32_t)-1;
    return (uint32_t)close(fd);
}
/* _lseek(fd, offset, origin) -> new offset. origin 0/1/2 == SEEK_SET/CUR/END. */
uint32_t aret_lseek(uint32_t esp) {
    off_t r = lseek((int)arg(esp, 0), (int32_t)arg(esp, 1), (int)arg(esp, 2));
    return (uint32_t)r;
}
/* _lseeki64(fd, __int64 offset, int origin) -> __int64. The 64-bit offset is two
 * stack slots (lo@1, hi@2); origin@3. Returns the new position in edx:eax (the
 * builder declares this shim uint64_t). Used by tail/od/split on large files. */
uint64_t aret_lseeki64(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    int64_t off = (int64_t)(((uint64_t)arg(esp, 2) << 32) | arg(esp, 1));
    int origin = (int)arg(esp, 3);
    off_t r = lseek(fd, (off_t)off, origin);
    return (uint64_t)r;
}
/* _telli64(fd) -> __int64 current position. */
uint64_t aret_telli64(uint32_t esp) {
    return (uint64_t)lseek((int)arg(esp, 0), 0, SEEK_CUR);
}
/* _ftelli64(FILE*) -> __int64 current position of one of our streams. */
uint64_t aret_ftelli64(uint32_t esp) {
    int fd = file_fd(arg(esp, 0));
    return fd < 0 ? (uint64_t)-1 : (uint64_t)lseek(fd, 0, SEEK_CUR);
}
/* _filbuf(FILE*) — the SVID buffer-refill primitive the getc/getchar macro falls
 * back to (mingw inlines `--_cnt >= 0 ? *_ptr++ : _filbuf(f)`). Our synthetic
 * _iob streams carry no buffer, so serve exactly one byte per call straight from
 * the fd and leave _cnt at 0, so the next getc re-enters here (unbuffered but
 * correct). Returns the byte (0..255), or EOF (-1) at end/error with the _IOEOF
 * flag set so feof() stays truthful. Without this the weak stub returned 0
 * forever and every stdin read loop (wc, sort, head, …) spun endlessly instead
 * of stopping at EOF. A real host FILE* (fopen) defers to the host CRT. */
uint32_t aret_filbuf(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    int c = pull_byte(file);             /* honours ungetc, sets _IOEOF at end   */
    uint8_t *f = (uint8_t *)(uintptr_t)file;
    if (file_fd(file) >= 0) *(int32_t *)(f + ARET_F_CNT) = 0; /* _cnt=0 -> re-enter */
    return c < 0 ? 0xFFFFFFFFu : (uint32_t)c;
}
/* _flsbuf(c, FILE*) — the putc/putchar macro's write-side fallback (mingw inlines
 * `--_cnt >= 0 ? *_ptr++ = c : _flsbuf(c, f)`). Unbuffered: write the byte straight
 * to the fd and keep _cnt at 0 so the next putc re-enters here. Returns c, or EOF. */
uint32_t aret_flsbuf(uint32_t esp) {
    int c = (int)arg(esp, 0);
    uint32_t file = arg(esp, 1);
    int fd = file_fd(file);
    if (fd < 0) return 0xFFFFFFFFu;
    unsigned char b = (unsigned char)c;
    ssize_t w = write(fd, &b, 1);
    uint8_t *f = (uint8_t *)(uintptr_t)file;
    *(int32_t *)(f + ARET_F_CNT) = 0;
    if (w != 1) { *(int32_t *)(f + ARET_F_FLAG) |= 0x20; return 0xFFFFFFFFu; }
    return (uint32_t)b;
}
/* getchar() — one byte from stdin. Some CRT builds inline it to the _filbuf
 * macro (handled above); others import the library entry point directly, so
 * shim it (else the weak stub returns 0 forever and the read loop hangs, like
 * the _filbuf gap). getc/fgetc/ungetc (taking a FILE*) are defined below. */
uint32_t aret_getchar(uint32_t esp) {
    (void)esp; unsigned char b;
    return read(0, &b, 1) == 1 ? (uint32_t)b : 0xFFFFFFFFu;
}
/* _isatty(fd): nonzero for a character device (console), 0 otherwise. Windows'
 * _isatty does NOT report ENOTTY through errno for a regular file, but Linux
 * isatty() does (via its TCGETS ioctl) — and BusyBox-w32 probes isatty() on every
 * read, so a leaked ENOTTY makes the *next* errno check misfire (tac reported
 * "Inappropriate ioctl for device" at EOF). Preserve errno to match Windows. */
uint32_t aret_isatty(uint32_t esp) {
    int saved = errno;
    int r = isatty((int)arg(esp, 0));
    errno = saved;
    return (uint32_t)r;
}
/* _setmode(fd, mode): text/binary distinction is meaningless on Linux. Report the
 * previous mode as binary (O_BINARY = 0x8000 in msvcrt) so callers see success. */
uint32_t aret_setmode(uint32_t esp) { (void)esp; return 0x8000; }

uint32_t aret_fprintf(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    const char *fmt = (const char *)(uintptr_t)arg(esp, 1);
    const uint32_t *a = &((const uint32_t *)(uintptr_t)esp)[2]; /* args after fmt */
    char out[8192];
    size_t o = aret_vformat(out, sizeof out, fmt, a);
    stdio_write(file, out, o);
    return (uint32_t)o;
}

uint32_t aret_fputc(uint32_t esp) {
    char c = (char)arg(esp, 0);
    stdio_write(arg(esp, 1), &c, 1);
    return (uint8_t)c;
}
/* putc(c, stream) is fputc; _putc/_fputc are the same. When not inlined to the
 * _flsbuf macro (compiler/opt-level dependent) the CRT imports it by name. */
uint32_t aret_putc(uint32_t esp) { return aret_fputc(esp); }

uint32_t aret_vprintf(uint32_t esp) {
    const char *fmt = (const char *)(uintptr_t)arg(esp, 0);
    const uint32_t *a = (const uint32_t *)(uintptr_t)arg(esp, 1);
    char out[8192];
    size_t o = aret_vformat(out, sizeof out, fmt, a);
    ssize_t w = write(1, out, o);
    (void)w;
    return (uint32_t)o;
}

/* Data imports (variables, not functions): the builder patches their IAT slot to
 * point at these synthetic objects. Returns 0 for names we do not model (the slot
 * is left as-is — fine for function imports, which are call-redirected). */
static void *aret_initenv_var = 0;       /* char**: saved environment           */
static char *aret_environ_arr[1] = { 0 };/* empty environment                   */
static void *aret_environ_var = aret_environ_arr;
static int aret_mb_cur_max_var = 1;

/* `_iob` / `__acrt_iob_func` / `__iob_func` called as a function: returns
 * &_iob[index] (the stdio stream for that index). */
uint32_t aret_acrt_iob_func(uint32_t esp) {
    uint32_t i = arg(esp, 0); if (i > 2) i = 1;
    return (uint32_t)(uintptr_t)(aret_iob + i * ARET_FILE_SIZE);
}
uint32_t aret_iob_func(uint32_t esp) { return aret_acrt_iob_func(esp); }

uint32_t aret_data_import(const char *name) {
    if (!strcmp(name, "_iob") || !strcmp(name, "__iob_func")) return (uint32_t)(uintptr_t)aret_iob;
    if (!strcmp(name, "__initenv")) return (uint32_t)(uintptr_t)&aret_initenv_var;
    if (!strcmp(name, "_environ")) return (uint32_t)(uintptr_t)&aret_environ_var;
    if (!strcmp(name, "__mb_cur_max")) return (uint32_t)(uintptr_t)&aret_mb_cur_max_var;
    return 0;
}

uint32_t aret_puts(uint32_t esp) {
    const char *s = (const char *)(uintptr_t)arg(esp, 0);
    if (!s) s = "(null)";
    size_t len = strlen(s);
    ssize_t a = write(1, s, len);
    ssize_t b = write(1, "\n", 1);
    (void)a; (void)b;
    return (uint32_t)(len + 1);
}

uint32_t aret_putchar(uint32_t esp) {
    unsigned char c = (unsigned char)arg(esp, 0);
    ssize_t w = write(1, &c, 1);
    (void)w;
    return c;
}

uint32_t aret_malloc(uint32_t esp) {
    return (uint32_t)(uintptr_t)malloc(arg(esp, 0));
}

uint32_t aret_calloc(uint32_t esp) {
    return (uint32_t)(uintptr_t)calloc(arg(esp, 0), arg(esp, 1));
}

uint32_t aret_realloc(uint32_t esp) {
    return (uint32_t)(uintptr_t)realloc((void *)(uintptr_t)arg(esp, 0), arg(esp, 1));
}

uint32_t aret_free(uint32_t esp) {
    free((void *)(uintptr_t)arg(esp, 0));
    return 0;
}

/* MSVC `_msize(p)` — usable size of a heap block. sqlite's default allocator
 * (`SQLITE_MALLOCSIZE`) calls it to track memory; without it every block reads
 * as 0 bytes and sqlite aborts with "out of memory". */
uint32_t aret_msize(uint32_t esp) {
    void *p = (void *)(uintptr_t)arg(esp, 0);
    return p ? (uint32_t)malloc_usable_size(p) : 0;
}

uint32_t aret_memcpy(uint32_t esp) {
    void *d = (void *)(uintptr_t)arg(esp, 0);
    memcpy(d, (const void *)(uintptr_t)arg(esp, 1), arg(esp, 2));
    return (uint32_t)(uintptr_t)d;
}

uint32_t aret_memset(uint32_t esp) {
    void *d = (void *)(uintptr_t)arg(esp, 0);
    memset(d, (int)arg(esp, 1), arg(esp, 2));
    return (uint32_t)(uintptr_t)d;
}

uint32_t aret_memmove(uint32_t esp) {
    void *d = (void *)(uintptr_t)arg(esp, 0);
    memmove(d, (const void *)(uintptr_t)arg(esp, 1), arg(esp, 2));
    return (uint32_t)(uintptr_t)d;
}

/* MSVC CRT internal allocator family (`_malloc_crt`/`_calloc_crt`/`_realloc_crt`/
 * `_free_crt`, msvcrNN): identical to the public malloc family but routed through
 * the CRT's private heap during startup. Host malloc — the weak stub returned NULL,
 * which makes the CRT/MFC startup fail its first allocation and abort. */
uint32_t aret_malloc_crt(uint32_t esp)  { return (uint32_t)(uintptr_t)malloc(arg(esp, 0)); }
uint32_t aret_calloc_crt(uint32_t esp)  { return (uint32_t)(uintptr_t)calloc(arg(esp, 0), arg(esp, 1)); }
uint32_t aret_realloc_crt(uint32_t esp) { return (uint32_t)(uintptr_t)realloc((void *)(uintptr_t)arg(esp, 0), arg(esp, 1)); }
uint32_t aret_free_crt(uint32_t esp)    { free((void *)(uintptr_t)arg(esp, 0)); return 0; }

/* memcpy_s(dst, destsz, src, count) -> errno_t (C11 Annex K / MSVC). Bounded copy:
 * validates the destination is large enough before copying; on a violation it zeroes
 * the whole destination (when it can) and returns an error code. EINVAL=22, ERANGE=34
 * (MSVC values). A no-op weak stub (returning 0 without copying) left MFC reading
 * uninitialised data — a silent wrong; this reproduces the real contract. */
uint32_t aret_memcpy_s(uint32_t esp) {
    void *dst = (void *)(uintptr_t)arg(esp, 0);
    uint32_t destsz = arg(esp, 1);
    const void *src = (const void *)(uintptr_t)arg(esp, 2);
    uint32_t count = arg(esp, 3);
    if (count == 0) return 0;
    if (!dst) return 22;                       /* EINVAL: no destination */
    if (!src || destsz < count) {              /* invalid source, or would overflow dst */
        memset(dst, 0, destsz);
        return src ? 34u : 22u;                /* ERANGE (too small) : EINVAL (null src) */
    }
    memcpy(dst, src, count);
    return 0;
}

uint32_t aret_strlen(uint32_t esp) {
    return (uint32_t)strlen((const char *)(uintptr_t)arg(esp, 0));
}

uint32_t aret_strcmp(uint32_t esp) {
    return (uint32_t)(int32_t)strcmp(
        (const char *)(uintptr_t)arg(esp, 0), (const char *)(uintptr_t)arg(esp, 1));
}

uint32_t aret_strcpy(uint32_t esp) {
    char *d = (char *)(uintptr_t)arg(esp, 0);
    strcpy(d, (const char *)(uintptr_t)arg(esp, 1));
    return (uint32_t)(uintptr_t)d;
}

/* ------------------------------------------------------------------ */
/* Filesystem subsystem (UBT Phase 3)                                 */
/* ------------------------------------------------------------------ */

static const char *aret_prefix(void) {
    const char *p = getenv("ARET_PREFIX");
    return (p && *p) ? p : "aret_prefix";
}

/* Translate a path to a native one. The recompiled program is a *native Linux
 * tool*, so genuine Windows paths map under the ARET prefix while real Unix
 * paths reach the real filesystem:
 *   "C:\dir\file" -> "<prefix>/drive_c/dir/file"   (DOS drive)
 *   "\dir\file"   -> "<prefix>/dir/file"            (Windows rooted, drive-less)
 *   "/dir/file"   -> "/dir/file"                    (Unix absolute: real FS)
 *   "dir\file"    -> "dir/file"                     (relative: passes through)
 * The `/`-absolute passthrough is what lets `cat /tmp/x` read the real file
 * (only `\`-rooted backslash paths, which a Unix tool never produces, are
 * sandboxed). Relative paths still pass through, so write-then-read round-trips
 * stay consistent. */
static void translate_path(const char *win, char *out, size_t cap) {
    if (!win) { if (cap) out[0] = 0; return; }
    size_t o = 0;
    const char *s = win;
    if (win[0] && win[1] == ':' && (win[2] == '\\' || win[2] == '/')) {
        o = (size_t)snprintf(out, cap, "%s/drive_%c", aret_prefix(),
                             (char)tolower((unsigned char)win[0]));
        s = win + 2; /* keep the leading separator */
    } else if (win[0] == '\\') {
        o = (size_t)snprintf(out, cap, "%s", aret_prefix());
        s = win;
    }
    for (; *s && o < cap - 1; s++) out[o++] = (*s == '\\') ? '/' : *s;
    out[o] = 0;
}

/* mkdir -p for the directory components of `path`. */
static void make_parents(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0777);
            *p = '/';
        }
    }
}

static int path_is_write_mode(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

/* Translate a POSIX-ish fopen mode string to open(2) flags. */
static int fopen_mode_flags(const char *mode) {
    if (!mode) return O_RDONLY;
    int rw = 0, has_plus = strchr(mode, '+') != 0;
    switch (mode[0]) {
        case 'r': rw = has_plus ? O_RDWR : O_RDONLY; break;
        case 'w': rw = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
        case 'a': rw = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
        default:  rw = O_RDONLY; break;
    }
    return rw;
}

uint32_t aret_fopen(uint32_t esp) {
    const char *name = (const char *)(uintptr_t)arg(esp, 0);
    const char *mode = (const char *)(uintptr_t)arg(esp, 1);
    char path[1024];
    translate_path(name, path, sizeof path);
    if (path_is_write_mode(mode)) make_parents(path);
    int fd = open(path, fopen_mode_flags(mode), 0666);
    if (fd < 0) return 0;                       /* NULL: fopen failed */
    uint32_t f = alloc_dynfile(fd);
    if (!f) { close(fd); return 0; }
    return f;
}

uint32_t aret_fclose(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    int fd = file_fd(file);
    if (fd > 2) close(fd);                       /* never close the std fds */
    free_dynfile(file);                          /* no-op for the _iob entries */
    return 0;
}

uint32_t aret_fread(uint32_t esp) {
    char *ptr = (char *)(uintptr_t)arg(esp, 0);
    uint32_t size = arg(esp, 1), nmemb = arg(esp, 2), file = arg(esp, 3);
    int fd = file_fd(file);
    if (fd < 0 || size == 0) return 0;
    size_t want = (size_t)size * nmemb, got = 0;
    /* honour a pending ungetc pushback before the bulk read */
    if (got < want) { int c = pull_byte(file); if (c < 0) return 0; ptr[got++] = (char)c; }
    while (got < want) {
        ssize_t n = read(fd, ptr + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return (uint32_t)(got / size);
}

uint32_t aret_fwrite(uint32_t esp) {
    const char *ptr = (const char *)(uintptr_t)arg(esp, 0);
    uint32_t size = arg(esp, 1), nmemb = arg(esp, 2), file = arg(esp, 3);
    if (file_fd(file) < 0 || size == 0) return 0;
    stdio_write(file, ptr, (size_t)size * nmemb);
    return nmemb;
}

uint32_t aret_fputs(uint32_t esp) {
    const char *s = (const char *)(uintptr_t)arg(esp, 0);
    uint32_t file = arg(esp, 1);
    if (file_fd(file) < 0) return 0xFFFFFFFFu;
    stdio_write(file, s, strlen(s));
    return 0;
}

uint32_t aret_fgets(uint32_t esp) {
    char *buf = (char *)(uintptr_t)arg(esp, 0);
    int n = (int)arg(esp, 1);
    uint32_t file = arg(esp, 2);
    /* fgets over a synthetic stream: up to n-1 chars, stop after '\n', NUL-end. */
    if (n <= 0 || file_fd(file) < 0) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = pull_byte(file);
        if (c < 0) break;                        /* EOF or error */
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;                         /* nothing read → NULL (EOF) */
    buf[i] = '\0';
    return (uint32_t)(uintptr_t)buf;
}

/* Character input + stream state. All synthetic streams read their fd (honouring
 * a pending ungetc); EOF is -1. */
uint32_t aret_getc(uint32_t esp) {
    int c = pull_byte(arg(esp, 0));
    return c < 0 ? 0xFFFFFFFFu : (uint32_t)c;
}
uint32_t aret_fgetc(uint32_t esp) { return aret_getc(esp); }
uint32_t aret_ungetc(uint32_t esp) {
    int c = (int)arg(esp, 0);
    uint32_t file = arg(esp, 1);
    if (file_fd(file) < 0 || c < 0) return 0xFFFFFFFFu;
    uint8_t *f = (uint8_t *)(uintptr_t)file;
    *(int32_t *)(f + ARET_F_UNGOT) = (c & 0xff) + 1;   /* one byte of pushback */
    *(int32_t *)(f + ARET_F_FLAG) &= ~0x10;            /* clear _IOEOF          */
    return (uint32_t)(c & 0xff);
}
uint32_t aret_feof(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (file_fd(file) < 0) return 1;
    return (*(int32_t *)(uintptr_t)((uint8_t *)(uintptr_t)file + ARET_F_FLAG) & 0x10) ? 1 : 0;
}
uint32_t aret_ferror(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (file_fd(file) < 0) return 0;
    return (*(int32_t *)(uintptr_t)((uint8_t *)(uintptr_t)file + ARET_F_FLAG) & 0x20) ? 1 : 0;
}
uint32_t aret_clearerr(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (file_fd(file) >= 0)
        *(int32_t *)(uintptr_t)((uint8_t *)(uintptr_t)file + ARET_F_FLAG) &= ~0x30;
    return 0;
}

uint32_t aret_fflush(uint32_t esp) {
    (void)esp;                                    /* unbuffered write()s: nothing to flush */
    return 0;
}

uint32_t aret_fseek(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    int fd = file_fd(file);
    if (fd < 0) return 0xFFFFFFFFu;
    uint8_t *f = (uint8_t *)(uintptr_t)file;
    *(int32_t *)(f + ARET_F_UNGOT) = 0;           /* drop pushback  */
    *(int32_t *)(f + ARET_F_FLAG) &= ~0x10;       /* clear _IOEOF   */
    off_t r = lseek(fd, (int32_t)arg(esp, 1), (int)arg(esp, 2));
    return r < 0 ? 0xFFFFFFFFu : 0;
}
uint32_t aret_rewind(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    int fd = file_fd(file);
    if (fd >= 0) {
        uint8_t *f = (uint8_t *)(uintptr_t)file;
        *(int32_t *)(f + ARET_F_UNGOT) = 0;
        *(int32_t *)(f + ARET_F_FLAG) &= ~0x30;
        lseek(fd, 0, SEEK_SET);
    }
    return 0;
}

uint32_t aret_ftell(uint32_t esp) {
    int fd = file_fd(arg(esp, 0));
    if (fd < 0) return 0xFFFFFFFFu;
    return (uint32_t)lseek(fd, 0, SEEK_CUR);
}

uint32_t aret_remove(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)remove(path);
}

uint32_t aret_rename(uint32_t esp) {
    char from[1024], to[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), from, sizeof from);
    translate_path((const char *)(uintptr_t)arg(esp, 1), to, sizeof to);
    make_parents(to);
    return (uint32_t)rename(from, to);
}

/* POSIX-style CRT file checks the MSVC runtime exposes (`_access`/`_chmod`). The
 * MSVC access mode bits (0=exist, 2=write, 4=read, 6=rw) coincide with POSIX
 * F_OK/W_OK/R_OK, so pass them through against the translated host path. */
uint32_t aret_access(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)access(path, (int)arg(esp, 1));
}
uint32_t aret_chmod(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)chmod(path, (int)arg(esp, 1));
}

/* freopen(path, mode, stream): reopen `stream` on `path`. A null/synthetic _iob
 * stream cannot be handed to host freopen, so close it (if a real FILE*) and open
 * the path fresh — the returned FILE* is what the program keeps using. */
uint32_t aret_freopen(uint32_t esp) {
    const char *name = (const char *)(uintptr_t)arg(esp, 0);
    const char *mode = (const char *)(uintptr_t)arg(esp, 1);
    uint32_t stream = arg(esp, 2);
    char path[1024];
    translate_path(name, path, sizeof path);
    if (path_is_write_mode(mode)) make_parents(path);
    int fd = open(path, fopen_mode_flags(mode), 0666);
    if (fd < 0) return 0;
    int old = file_fd(stream);
    if (old > 2) {                                /* rebind a std/pool stream's fd */
        dup2(fd, old);
        close(fd);
        return stream;
    }
    /* std stream (0/1/2) or none: dup onto it if a std slot, else a fresh pool FILE */
    if (old >= 0) { dup2(fd, old); close(fd); return stream; }
    uint32_t f = alloc_dynfile(fd);
    if (!f) { close(fd); return 0; }
    return f;
}

uint32_t aret_tmpfile(uint32_t esp) {
    (void)esp;
    char tmpl[] = "/tmp/aretXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return 0;
    unlink(tmpl);                                 /* anonymous: removed on close/exit */
    uint32_t f = alloc_dynfile(fd);
    if (!f) { close(fd); return 0; }
    return f;
}
/* _fdopen(fd, mode): wrap an existing fd in an msvcrt-layout FILE. */
uint32_t aret_fdopen(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    if (fd < 0) return 0;
    return alloc_dynfile(fd);
}

/* setvbuf(stream, buf, mode, size): buffering is an optimization with no
 * observable effect on correct output; accept and report success. */
uint32_t aret_setvbuf(uint32_t esp) { (void)esp; return 0; }

/* Win32 file API — handles are POSIX file descriptors in this model, so the
 * existing aret_ReadFile/aret_WriteFile (which treat the handle as an fd) work
 * unchanged with handles returned here. */
#define ARET_INVALID_HANDLE 0xFFFFFFFFu
#define GENERIC_READ_FLAG  0x80000000u
#define GENERIC_WRITE_FLAG 0x40000000u

/* Narrow a Windows wide string (UTF-16LE) to a byte string (ASCII/Latin-1 — the
 * common case; covers the paths a CLI tool sees). */
static void aret_w2n(const uint16_t *w, char *out, size_t cap) {
    size_t i = 0;
    if (w) for (; w[i] && i + 1 < cap; i++) out[i] = (char)(w[i] & 0xff);
    out[i] = 0;
}

static uint32_t aret_open_named(const char *name, uint32_t access, uint32_t disposition) {
    char path[1024];
    translate_path(name, path, sizeof path);
    int rd = (access & GENERIC_READ_FLAG) != 0;
    int wr = (access & GENERIC_WRITE_FLAG) != 0;
    int flags = (rd && wr) ? O_RDWR : (wr ? O_WRONLY : O_RDONLY);
    switch (disposition) {
        case 1: flags |= O_CREAT | O_EXCL; break;  /* CREATE_NEW */
        case 2: flags |= O_CREAT | O_TRUNC; break; /* CREATE_ALWAYS */
        case 3: break;                             /* OPEN_EXISTING */
        case 4: flags |= O_CREAT; break;           /* OPEN_ALWAYS */
        case 5: flags |= O_TRUNC; break;           /* TRUNCATE_EXISTING */
        default: break;
    }
    if (wr || disposition == 1 || disposition == 2 || disposition == 4) make_parents(path);
    int fd = open(path, flags, 0666);
    return fd < 0 ? ARET_INVALID_HANDLE : (uint32_t)fd;
}
uint32_t aret_CreateFileA(uint32_t esp) {
    return aret_open_named((const char *)(uintptr_t)arg(esp, 0), arg(esp, 1), arg(esp, 4));
}
uint32_t aret_CreateFileW(uint32_t esp) {
    char name[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    return aret_open_named(name, arg(esp, 1), arg(esp, 4));
}

/* Win16-era file API (_lopen/_lcreat/_lclose/_lread/_lwrite/_llseek + _hread/
 * _hwrite): an HFILE is a POSIX fd in this model, so these map straight onto
 * open/read/write/lseek/close, sharing translate_path. HFILE_ERROR = -1. */
#define ARET_HFILE_ERROR 0xFFFFFFFFu
uint32_t aret_lopen(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    uint32_t rw = arg(esp, 1) & 3;      /* OF_READ=0, OF_WRITE=1, OF_READWRITE=2 */
    int flags = rw == 0 ? O_RDONLY : (rw == 1 ? O_WRONLY : O_RDWR);
    int fd = open(path, flags);
    return fd < 0 ? ARET_HFILE_ERROR : (uint32_t)fd;
}
uint32_t aret_lcreat(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    make_parents(path);
    int ro = arg(esp, 1) & 1;           /* iAttribute bit0 = FILE_ATTRIBUTE_READONLY */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, ro ? 0444 : 0666);
    return fd < 0 ? ARET_HFILE_ERROR : (uint32_t)fd;
}
uint32_t aret_lclose(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    return close(fd) == 0 ? (uint32_t)fd : ARET_HFILE_ERROR;
}
uint32_t aret_lread(uint32_t esp) {
    ssize_t r = read((int)arg(esp, 0), (void *)(uintptr_t)arg(esp, 1), arg(esp, 2));
    return r < 0 ? ARET_HFILE_ERROR : (uint32_t)r;
}
uint32_t aret_lwrite(uint32_t esp) {
    ssize_t r = write((int)arg(esp, 0), (const void *)(uintptr_t)arg(esp, 1), arg(esp, 2));
    return r < 0 ? ARET_HFILE_ERROR : (uint32_t)r;
}
uint32_t aret_hread(uint32_t esp)  { return aret_lread(esp); }   /* _hread: same, LONG count */
uint32_t aret_hwrite(uint32_t esp) { return aret_lwrite(esp); }
uint32_t aret_llseek(uint32_t esp) {
    int origin = (int)arg(esp, 2);      /* 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END */
    off_t r = lseek((int)arg(esp, 0), (int32_t)arg(esp, 1),
                    origin == 1 ? SEEK_CUR : (origin == 2 ? SEEK_END : SEEK_SET));
    return r < 0 ? ARET_HFILE_ERROR : (uint32_t)r;
}
/* OpenFile(lpFileName, lpReOpenBuff, uStyle) — the Win16-legacy file API (still
 * imported by old apps). HFILE = fd (same model as _lopen). uStyle low bits pick
 * OF_READ/WRITE/READWRITE; OF_CREATE(0x1000) creates+truncates; OF_DELETE(0x200)
 * unlinks; OF_EXIST(0x4000) is a read-open used as an existence test. On failure ->
 * HFILE_ERROR with OFSTRUCT.nErrCode set. OFSTRUCT: cBytes@0, fFixedDisk@1,
 * nErrCode@2 (WORD), szPathName@8[128]. The canonical path Wine writes to szPathName
 * is environment-dependent (drive mapping) and not oracle-compared; we fill the input
 * name and report ret/data honestly. */
uint32_t aret_OpenFile(uint32_t esp) {
    const char *name = (const char *)(uintptr_t)arg(esp, 0);
    uint8_t *ofs = (uint8_t *)(uintptr_t)arg(esp, 1);
    uint32_t style = arg(esp, 2);
    char path[1024];
    translate_path(name ? name : "", path, sizeof path);
    if (ofs) { int i = 0; if (name) for (; name[i] && i < 127; i++) ofs[8 + i] = (uint8_t)name[i]; ofs[8 + i] = 0; }
    uint32_t ret, err = 0;
    if (style & 0x200u) {                        /* OF_DELETE */
        ret = (unlink(path) == 0) ? 0u : ARET_HFILE_ERROR;
        if (ret == ARET_HFILE_ERROR) err = 2;
    } else if (style & 0x1000u) {                /* OF_CREATE */
        make_parents(path);
        int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) { ret = ARET_HFILE_ERROR; err = 2; } else ret = (uint32_t)fd;
    } else {                                     /* OF_READ / WRITE / READWRITE / EXIST */
        uint32_t rw = style & 3u;
        int flags = rw == 1 ? O_WRONLY : (rw == 2 ? O_RDWR : O_RDONLY);
        int fd = open(path, flags);
        if (fd < 0) { ret = ARET_HFILE_ERROR; err = 2; } else ret = (uint32_t)fd;
    }
    if (ofs) { uint32_t e = (ret == ARET_HFILE_ERROR) ? err : 0; ofs[2] = (uint8_t)(e & 0xFF); ofs[3] = (uint8_t)(e >> 8); }
    return ret;
}
/* GetLogicalDrives: bitmask of present drives. The model exposes one drive, C: (bit 2)
 * — the conventional Windows system drive apps run from. (Wine also exposes Z: for the
 * Unix root, but that is a Wine artifact and environment-dependent, so we model the
 * portable minimum: C: present.) */
uint32_t aret_GetLogicalDrives(uint32_t esp) { (void)esp; return 1u << 2; }
/* ---- LZ decompression (lzexpand.dll: SZDD / LZSS) --------------------------
 * Microsoft COMPRESS.EXE format: an 8-byte magic, mode + missing-char, a 4-byte
 * uncompressed size, then LZSS data over a 4 KB ring buffer (init 0x20, position
 * 4078). A genuine deterministic decompressor (verified-algorithm reuse) — the LZ
 * APIs decompress install/data files. An LZ handle owns the whole decompressed
 * content; a non-SZDD file is carried verbatim. HFILE = fd; LZ handle tag base
 * 0x4C5A0000. */
#define ARET_LZ_BASE 0x4C5A0000u
#define ARET_LZ_MAX 32
static struct { int used; uint8_t *buf; size_t len, pos; } g_lz[ARET_LZ_MAX];

static int lz_szdd_decompress(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen) {
    static const uint8_t magic[8] = { 0x53,0x5A,0x44,0x44,0x88,0xF0,0x27,0x33 };
    if (inlen < 14 || memcmp(in, magic, 8) != 0) return 0;
    uint32_t osize = (uint32_t)in[10] | ((uint32_t)in[11] << 8) | ((uint32_t)in[12] << 16) | ((uint32_t)in[13] << 24);
    uint8_t *o = (uint8_t *)malloc(osize ? osize : 1);
    if (!o) return 0;
    uint8_t ring[4096]; memset(ring, 0x20, sizeof ring);
    int rp = 4096 - 16;
    size_t ip = 14, op = 0;
    while (ip < inlen && op < osize) {
        uint8_t control = in[ip++];
        for (int bit = 0; bit < 8 && op < osize; bit++) {
            if (control & (1u << bit)) {                    /* literal */
                if (ip >= inlen) break;
                uint8_t b = in[ip++];
                o[op++] = b; ring[rp] = b; rp = (rp + 1) & 0xFFF;
            } else {                                        /* back-reference */
                if (ip + 1 >= inlen) break;
                uint8_t lo = in[ip++], hi = in[ip++];
                int mpos = lo | ((hi & 0xF0) << 4), mlen = (hi & 0x0F) + 3;
                for (int k = 0; k < mlen && op < osize; k++) {
                    uint8_t b = ring[(mpos + k) & 0xFFF];
                    o[op++] = b; ring[rp] = b; rp = (rp + 1) & 0xFFF;
                }
            }
        }
    }
    *out = o; *outlen = op; return 1;
}
/* Read a whole file; decompress if SZDD, else keep it verbatim. */
static int lz_slurp(int fd, uint8_t **out, size_t *outlen) {
    if (fd < 0) return 0;
    off_t sz = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
    if (sz < 0) return 0;
    uint8_t *raw = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!raw) return 0;
    ssize_t rd = read(fd, raw, (size_t)sz);
    if (rd < 0) { free(raw); return 0; }
    if (lz_szdd_decompress(raw, (size_t)rd, out, outlen)) { free(raw); return 1; }
    *out = raw; *outlen = (size_t)rd; return 1;
}
static int lz_alloc(uint8_t *buf, size_t len) {
    for (int i = 0; i < ARET_LZ_MAX; i++) if (!g_lz[i].used) {
        g_lz[i].used = 1; g_lz[i].buf = buf; g_lz[i].len = len; g_lz[i].pos = 0; return i;
    }
    return -1;
}
static int lz_idx(uint32_t h) {
    if ((h & 0xFFFF0000u) != ARET_LZ_BASE) return -1;
    uint32_t i = h & 0xFFFFu;
    return (i < ARET_LZ_MAX && g_lz[i].used) ? (int)i : -1;
}
/* LZOpenFileA(lpFileName, lpReOpenBuf, wStyle) -> HFILE (an LZ handle). Fills the
 * OFSTRUCT's szPathName. */
uint32_t aret_LZOpenFileA(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ARET_HFILE_ERROR;
    uint8_t *buf; size_t len; int ok = lz_slurp(fd, &buf, &len); close(fd);
    if (!ok) return ARET_HFILE_ERROR;
    int i = lz_alloc(buf, len);
    if (i < 0) { free(buf); return ARET_HFILE_ERROR; }
    uint8_t *of = (uint8_t *)(uintptr_t)arg(esp, 1);      /* OFSTRUCT */
    if (of) { memset(of, 0, 136); of[0] = 136; const char *s = (const char *)(uintptr_t)arg(esp, 0);
              int k = 0; if (s) for (; s[k] && k < 127; k++) of[8 + k] = s[k]; of[8 + k] = 0; }
    return ARET_LZ_BASE | (uint32_t)i;
}
uint32_t aret_LZInit(uint32_t esp) {
    uint32_t hf = arg(esp, 0);
    if (lz_idx(hf) >= 0) return hf;                        /* already an LZ handle */
    uint8_t *buf; size_t len;
    if (!lz_slurp((int)hf, &buf, &len)) return ARET_HFILE_ERROR;
    int i = lz_alloc(buf, len);
    if (i < 0) { free(buf); return ARET_HFILE_ERROR; }
    return ARET_LZ_BASE | (uint32_t)i;
}
/* LZRead(hFile, lpBuffer, cbRead) -> bytes read (from the decompressed content). */
uint32_t aret_LZRead(uint32_t esp) {
    int i = lz_idx(arg(esp, 0));
    if (i < 0) return aret_lread(esp);                    /* raw fd: plain read */
    uint8_t *dst = (uint8_t *)(uintptr_t)arg(esp, 1);
    size_t want = arg(esp, 2), avail = g_lz[i].len - g_lz[i].pos;
    if (want > avail) want = avail;
    if (dst && want) memcpy(dst, g_lz[i].buf + g_lz[i].pos, want);
    g_lz[i].pos += want;
    return (uint32_t)want;
}
/* LZSeek(hFile, lOffset, iOrigin) -> new position. */
uint32_t aret_LZSeek(uint32_t esp) {
    int i = lz_idx(arg(esp, 0));
    if (i < 0) return aret_llseek(esp);
    int32_t off = (int32_t)arg(esp, 1); int origin = (int)arg(esp, 2);
    long base = origin == 1 ? (long)g_lz[i].pos : (origin == 2 ? (long)g_lz[i].len : 0);
    long np = base + off; if (np < 0) np = 0; if (np > (long)g_lz[i].len) np = (long)g_lz[i].len;
    g_lz[i].pos = (size_t)np; return (uint32_t)np;
}
uint32_t aret_LZClose(uint32_t esp) {
    int i = lz_idx(arg(esp, 0));
    if (i < 0) { close((int)arg(esp, 0)); return 0; }
    free(g_lz[i].buf); g_lz[i].used = 0; g_lz[i].buf = NULL; return 0;
}
/* LZCopy(hfSource, hfDest) -> LONG bytes copied. Source = LZ handle or raw fd;
 * dest = a writable fd (from _lcreat). */
uint32_t aret_LZCopy(uint32_t esp) {
    uint32_t src = arg(esp, 0); int dfd = (int)arg(esp, 1);
    int i = lz_idx(src);
    uint8_t *buf; size_t len; int owned = 0;
    if (i >= 0) { buf = g_lz[i].buf; len = g_lz[i].len; }
    else { if (!lz_slurp((int)src, &buf, &len)) return 0xFFFFFFFFu; owned = 1; }
    size_t off = 0; while (off < len) { ssize_t w = write(dfd, buf + off, len - off); if (w <= 0) break; off += (size_t)w; }
    if (owned) free(buf);
    return (uint32_t)off;
}
/* GetExpandedNameA(lpszSource, lpszBuffer) -> BOOL. Restore the packed file's
 * original name: the SZDD header's byte 9 is the char stripped from the extension
 * (which the packer replaced with '_'). */
uint32_t aret_GetExpandedNameA(uint32_t esp) {
    const char *src = (const char *)(uintptr_t)arg(esp, 0);
    char *dst = (char *)(uintptr_t)arg(esp, 1);
    if (!src || !dst) return 0;
    int n = 0; for (; src[n] && n < 255; n++) dst[n] = src[n]; dst[n] = 0;
    char path[1024]; translate_path(src, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        uint8_t hdr[10];
        if (read(fd, hdr, 10) == 10) {
            static const uint8_t magic[8] = { 0x53,0x5A,0x44,0x44,0x88,0xF0,0x27,0x33 };
            if (memcmp(hdr, magic, 8) == 0 && hdr[9] && hdr[9] != 0) {
                /* replace a trailing '_' in the extension with the missing char */
                int last = n - 1;
                if (last >= 0 && dst[last] == '_') dst[last] = (char)hdr[9];
            }
        }
        close(fd);
    }
    return 1;
}

/* lstrcpynA(dst, src, iMaxLength) -> dst: copy at most iMaxLength-1 chars + NUL
 * (iMaxLength counts the NUL). */
uint32_t aret_lstrcpynA(uint32_t esp) {
    char *dst = (char *)(uintptr_t)arg(esp, 0);
    const char *src = (const char *)(uintptr_t)arg(esp, 1);
    int n = (int)arg(esp, 2);
    if (!dst || n <= 0) return (uint32_t)(uintptr_t)dst;
    int i = 0;
    if (src) for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return (uint32_t)(uintptr_t)dst;
}

/* .INI profile API (GetPrivateProfileString/Int, WritePrivateProfileString).
 * Backed by a real INI file (translate_path). We match Wine's READ semantics —
 * value whitespace trimmed, one surrounding double-quote pair stripped, section/
 * key matched case-insensitively — which is all the oracle checks (the on-disk
 * layout is our own; only the read-back values are compared). */
static char *ini_slurp(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n < 0) n = 0;
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(fp); return NULL; }
    size_t rd = fread(b, 1, (size_t)n, fp); fclose(fp);
    b[rd] = 0;
    return b;
}
/* Case-insensitive compare of a[0..alen) to NUL-terminated b. */
static int ini_ieq(const char *a, int alen, const char *b) {
    int i = 0;
    for (; i < alen && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return i == alen && b[i] == 0;
}
/* Clean a value (points just after '='): trim leading/trailing ws, strip one pair
 * of surrounding double quotes. Copies into out (cap incl NUL). */
static void ini_clean_value(const char *v, const char *le, char *out, int cap) {
    while (v < le && (*v == ' ' || *v == '\t')) v++;
    const char *e = le;
    while (e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    int len = (int)(e - v);
    if (len >= 2 && v[0] == '"' && v[len - 1] == '"') { v++; len -= 2; }
    if (len > cap - 1) len = cap - 1;
    if (len < 0) len = 0;
    for (int i = 0; i < len; i++) out[i] = v[i];
    out[len] = 0;
}
/* Look up section/key in the slurped data. Returns 1 (value in out) or 0. */
static int ini_get(const char *data, const char *section, const char *key, char *out, int cap) {
    if (!data) return 0;
    const char *p = data; int in = 0;
    while (*p) {
        const char *le = p; while (*le && *le != '\n') le++;
        const char *nl = (*le == '\n') ? le + 1 : le;
        const char *t = p; while (t < le && (*t == ' ' || *t == '\t')) t++;
        if (t < le && *t == '[') {
            const char *s = t + 1, *e = s; while (e < le && *e != ']') e++;
            in = ini_ieq(s, (int)(e - s), section);
        } else if (in && t < le && *t != ';' && *t != '#' && *t != '\r') {
            const char *eq = t; while (eq < le && *eq != '=') eq++;
            if (eq < le) {
                const char *ke = eq; while (ke > t && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if (ini_ieq(t, (int)(ke - t), key)) { ini_clean_value(eq + 1, le, out, cap); return 1; }
            }
        }
        p = nl;
    }
    return 0;
}
/* Rewrite the INI content applying (section,key,value): value=NULL deletes the
 * key; key=NULL deletes the whole section; else set/insert. Returns malloc'd
 * content (*outlen), or NULL on alloc failure. */
static char *ini_rewrite(const char *data, const char *section, const char *key,
                         const char *value, size_t *outlen) {
    size_t inlen = data ? strlen(data) : 0;
    size_t cap = inlen + strlen(section) + (key ? strlen(key) : 0) + (value ? strlen(value) : 0) + 64;
    char *out = (char *)malloc(cap); if (!out) return NULL;
    size_t o = 0;
    int in_target = 0, key_done = 0, section_seen = 0;
    const char *p = data ? data : "";
    while (*p) {
        const char *ls = p, *le = p; while (*le && *le != '\n') le++;
        const char *nl = (*le == '\n') ? le + 1 : le;
        const char *t = ls; while (t < le && (*t == ' ' || *t == '\t')) t++;
        if (t < le && *t == '[') {
            if (in_target && key && value && !key_done)          /* leaving target: flush key */
                { o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value); key_done = 1; }
            const char *s = t + 1, *e = s; while (e < le && *e != ']') e++;
            int match = ini_ieq(s, (int)(e - s), section);
            in_target = match; if (match) section_seen = 1;
            if (match && !key) { p = nl; continue; }              /* delete whole section: skip */
            memcpy(out + o, ls, (size_t)(nl - ls)); o += (size_t)(nl - ls);
            p = nl; continue;
        }
        if (in_target && !key) { p = nl; continue; }              /* deleting section: skip lines */
        if (in_target && key) {
            const char *eq = t; while (eq < le && *eq != '=') eq++;
            if (eq < le) {
                const char *ke = eq; while (ke > t && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
                if (ini_ieq(t, (int)(ke - t), key)) {
                    if (value) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
                    key_done = 1; p = nl; continue;               /* replaced or deleted */
                }
            }
        }
        memcpy(out + o, ls, (size_t)(nl - ls)); o += (size_t)(nl - ls);
        p = nl;
    }
    if (key && value && !key_done) {
        if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
        if (!section_seen) o += (size_t)snprintf(out + o, cap - o, "[%s]\n%s=%s\n", section, key, value);
        else               o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
    }
    *outlen = o;
    return out;
}
uint32_t aret_GetPrivateProfileStringA(uint32_t esp) {
    const char *section = (const char *)(uintptr_t)arg(esp, 0);
    const char *key = (const char *)(uintptr_t)arg(esp, 1);
    const char *def = (const char *)(uintptr_t)arg(esp, 2);
    char *buf = (char *)(uintptr_t)arg(esp, 3);
    uint32_t size = arg(esp, 4);
    if (!buf || size == 0) return 0;
    char path[1024]; translate_path((const char *)(uintptr_t)arg(esp, 5), path, sizeof path);
    char *data = ini_slurp(path);
    char val[1024]; int found = 0;
    if (section && key) found = ini_get(data, section, key, val, sizeof val);
    if (data) free(data);
    const char *src = found ? val : (def ? def : "");
    uint32_t n = 0; for (; src[n] && n < size - 1; n++) buf[n] = src[n];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetPrivateProfileIntA(uint32_t esp) {
    const char *section = (const char *)(uintptr_t)arg(esp, 0);
    const char *key = (const char *)(uintptr_t)arg(esp, 1);
    int def = (int)arg(esp, 2);
    char path[1024]; translate_path((const char *)(uintptr_t)arg(esp, 3), path, sizeof path);
    char *data = ini_slurp(path);
    char val[64]; int found = section && key && ini_get(data, section, key, val, sizeof val);
    if (data) free(data);
    if (!found) return (uint32_t)def;
    return (uint32_t)strtol(val, NULL, 10);          /* leading integer; 0 if not numeric */
}
/* GetProfileStringA/GetProfileIntA — the win.ini variants of the private-profile
 * calls (implicit file "win.ini"); the same INI reader, no filename argument. */
uint32_t aret_GetProfileStringA(uint32_t esp) {
    const char *section = (const char *)(uintptr_t)arg(esp, 0);
    const char *key = (const char *)(uintptr_t)arg(esp, 1);
    const char *def = (const char *)(uintptr_t)arg(esp, 2);
    char *buf = (char *)(uintptr_t)arg(esp, 3);
    uint32_t size = arg(esp, 4);
    if (!buf || size == 0) return 0;
    char path[1024]; translate_path("win.ini", path, sizeof path);
    char *data = ini_slurp(path);
    char val[1024]; int found = section && key && ini_get(data, section, key, val, sizeof val);
    if (data) free(data);
    const char *src = found ? val : (def ? def : "");
    uint32_t n = 0; for (; src[n] && n < size - 1; n++) buf[n] = src[n];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetProfileIntA(uint32_t esp) {
    const char *section = (const char *)(uintptr_t)arg(esp, 0);
    const char *key = (const char *)(uintptr_t)arg(esp, 1);
    int def = (int)arg(esp, 2);
    char path[1024]; translate_path("win.ini", path, sizeof path);
    char *data = ini_slurp(path);
    char val[64]; int found = section && key && ini_get(data, section, key, val, sizeof val);
    if (data) free(data);
    if (!found) return (uint32_t)def;
    return (uint32_t)strtol(val, NULL, 10);
}
/* MoveFileA(existing, new) -> BOOL. POSIX rename (both paths translated). */
/* GetShortPathNameA(long, short, cch) -> length. Linux has no 8.3 aliasing, so the
 * short path IS the long path (copied through). */
uint32_t aret_GetShortPathNameA(uint32_t esp) {
    const char *lng = (const char *)(uintptr_t)arg(esp, 0);
    char *shrt = (char *)(uintptr_t)arg(esp, 1);
    uint32_t cch = arg(esp, 2);
    if (!lng) return 0;
    uint32_t len = (uint32_t)strlen(lng);
    if (shrt && cch > len) { memcpy(shrt, lng, len + 1); return len; }
    return len + 1;   /* required buffer size (incl NUL) */
}
uint32_t aret_MoveFileA(uint32_t esp) {
    char from[1024], to[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), from, sizeof from);
    translate_path((const char *)(uintptr_t)arg(esp, 1), to, sizeof to);
    return (uint32_t)(rename(from, to) == 0);
}
uint32_t aret_WritePrivateProfileStringA(uint32_t esp) {
    const char *section = (const char *)(uintptr_t)arg(esp, 0);
    const char *key = (const char *)(uintptr_t)arg(esp, 1);
    const char *value = (const char *)(uintptr_t)arg(esp, 2);
    if (!section) return 0;
    char path[1024]; translate_path((const char *)(uintptr_t)arg(esp, 3), path, sizeof path);
    char *data = ini_slurp(path);
    size_t olen = 0;
    char *out = ini_rewrite(data, section, key, value, &olen);
    if (data) free(data);
    if (!out) return 0;
    make_parents(path);
    FILE *fp = fopen(path, "wb");
    if (!fp) { free(out); return 0; }
    fwrite(out, 1, olen, fp); fclose(fp); free(out);
    return 1;
}

/* File mapping (CreateFileMapping/MapViewOfFile/UnmapViewOfFile) — bridged to
 * host mmap. The guest runs natively with flat pointers, so a host mmap address
 * is directly usable by the guest. A mapping HANDLE is a heap pointer (not an
 * fd), so CloseHandle must recognise it; the view base→length is tracked so
 * UnmapViewOfFile can munmap. */
#ifndef __wasm__
typedef struct { int fd; uint32_t prot; uint64_t size; } aret_mapping_t;
#define ARET_MAP_MAX 64
static aret_mapping_t *aret_maps[ARET_MAP_MAX];
static struct { void *base; size_t len; } aret_views[ARET_MAP_MAX];

uint32_t aret_CreateFileMappingA(uint32_t esp) {
    int fd = (int)arg(esp, 0);          /* INVALID_HANDLE_VALUE => anonymous */
    uint32_t prot = arg(esp, 2);        /* PAGE_READONLY=2, PAGE_READWRITE=4 */
    uint64_t size = ((uint64_t)arg(esp, 3) << 32) | arg(esp, 4);
    if (fd == (int)ARET_INVALID_HANDLE) fd = -1;
    if (size == 0 && fd >= 0) { struct stat st; if (fstat(fd, &st) == 0) size = (uint64_t)st.st_size; }
    if (size && prot == 4 && fd >= 0) { if (ftruncate(fd, (off_t)size) != 0) {/*best effort*/} }
    aret_mapping_t *m = (aret_mapping_t *)malloc(sizeof *m);
    if (!m) return 0;
    m->fd = fd; m->prot = prot; m->size = size;
    for (int i = 0; i < ARET_MAP_MAX; i++) if (!aret_maps[i]) { aret_maps[i] = m; break; }
    return (uint32_t)(uintptr_t)m;
}
uint32_t aret_CreateFileMappingW(uint32_t esp) { return aret_CreateFileMappingA(esp); }

uint32_t aret_MapViewOfFile(uint32_t esp) {
    aret_mapping_t *m = (aret_mapping_t *)(uintptr_t)arg(esp, 0);
    if (!m) return 0;
    uint32_t access = arg(esp, 1);      /* FILE_MAP_COPY=1, WRITE=2, READ=4 */
    uint64_t off = ((uint64_t)arg(esp, 2) << 32) | arg(esp, 3);
    size_t len = (size_t)arg(esp, 4);
    if (len == 0) len = (size_t)(m->size - off);
    int prot = (m->prot == 4 || access == 1 || access == 2) ? (PROT_READ | PROT_WRITE) : PROT_READ;
    int flags = (access == 1) ? MAP_PRIVATE : MAP_SHARED;
    int mfd = m->fd;
    int mflags = flags | (mfd < 0 ? MAP_ANONYMOUS : 0);
    void *base = mmap(NULL, len ? len : 1, prot, mflags, mfd, (off_t)off);
    if (base == MAP_FAILED) return 0;
    for (int i = 0; i < ARET_MAP_MAX; i++) if (!aret_views[i].base) { aret_views[i].base = base; aret_views[i].len = len; break; }
    return (uint32_t)(uintptr_t)base;
}
uint32_t aret_UnmapViewOfFile(uint32_t esp) {
    void *base = (void *)(uintptr_t)arg(esp, 0);
    for (int i = 0; i < ARET_MAP_MAX; i++) if (aret_views[i].base == base) {
        munmap(base, aret_views[i].len); aret_views[i].base = NULL; return 1;
    }
    return 0;
}
uint32_t aret_FlushViewOfFile(uint32_t esp) {
    void *base = (void *)(uintptr_t)arg(esp, 0);
    size_t n = (size_t)arg(esp, 1);
    for (int i = 0; i < ARET_MAP_MAX; i++) if (aret_views[i].base == base) {
        msync(base, n ? n : aret_views[i].len, MS_SYNC); return 1;
    }
    return 0;
}

uint32_t aret_CloseHandle(uint32_t esp) {
    uint32_t h = arg(esp, 0);
    if ((h & 0xFF000000u) == 0x70000000u) return 1;      /* thread handle: reference only */
    for (int i = 0; i < ARET_MAP_MAX; i++) if (aret_maps[i] && (uint32_t)(uintptr_t)aret_maps[i] == h) {
        free(aret_maps[i]); aret_maps[i] = NULL; return 1;
    }
    return close((int)h) == 0 ? 1 : 0;
}
#else /* __wasm__: no file mapping; a HANDLE is always an fd. */
uint32_t aret_CloseHandle(uint32_t esp) {
    uint32_t h = arg(esp, 0);
    if ((h & 0xFF000000u) == 0x70000000u) return 1;      /* thread handle: reference only */
    return close((int)h) == 0 ? 1 : 0;
}
#endif

/* CreatePipe(hRead, hWrite, sa, size) -> BOOL. An anonymous pipe maps exactly to
 * POSIX pipe(): the two HANDLEs are the read/write fds (this model's HANDLE == fd,
 * so WriteFile/ReadFile/CloseHandle operate on them directly). Faithful and
 * self-contained. (The security attributes and the advisory buffer size are not
 * modelled — the kernel picks the buffer size, as Windows also may.) */
uint32_t aret_CreatePipe(uint32_t esp) {
    uint32_t *hr = (uint32_t *)(uintptr_t)arg(esp, 0);
    uint32_t *hw = (uint32_t *)(uintptr_t)arg(esp, 1);
    if (!hr || !hw) return 0;
    int fds[2];
    if (pipe(fds) != 0) return 0;
    *hr = (uint32_t)fds[0];
    *hw = (uint32_t)fds[1];
    return 1;
}

/* GetFileSize(handle, lpFileSizeHigh) -> low 32 bits of the size (handles are
 * fds in this model). Sets *lpFileSizeHigh when non-NULL; INVALID_FILE_SIZE on
 * error. */
uint32_t aret_GetFileSize(uint32_t esp) {
    struct stat st;
    if (fstat((int)arg(esp, 0), &st) != 0) return 0xFFFFFFFFu;
    uint32_t *hi = (uint32_t *)(uintptr_t)arg(esp, 1);
    if (hi) *hi = (uint32_t)(((uint64_t)st.st_size) >> 32);
    return (uint32_t)(uint64_t)st.st_size;
}

uint32_t aret_DeleteFileA(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return unlink(path) == 0 ? 1 : 0;
}
uint32_t aret_CreateDirectoryA(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return mkdir(path, 0777) == 0 ? 1 : 0;
}
uint32_t aret_RemoveDirectoryA(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return rmdir(path) == 0 ? 1 : 0;
}

/* FindFirstFile/FindNextFile/FindClose — directory enumeration with a wildcard.
 * Bridged to opendir/readdir + fnmatch (case-insensitive, like Windows). The
 * HANDLE is a pointer to the iteration state. Fills WIN32_FIND_DATAA: the only
 * fields a CLI tool reads are the attributes, the size, and cFileName (at the
 * fixed offset 44 in the 32-bit struct); times are left zero. */
#define ARET_FFD_NAME_OFF 44u
#define FILE_ATTRIBUTE_DIRECTORY_F 0x10u
#define FILE_ATTRIBUTE_NORMAL_F 0x80u
typedef struct { DIR *d; char dir[1024]; char pat[260]; int wide; } aret_find_t;

/* Case-insensitive wildcard match (Windows file matching is case-folding);
 * FNM_CASEFOLD is a GNU extension, so lowercase both sides and use plain fnmatch. */
static int aret_ci_match(const char *pat, const char *name) {
    char lp[300], ln[300];
    size_t i;
    for (i = 0; pat[i] && i < sizeof lp - 1; i++) lp[i] = (char)tolower((unsigned char)pat[i]);
    lp[i] = 0;
    for (i = 0; name[i] && i < sizeof ln - 1; i++) ln[i] = (char)tolower((unsigned char)name[i]);
    ln[i] = 0;
    return fnmatch(lp, ln, 0);
}
static int aret_fill_find(aret_find_t *st, uint8_t *fd) {
    struct dirent *e;
    while ((e = readdir(st->d))) {
        if (aret_ci_match(st->pat, e->d_name) != 0) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", st->dir, e->d_name);
        struct stat sb;
        uint32_t attr = FILE_ATTRIBUTE_NORMAL_F;
        uint64_t size = 0;
        if (stat(full, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) attr = FILE_ATTRIBUTE_DIRECTORY_F;
            size = (uint64_t)sb.st_size;
        }
        /* WIN32_FIND_DATA{A,W} share their layout up to cFileName (offset 44);
         * only the name's width differs (char[260] vs WCHAR[260]). */
        memset(fd, 0, st->wide ? 592 : 320);
        *(uint32_t *)(fd + 0) = attr;
        *(uint32_t *)(fd + 28) = (uint32_t)(size >> 32);  /* nFileSizeHigh */
        *(uint32_t *)(fd + 32) = (uint32_t)size;          /* nFileSizeLow  */
        if (st->wide) {
            uint16_t *w = (uint16_t *)(fd + ARET_FFD_NAME_OFF);
            int i = 0;
            for (; e->d_name[i] && i < 259; i++) w[i] = (unsigned char)e->d_name[i];
            w[i] = 0;
        } else {
            snprintf((char *)(fd + ARET_FFD_NAME_OFF), 260, "%s", e->d_name);
        }
        return 1;
    }
    return 0;
}
static uint32_t aret_find_first(const char *name, uint8_t *fd, int wide) {
    char path[1024];
    translate_path(name, path, sizeof path);
    aret_find_t *st = (aret_find_t *)malloc(sizeof *st);
    if (!st) return ARET_INVALID_HANDLE;
    st->wide = wide;
    /* split into directory + filename pattern at the last separator */
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; snprintf(st->dir, sizeof st->dir, "%s", path); snprintf(st->pat, sizeof st->pat, "%s", slash + 1); }
    else { snprintf(st->dir, sizeof st->dir, "."); snprintf(st->pat, sizeof st->pat, "%s", path); }
    st->d = opendir(st->dir[0] ? st->dir : "/");
    if (!st->d) { free(st); return ARET_INVALID_HANDLE; }
    if (!aret_fill_find(st, fd)) { closedir(st->d); free(st); return ARET_INVALID_HANDLE; }
    return (uint32_t)(uintptr_t)st;
}
uint32_t aret_FindFirstFileA(uint32_t esp) {
    return aret_find_first((const char *)(uintptr_t)arg(esp, 0), (uint8_t *)(uintptr_t)arg(esp, 1), 0);
}
uint32_t aret_FindFirstFileW(uint32_t esp) {
    char name[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    return aret_find_first(name, (uint8_t *)(uintptr_t)arg(esp, 1), 1);
}
uint32_t aret_FindNextFileA(uint32_t esp) {
    aret_find_t *st = (aret_find_t *)(uintptr_t)arg(esp, 0);
    if (!st) return 0;
    return (uint32_t)aret_fill_find(st, (uint8_t *)(uintptr_t)arg(esp, 1));
}
uint32_t aret_FindNextFileW(uint32_t esp) { return aret_FindNextFileA(esp); }
uint32_t aret_FindClose(uint32_t esp) {
    aret_find_t *st = (aret_find_t *)(uintptr_t)arg(esp, 0);
    if (!st) return 0;
    closedir(st->d); free(st);
    return 1;
}

/* CRT directory iteration: _findfirst / _findnext / _findclose (msvcrt, cdecl),
 * over `struct _finddata_t` (280 bytes, MEASURED offsets): attrib@0, time_create@4,
 * time_access@8, time_write@12 (32-bit time_t), size@16 (32-bit _fsize_t), name@20
 * (char[260]). Same opendir/fnmatch machinery as FindFirstFileA, but a *different*
 * struct layout and a DIFFERENT attrib encoding — MEASURED against Wine's msvcrt:
 * a regular file is _A_ARCH(0x20), a directory is _A_SUBDIR(0x10) with no archive
 * bit, a read-only file adds _A_RDONLY(0x01). '.'/'..' are enumerated (like Wine).
 * Return conventions also differ: _findfirst -> intptr_t handle or -1 (errno=ENOENT)
 * on no match; _findnext -> 0 on success, -1 at end; _findclose -> 0. Times are the
 * real host stat times (correct values; not oracle-verified as they are env-dependent,
 * exactly like the FindFirstFile sibling). */
static int aret_fill_finddata(aret_find_t *st, uint8_t *fd) {
    struct dirent *e;
    while ((e = readdir(st->d))) {
        if (aret_ci_match(st->pat, e->d_name) != 0) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", st->dir, e->d_name);
        struct stat sb;
        uint32_t attrib = 0x20u;                       /* _A_ARCH (regular file) */
        uint64_t size = 0;
        uint32_t tc = 0, ta = 0, tw = 0;
        if (stat(full, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) attrib = 0x10u;   /* _A_SUBDIR (no archive bit) */
            else {
                if (!(sb.st_mode & S_IWUSR)) attrib |= 0x01u; /* _A_RDONLY */
                size = (uint64_t)sb.st_size;            /* Windows dirs report size 0 */
            }
            tc = (uint32_t)sb.st_ctime; ta = (uint32_t)sb.st_atime; tw = (uint32_t)sb.st_mtime;
        }
        memset(fd, 0, 280);
        *(uint32_t *)(fd + 0)  = attrib;
        *(uint32_t *)(fd + 4)  = tc;
        *(uint32_t *)(fd + 8)  = ta;
        *(uint32_t *)(fd + 12) = tw;
        *(uint32_t *)(fd + 16) = (uint32_t)size;        /* _fsize_t is 32-bit */
        snprintf((char *)(fd + 20), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}
uint32_t aret_findfirst(uint32_t esp) {
    const char *spec = (const char *)(uintptr_t)arg(esp, 0);
    uint8_t *fd = (uint8_t *)(uintptr_t)arg(esp, 1);
    char path[1024];
    translate_path(spec, path, sizeof path);
    aret_find_t *st = (aret_find_t *)malloc(sizeof *st);
    if (!st) { errno = ENOENT; return 0xFFFFFFFFu; }
    st->wide = 0;
    char *slash = strrchr(path, '/');
    if (slash) { *slash = 0; snprintf(st->dir, sizeof st->dir, "%s", path); snprintf(st->pat, sizeof st->pat, "%s", slash + 1); }
    else { snprintf(st->dir, sizeof st->dir, "."); snprintf(st->pat, sizeof st->pat, "%s", path); }
    st->d = opendir(st->dir[0] ? st->dir : "/");
    if (!st->d) { free(st); errno = ENOENT; return 0xFFFFFFFFu; }
    if (!aret_fill_finddata(st, fd)) { closedir(st->d); free(st); errno = ENOENT; return 0xFFFFFFFFu; }
    return (uint32_t)(uintptr_t)st;
}
uint32_t aret_findnext(uint32_t esp) {
    aret_find_t *st = (aret_find_t *)(uintptr_t)arg(esp, 0);
    if (!st) { errno = EINVAL; return 0xFFFFFFFFu; }
    if (aret_fill_finddata(st, (uint8_t *)(uintptr_t)arg(esp, 1))) return 0;
    errno = ENOENT;
    return 0xFFFFFFFFu;                                 /* -1 at end of directory */
}
uint32_t aret_findclose(uint32_t esp) {
    aret_find_t *st = (aret_find_t *)(uintptr_t)arg(esp, 0);
    if (!st) return 0xFFFFFFFFu;
    closedir(st->d); free(st);
    return 0;
}

/* GetFileAttributesA(name) -> DWORD. Win32 file-existence/type probe (mingw's
 * stat/access, and busybox's path handling, lean on it). stat() the translated
 * path and map the mode to the Win32 attribute bitmask, or return
 * INVALID_FILE_ATTRIBUTES (0xFFFFFFFF) when the path does not exist. */
static uint32_t aret_attr_named(const char *name) {
    char path[1024];
    translate_path(name, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) {
        /* Callers (e.g. sqlite's winAccess) inspect GetLastError to tell "does
         * not exist" (fine) from a real access error (SQLITE_IOERR). Set it. */
        g_last_error = (errno == ENOTDIR) ? 3u : 2u; /* PATH/FILE_NOT_FOUND */
        return 0xFFFFFFFFu;                          /* INVALID_FILE_ATTRIBUTES */
    }
    g_last_error = 0;
    uint32_t attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 0x10u;        /* FILE_ATTRIBUTE_DIRECTORY */
    if (!(st.st_mode & S_IWUSR)) attr |= 0x01u;    /* FILE_ATTRIBUTE_READONLY */
    if (attr == 0) attr = 0x80u;                   /* FILE_ATTRIBUTE_NORMAL */
    return attr;
}
uint32_t aret_GetFileAttributesA(uint32_t esp) {
    return aret_attr_named((const char *)(uintptr_t)arg(esp, 0));
}

/* SetFileAttributesA/W(name, attrs) -> BOOL. The only POSIX-mappable attribute is
 * FILE_ATTRIBUTE_READONLY (0x01) <-> the write permission bits; the rest (hidden/
 * system/archive) have no host analogue and are accepted-and-ignored, exactly as
 * Wine does. Round-trips with GetFileAttributes (which reports READONLY from the
 * same write bit). */
static uint32_t aret_setattr_named(const char *name, uint32_t attr) {
    char path[1024];
    translate_path(name, path, sizeof path);
    struct stat st;
    if (stat(path, &st) != 0) { g_last_error = (errno == ENOTDIR) ? 3u : 2u; return 0; }
    mode_t m = st.st_mode;
    if (attr & 0x01u) m &= ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH); /* READONLY */
    else m |= S_IWUSR;                                             /* writable */
    if (chmod(path, m) != 0) { g_last_error = 5u; return 0; }      /* ACCESS_DENIED */
    g_last_error = 0;
    return 1;
}
uint32_t aret_SetFileAttributesA(uint32_t esp) {
    return aret_setattr_named((const char *)(uintptr_t)arg(esp, 0), arg(esp, 1));
}
uint32_t aret_SetFileAttributesW(uint32_t esp) {
    char name[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    return aret_setattr_named(name, arg(esp, 1));
}
uint32_t aret_GetFileAttributesW(uint32_t esp) {
    char name[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    return aret_attr_named(name);
}

/* Fill a WIN32_FILE_ATTRIBUTE_DATA (36 bytes: attrs, 3 FILETIMEs, size hi/lo)
 * from stat(). Returns 1 on success, 0 if the path does not exist (and sets
 * GetLastError, like aret_attr_named). Shared by GetFileAttributesExA/W — the
 * standard-info level (0) is the only one used. */
static int aret_attr_ex_named(const char *name, uint32_t *out) {
    char path[1024];
    translate_path(name, path, sizeof path);
    struct stat st;
    if (!out || stat(path, &st) != 0) {
        g_last_error = (errno == ENOTDIR) ? 3u : 2u; /* PATH/FILE_NOT_FOUND */
        return 0;
    }
    g_last_error = 0;
    uint32_t attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 0x10u;      /* FILE_ATTRIBUTE_DIRECTORY */
    if (!(st.st_mode & S_IWUSR)) attr |= 0x01u;  /* FILE_ATTRIBUTE_READONLY */
    if (attr == 0) attr = 0x80u;                 /* FILE_ATTRIBUTE_NORMAL */
    /* Unix time (s) -> Windows FILETIME (100ns ticks since 1601-01-01). */
    uint64_t ct = ((uint64_t)st.st_ctime + 11644473600ULL) * 10000000ULL;
    uint64_t at = ((uint64_t)st.st_atime + 11644473600ULL) * 10000000ULL;
    uint64_t wt = ((uint64_t)st.st_mtime + 11644473600ULL) * 10000000ULL;
    uint64_t sz = (uint64_t)st.st_size;
    out[0] = attr;
    out[1] = (uint32_t)ct; out[2] = (uint32_t)(ct >> 32);   /* ftCreationTime */
    out[3] = (uint32_t)at; out[4] = (uint32_t)(at >> 32);   /* ftLastAccessTime */
    out[5] = (uint32_t)wt; out[6] = (uint32_t)(wt >> 32);   /* ftLastWriteTime */
    out[7] = (uint32_t)(sz >> 32);                          /* nFileSizeHigh */
    out[8] = (uint32_t)sz;                                  /* nFileSizeLow */
    return 1;
}
uint32_t aret_GetFileAttributesExA(uint32_t esp) {
    return aret_attr_ex_named((const char *)(uintptr_t)arg(esp, 0),
                              (uint32_t *)(uintptr_t)arg(esp, 2));
}
uint32_t aret_GetFileAttributesExW(uint32_t esp) {
    char name[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    return aret_attr_ex_named(name, (uint32_t *)(uintptr_t)arg(esp, 2));
}

uint32_t aret_DeleteFileW(uint32_t esp) {
    char name[1024], path[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    translate_path(name, path, sizeof path);
    return unlink(path) == 0 ? 1 : 0;
}

/* Widen a byte string (ASCII/Latin-1) to UTF-16LE. Writes at most cap WCHARs
 * incl. the NUL; returns the length written excluding the NUL. */
static size_t aret_n2w(const char *s, uint16_t *out, size_t cap) {
    size_t i = 0;
    if (out && cap) {
        for (; s && s[i] && i + 1 < cap; i++) out[i] = (uint16_t)(unsigned char)s[i];
        out[i] = 0;
    }
    return i;
}

/* GetFullPathNameW(name, nBufferLength(WCHARs), buf(wide), *filePart(wide**)):
 * wide sibling of GetFullPathNameA. Resolves against the cwd, sets *filePart to
 * the last path component within buf. Returns the length in WCHARs written (excl.
 * NUL), or the required size incl. NUL if the buffer is too small, 0 on failure.
 * The Win8+ VFS path of many CRT programs (e.g. sqlite) resolves paths through
 * this wide entry point. */
uint32_t aret_GetFullPathNameW(uint32_t esp) {
    char name[2048];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    uint32_t buflen = arg(esp, 1);              /* in WCHARs */
    uint16_t *buf = (uint16_t *)(uintptr_t)arg(esp, 2);
    uint32_t *filepart = (uint32_t *)(uintptr_t)arg(esp, 3);
    char tmp[4096];
    if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':')) {
        snprintf(tmp, sizeof tmp, "%s", name);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, name);
    }
    uint32_t len = (uint32_t)strlen(tmp);
    if (!buf || buflen <= len) return len + 1;  /* required size incl. NUL */
    aret_n2w(tmp, buf, buflen);
    if (filepart) {
        uint32_t sep = 0;                        /* index just past last separator */
        for (uint32_t i = 0; i < len; i++)
            if (tmp[i] == '/' || tmp[i] == '\\') sep = i + 1;
        *filepart = (uint32_t)(uintptr_t)(buf + sep);
    }
    return len;
}

/* _open(name, oflag, [pmode]) -> fd. msvcrt oflag bits differ from POSIX, so
 * translate them rather than pass through: O_RDONLY=0/O_WRONLY=1/O_RDWR=2 match,
 * but O_APPEND=0x8, O_CREAT=0x100, O_TRUNC=0x200, O_EXCL=0x400 do not (O_TEXT/
 * O_BINARY are no-ops on Linux). Placed here (after translate_path/make_parents
 * are defined) so it can use them. */
uint32_t aret_open(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    uint32_t mo = arg(esp, 1);
    int fl;
    switch (mo & 0x3u) {
        case 1: fl = O_WRONLY; break;
        case 2: fl = O_RDWR; break;
        default: fl = O_RDONLY; break;
    }
    if (mo & 0x008u) fl |= O_APPEND;
    if (mo & 0x100u) fl |= O_CREAT;
    if (mo & 0x200u) fl |= O_TRUNC;
    if (mo & 0x400u) fl |= O_EXCL;
    if (fl & O_CREAT) make_parents(path);
    int fd = open(path, fl, 0666);
    return (uint32_t)(fd < 0 ? (uint32_t)-1 : (uint32_t)fd);
}

/* ---- msvcrt stat family + CRT file-info group -----------------------------
 * msvcrt's `struct _stat` (a.k.a. _stat32, 36 bytes) and `struct _stati64`
 * (48 bytes) have a FIXED Windows field layout that does NOT match a natural
 * i386 struct: MSVC 8-byte-aligns the `__int64 st_size` (offset 24) whereas the
 * i386 System V ABI 4-byte-aligns `long long` — a natural struct would put
 * st_size at 20 and misread every field after it (a silent-wrong size). So we
 * write to explicit byte offsets, the only layout-safe way. Common head:
 *   0 dev(u32) 4 ino(u16) 6 mode(u16) 8 nlink(i16) 10 uid(i16) 12 gid(i16)
 *   16 rdev(u32); then size + a/m/ctime (32- or 64-bit size). */
static void aret_put_u16(uint8_t *b, unsigned o, uint16_t v) { b[o] = (uint8_t)v; b[o + 1] = (uint8_t)(v >> 8); }
static void aret_put_u32(uint8_t *b, unsigned o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (uint8_t)(v >> (8 * i)); }
static void aret_put_u64(uint8_t *b, unsigned o, uint64_t v) { for (int i = 0; i < 8; i++) b[o + i] = (uint8_t)(v >> (8 * i)); }

/* POSIX st_mode -> the msvcrt st_mode bits callers actually test: the file-type
 * field (_S_IFDIR 0x4000 / _S_IFCHR 0x2000 / _S_IFREG 0x8000) and read/write/exec
 * permission bits mirrored to owner/group/other — as msvcrt itself derives them.
 * The is-directory / is-regular checks (the ones programs make) depend on this. */
static uint16_t aret_msvcrt_mode(mode_t m) {
    uint16_t type = S_ISDIR(m) ? 0x4000u : (S_ISCHR(m) ? 0x2000u : 0x8000u);
    uint16_t perm = 0x0100u;                                  /* _S_IREAD (always) */
    if (m & S_IWUSR) perm |= 0x0080u;                         /* _S_IWRITE */
    if (S_ISDIR(m) || (m & S_IXUSR)) perm |= 0x0040u;         /* _S_IEXEC */
    perm |= (uint16_t)(perm >> 3) | (uint16_t)(perm >> 6);    /* mirror to grp/other */
    return (uint16_t)(type | perm);
}
static void aret_fill_stat32(uint8_t *b, const struct stat *st) {
    memset(b, 0, 36);
    aret_put_u32(b, 0, (uint32_t)st->st_dev);
    aret_put_u16(b, 4, (uint16_t)st->st_ino);
    aret_put_u16(b, 6, aret_msvcrt_mode(st->st_mode));
    aret_put_u16(b, 8, (uint16_t)st->st_nlink);
    aret_put_u32(b, 16, (uint32_t)st->st_rdev);
    aret_put_u32(b, 20, (uint32_t)st->st_size);
    aret_put_u32(b, 24, (uint32_t)st->st_atime);
    aret_put_u32(b, 28, (uint32_t)st->st_mtime);
    aret_put_u32(b, 32, (uint32_t)st->st_ctime);
}
static void aret_fill_stati64(uint8_t *b, const struct stat *st) {
    memset(b, 0, 48);
    aret_put_u32(b, 0, (uint32_t)st->st_dev);
    aret_put_u16(b, 4, (uint16_t)st->st_ino);
    aret_put_u16(b, 6, aret_msvcrt_mode(st->st_mode));
    aret_put_u16(b, 8, (uint16_t)st->st_nlink);
    aret_put_u32(b, 16, (uint32_t)st->st_rdev);
    aret_put_u64(b, 24, (uint64_t)st->st_size);          /* 8-byte-aligned per MSVC */
    aret_put_u32(b, 32, (uint32_t)st->st_atime);
    aret_put_u32(b, 36, (uint32_t)st->st_mtime);
    aret_put_u32(b, 40, (uint32_t)st->st_ctime);
}

/* _fstat(fd, struct _stat*) -> 0 / -1. */
uint32_t aret_fstat(uint32_t esp) {
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || fstat((int)arg(esp, 0), &st) != 0) return (uint32_t)-1;
    aret_fill_stat32(buf, &st);
    return 0;
}
/* _stat(path, struct _stat*) -> 0 / -1. */
uint32_t aret_stat(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || stat(path, &st) != 0) return (uint32_t)-1;
    aret_fill_stat32(buf, &st);
    return 0;
}
/* _stati64 / _stat32i64(path, struct _stati64*) -> 0 / -1 (64-bit st_size). */
uint32_t aret_stati64(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || stat(path, &st) != 0) return (uint32_t)-1;
    aret_fill_stati64(buf, &st);
    return 0;
}
/* _fstati64 / _fstat32i64(fd, struct _stati64*) -> 0 / -1. */
uint32_t aret_fstati64(uint32_t esp) {
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || fstat((int)arg(esp, 0), &st) != 0) return (uint32_t)-1;
    aret_fill_stati64(buf, &st);
    return 0;
}
/* _wstati64 / _wstat64(wpath, struct _stati64*) — wide-char path stat. */
uint32_t aret_wstati64(uint32_t esp) {
    char name[1024], path[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    translate_path(name, path, sizeof path);
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || stat(path, &st) != 0) return (uint32_t)-1;
    aret_fill_stati64(buf, &st);
    return 0;
}
uint32_t aret_wstat(uint32_t esp) {
    char name[1024], path[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    translate_path(name, path, sizeof path);
    uint8_t *buf = (uint8_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!buf || stat(path, &st) != 0) return (uint32_t)-1;
    aret_fill_stat32(buf, &st);
    return 0;
}
/* _wfopen(wpath, wmode) — wide-char fopen; returns one of our msvcrt-layout FILEs
 * (see aret_fopen). NASM opens its output file through this. */
uint32_t aret_wfopen(uint32_t esp) {
    char name[1024], mode[32], path[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 1), mode, sizeof mode);
    translate_path(name, path, sizeof path);
    if (path_is_write_mode(mode)) make_parents(path);
    int fd = open(path, fopen_mode_flags(mode), 0666);
    if (fd < 0) return 0;
    uint32_t f = alloc_dynfile(fd);
    if (!f) { close(fd); return 0; }
    return f;
}
/* _waccess(wpath, mode) — wide-char access(2). */
uint32_t aret_waccess(uint32_t esp) {
    char name[1024], path[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), name, sizeof name);
    translate_path(name, path, sizeof path);
    return (uint32_t)access(path, (int)arg(esp, 1));
}
/* _filelengthi64(fd) -> __int64 file length (edx:eax; import_returns_u64). */
uint64_t aret_filelengthi64(uint32_t esp) {
    struct stat st;
    if (fstat((int)arg(esp, 0), &st) != 0) return (uint64_t)-1;
    return (uint64_t)st.st_size;
}
/* _filelength(fd) -> long (32-bit length). */
uint32_t aret_filelength(uint32_t esp) {
    struct stat st;
    if (fstat((int)arg(esp, 0), &st) != 0) return (uint32_t)-1;
    return (uint32_t)st.st_size;
}
/* _chsize(fd, size) -> 0 / -1 (truncate/extend). */
uint32_t aret_chsize(uint32_t esp) {
    return (uint32_t)(ftruncate((int)arg(esp, 0), (off_t)(int32_t)arg(esp, 1)) == 0 ? 0 : (uint32_t)-1);
}
/* fgetpos/fsetpos(FILE*, fpos_t*) — msvcrt fpos_t is __int64 (byte offset). */
uint32_t aret_fgetpos(uint32_t esp) {
    int fd = file_fd(arg(esp, 0));
    int64_t *pos = (int64_t *)(uintptr_t)arg(esp, 1);
    if (fd < 0 || !pos) return (uint32_t)-1;
    *pos = (int64_t)lseek(fd, 0, SEEK_CUR);
    return 0;
}
uint32_t aret_fsetpos(uint32_t esp) {
    int fd = file_fd(arg(esp, 0));
    const int64_t *pos = (const int64_t *)(uintptr_t)arg(esp, 1);
    if (fd < 0 || !pos) return (uint32_t)-1;
    return (uint32_t)(lseek(fd, (off_t)*pos, SEEK_SET) < 0 ? (uint32_t)-1 : 0);
}
/* perror(s): "s: <errno string>\n" to stderr (fd 2). */
uint32_t aret_perror(uint32_t esp) {
    const char *s = (const char *)(uintptr_t)arg(esp, 0);
    const char *e = strerror(errno);
    if (s && *s) { ssize_t w = write(2, s, strlen(s)); (void)w; w = write(2, ": ", 2); (void)w; }
    ssize_t w = write(2, e, strlen(e)); (void)w; w = write(2, "\n", 1); (void)w;
    return 0;
}

/* _mkdir(path) -> 0 / -1 (msvcrt takes only the path; mode is implied 0777). */
uint32_t aret_mkdir(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)(mkdir(path, 0777) == 0 ? 0 : (uint32_t)-1);
}
/* _unlink(path) -> 0 / -1. */
uint32_t aret_unlink(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)(unlink(path) == 0 ? 0 : (uint32_t)-1);
}
/* _rmdir(path) -> 0 / -1. Remove an empty directory (POSIX rmdir). */
uint32_t aret_rmdir(uint32_t esp) {
    char path[1024];
    translate_path((const char *)(uintptr_t)arg(esp, 0), path, sizeof path);
    return (uint32_t)(rmdir(path) == 0 ? 0 : (uint32_t)-1);
}
/* _getpid() -> process id. */
uint32_t aret_getpid(uint32_t esp) {
    (void)esp;
    return (uint32_t)getpid();
}

/* ---- temp directory / temp file names -------------------------------------
 * The exact temp *path* legitimately differs from Windows (a native dir, not
 * `C:\…\Temp\`), but the *contract* is honoured: the path ends in a separator,
 * and GetTempFileName composes `<dir>\<pre><hhhh>.TMP` and — when unique==0 —
 * finds a free name and CREATES the empty file (O_CREAT|O_EXCL), returning the
 * value. The file is created at translate_path(returned name) so the caller
 * re-opening that name lands on the same file. */
static uint32_t aret_temp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    if (n + 2 > cap) return 0;
    memcpy(out, t, n);
    if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    out[n] = 0;
    return (uint32_t)n;
}
uint32_t aret_GetTempPathW(uint32_t esp) {
    uint32_t size = arg(esp, 0);                        /* buffer size in WCHARs */
    uint16_t *buf = (uint16_t *)(uintptr_t)arg(esp, 1);
    char t[4096];
    uint32_t len = aret_temp_dir(t, sizeof t);
    if (buf && size > len) { aret_n2w(t, buf, size); return len; }
    return len + 1;
}
/* Compose (and, if unique==0, create) a temp file name into `out`. Returns the
 * 16-bit unique value, or 0 on failure. */
static uint32_t aret_make_temp_name(const char *path, const char *prefix,
                                    uint32_t unique, char *out, size_t outcap) {
    char pre[4] = {0};
    for (int i = 0; i < 3 && prefix && prefix[i]; i++) pre[i] = prefix[i];
    char dir[3600];
    size_t dl = path ? strlen(path) : 0;
    if (dl + 2 >= sizeof dir) return 0;
    if (path) memcpy(dir, path, dl);
    if (dl == 0 || (dir[dl - 1] != '/' && dir[dl - 1] != '\\')) dir[dl++] = '/';
    dir[dl] = 0;
    uint32_t u = unique & 0xffffu;
    if (u == 0) {
        static uint32_t seed = 0;
        if (!seed) seed = (uint32_t)getpid();
        for (int tries = 0; tries < 65536; tries++) {
            u = (++seed) & 0xffffu; if (!u) u = 1;
            snprintf(out, outcap, "%s%s%04X.TMP", dir, pre, u);
            char host[4096];
            translate_path(out, host, sizeof host);
            int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd >= 0) { close(fd); return u; }
        }
        return 0;
    }
    snprintf(out, outcap, "%s%s%04X.TMP", dir, pre, u);
    return u;
}
uint32_t aret_GetTempFileNameA(uint32_t esp) {
    const char *path = (const char *)(uintptr_t)arg(esp, 0);
    const char *prefix = (const char *)(uintptr_t)arg(esp, 1);
    char *out = (char *)(uintptr_t)arg(esp, 3);
    if (!path || !out) return 0;
    return aret_make_temp_name(path, prefix, arg(esp, 2), out, 4096);
}
uint32_t aret_GetTempFileNameW(uint32_t esp) {
    char path[3200] = {0}, prefix[16] = {0}, out[4096];
    const uint16_t *wp = (const uint16_t *)(uintptr_t)arg(esp, 0);
    const uint16_t *wpre = (const uint16_t *)(uintptr_t)arg(esp, 1);
    uint16_t *wout = (uint16_t *)(uintptr_t)arg(esp, 3);
    if (!wp || !wout) return 0;
    aret_w2n(wp, path, sizeof path);
    if (wpre) aret_w2n(wpre, prefix, sizeof prefix);
    uint32_t u = aret_make_temp_name(path, prefix, arg(esp, 2), out, sizeof out);
    if (u) aret_n2w(out, wout, 4096);
    return u;
}
/* _wremove(wpath) -> 0 / -1 (the wide sibling of remove(), completing the wide
 * file layer). */
uint32_t aret_wremove(uint32_t esp) {
    char path[4096], host[4096];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), path, sizeof path);
    translate_path(path, host, sizeof host);
    return (uint32_t)(remove(host) == 0 ? 0 : (uint32_t)-1);
}

/* ---- file end / file times (FILETIME <-> POSIX) ---------------------------
 * FILETIME = 100-ns ticks since 1601-01-01 UTC. */
static void aret_ft_to_ts(const uint32_t *ft, struct timespec *ts) {
    uint64_t v = ((uint64_t)ft[1] << 32) | ft[0];
    if (v == 0) { ts->tv_sec = 0; ts->tv_nsec = UTIME_OMIT; return; } /* 0 -> leave unchanged */
    v -= 116444736000000000ULL;                                       /* 1601 -> 1970 epoch */
    ts->tv_sec = (time_t)(v / 10000000ULL);
    ts->tv_nsec = (long)((v % 10000000ULL) * 100ULL);
}
static void aret_ts_to_ft(time_t sec, long nsec, uint32_t *out) {
    uint64_t v = ((uint64_t)sec + 11644473600ULL) * 10000000ULL + (uint64_t)nsec / 100ULL;
    out[0] = (uint32_t)v;
    out[1] = (uint32_t)(v >> 32);
}
/* SetEndOfFile(h) -> truncate the file to the current file position. */
uint32_t aret_SetEndOfFile(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0) return 0;
    return (uint32_t)(ftruncate(fd, pos) == 0 ? 1 : 0);
}
/* SetFileTime(h, create, access, write): sets access+write (Linux has no settable
 * creation time; a NULL or all-zero field is left unchanged, as on Windows). */
uint32_t aret_SetFileTime(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    const uint32_t *at = (const uint32_t *)(uintptr_t)arg(esp, 2);
    const uint32_t *wt = (const uint32_t *)(uintptr_t)arg(esp, 3);
    struct timespec ts[2];
    if (at) aret_ft_to_ts(at, &ts[0]); else { ts[0].tv_sec = 0; ts[0].tv_nsec = UTIME_OMIT; }
    if (wt) aret_ft_to_ts(wt, &ts[1]); else { ts[1].tv_sec = 0; ts[1].tv_nsec = UTIME_OMIT; }
    return (uint32_t)(futimens(fd, ts) == 0 ? 1 : 0);
}
/* GetFileTime(h, create, access, write): fills the FILETIMEs it is handed. No
 * birth time on Linux via fstat, so creation reuses ctime (as Wine also does). */
uint32_t aret_GetFileTime(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    uint32_t *ct = (uint32_t *)(uintptr_t)arg(esp, 1);
    uint32_t *at = (uint32_t *)(uintptr_t)arg(esp, 2);
    uint32_t *wt = (uint32_t *)(uintptr_t)arg(esp, 3);
    if (ct) aret_ts_to_ft(st.st_ctime, 0, ct);
    if (at) aret_ts_to_ft(st.st_atime, 0, at);
    if (wt) aret_ts_to_ft(st.st_mtime, 0, wt);
    return 1;
}
/* GetFileInformationByHandle(h, BY_HANDLE_FILE_INFORMATION*): fstat the fd and
 * fill the 52-byte record — attrs, 3 FILETIMEs, volume serial, 64-bit size,
 * link count, and the 64-bit file index (inode). Handles are fds in this model
 * (as in GetFileSize/GetFileTime). Used by mingw CRT `fstat`/`isatty` paths and
 * tools that dedup by (volume, file index) — e.g. GNU m4. */
uint32_t aret_GetFileInformationByHandle(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    uint32_t *out = (uint32_t *)(uintptr_t)arg(esp, 1);
    struct stat st;
    if (!out || fstat(fd, &st) != 0) { g_last_error = 6u; /* ERROR_INVALID_HANDLE */ return 0; }
    uint32_t attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 0x10u;      /* FILE_ATTRIBUTE_DIRECTORY */
    if (!(st.st_mode & S_IWUSR)) attr |= 0x01u;  /* FILE_ATTRIBUTE_READONLY */
    if (attr == 0) attr = 0x80u;                 /* FILE_ATTRIBUTE_NORMAL */
    uint64_t sz = (uint64_t)st.st_size;
    uint64_t ino = (uint64_t)st.st_ino;
    out[0] = attr;                               /* dwFileAttributes     @0  */
    aret_ts_to_ft(st.st_ctime, 0, out + 1);      /* ftCreationTime       @4  */
    aret_ts_to_ft(st.st_atime, 0, out + 3);      /* ftLastAccessTime     @12 */
    aret_ts_to_ft(st.st_mtime, 0, out + 5);      /* ftLastWriteTime      @20 */
    out[7] = (uint32_t)st.st_dev;                /* dwVolumeSerialNumber @28 */
    out[8] = (uint32_t)(sz >> 32);               /* nFileSizeHigh        @32 */
    out[9] = (uint32_t)sz;                        /* nFileSizeLow         @36 */
    out[10] = (uint32_t)st.st_nlink;             /* nNumberOfLinks       @40 */
    out[11] = (uint32_t)(ino >> 32);             /* nFileIndexHigh       @44 */
    out[12] = (uint32_t)ino;                      /* nFileIndexLow        @48 */
    g_last_error = 0;
    return 1;
}
/* Local<->UTC FILETIME: a constant shift by the *current* timezone bias (Windows
 * uses the current bias, not the historical one). tm_gmtoff is seconds east of
 * UTC; under a UTC timezone (the differential harness) this is the identity. */
static int64_t aret_tz_off_100ns(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (int64_t)lt.tm_gmtoff * 10000000LL;
}
uint32_t aret_LocalFileTimeToFileTime(uint32_t esp) {
    const uint32_t *in = (const uint32_t *)(uintptr_t)arg(esp, 0);
    uint32_t *out = (uint32_t *)(uintptr_t)arg(esp, 1);
    if (!in || !out) return 0;
    uint64_t v = (((uint64_t)in[1] << 32) | in[0]) - (uint64_t)aret_tz_off_100ns();
    out[0] = (uint32_t)v; out[1] = (uint32_t)(v >> 32);
    return 1;
}
uint32_t aret_FileTimeToLocalFileTime(uint32_t esp) {
    const uint32_t *in = (const uint32_t *)(uintptr_t)arg(esp, 0);
    uint32_t *out = (uint32_t *)(uintptr_t)arg(esp, 1);
    if (!in || !out) return 0;
    uint64_t v = (((uint64_t)in[1] << 32) | in[0]) + (uint64_t)aret_tz_off_100ns();
    out[0] = (uint32_t)v; out[1] = (uint32_t)(v >> 32);
    return 1;
}

uint32_t aret_SetFilePointer(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    int32_t dist = (int32_t)arg(esp, 1);
    uint32_t method = arg(esp, 3); /* 0 FILE_BEGIN, 1 FILE_CURRENT, 2 FILE_END */
    int whence = method == 1 ? SEEK_CUR : (method == 2 ? SEEK_END : SEEK_SET);
    off_t r = lseek(fd, dist, whence);
    return (uint32_t)r;
}
/* SetFilePointerEx(h, liDistance[by value, lo@1 hi@2], *newPos, method) -> BOOL */
uint32_t aret_SetFilePointerEx(uint32_t esp) {
    int fd = (int)arg(esp, 0);
    int64_t dist = (int64_t)(((uint64_t)arg(esp, 2) << 32) | arg(esp, 1));
    uint32_t method = arg(esp, 4);
    int whence = method == 1 ? SEEK_CUR : (method == 2 ? SEEK_END : SEEK_SET);
    off_t r = lseek(fd, (off_t)dist, whence);
    if (r < 0) return 0;
    uint64_t *np = (uint64_t *)(uintptr_t)arg(esp, 3);
    if (np) *np = (uint64_t)r;
    return 1;
}
/* GetFileSizeEx(h, *LARGE_INTEGER) -> BOOL (handles are fds). */
uint32_t aret_GetFileSizeEx(uint32_t esp) {
    struct stat st;
    if (fstat((int)arg(esp, 0), &st) != 0) return 0;
    uint64_t *p = (uint64_t *)(uintptr_t)arg(esp, 1);
    if (p) *p = (uint64_t)st.st_size;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

/* Reported by the builder's weak per-import stubs when an unimplemented API is
 * actually called. Warn once per name (the stubs pass stable string literals, so
 * pointer comparison de-dups), then let the program limp on. */
void aret_unimpl(const char *name) {
    static const char *seen[1024];
    static int nseen = 0;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == name) return;
    if (nseen < 1024) seen[nseen++] = name;
    fprintf(stderr, "ARET: unimplemented import called: %s\n", name);
}

/* Reached an instruction the lifter could not model: fail loud rather than
 * silently substitute a no-op (which would be a wrong result presented as
 * correct). The address/text pinpoints what to model next. */
/* ---- Execution trace (doc 81 §I1) -------------------------------------------------
 * A lock-free in-memory ring buffer recording each lifted function's entry (VA + esp +
 * the threaded register params). Populated only in a `--trace` build (the emitter
 * prefixes each body with `aret_trace_push`); dumped only on a crash path
 * (aret_unmodelled / an unhandled hardware fault). No I/O on the hot path, off by
 * default → the normal product is unaffected. The dump reconstructs the call chain and
 * register state leading to a late corruption — the accelerator for the MFC
 * lift-correctness mop-up. Mono-fiber for now (a per-fiber buffer is the multi-thread
 * extension); the head is a plain counter — a torn write at a crash only garbles one
 * old row. */
#define ARET_TRACE_N 65536u
struct aret_trace_ent { uint32_t va, esp, eax, ecx, edx, ebp, esi, edi, ebx; };
static struct aret_trace_ent aret_trace_buf[ARET_TRACE_N];
static uint32_t aret_trace_head = 0;
void aret_trace_push(uint32_t va, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx,
                     uint32_t ebp, uint32_t esi, uint32_t edi, uint32_t ebx) {
    struct aret_trace_ent *e = &aret_trace_buf[aret_trace_head & (ARET_TRACE_N - 1u)];
    e->va = va; e->esp = esp; e->eax = eax; e->ecx = ecx; e->edx = edx;
    e->ebp = ebp; e->esi = esi; e->edi = edi; e->ebx = ebx;
    aret_trace_head++;
}
void aret_trace_dump(void) {
    if (aret_trace_head == 0) return;
    uint32_t n = aret_trace_head < ARET_TRACE_N ? aret_trace_head : ARET_TRACE_N;
    /* The recent tail is what matters near a crash, so 400 is the default. But an
     * esp drift is diagnosed by comparing a function's ENTRY esp with the esp at
     * its epilogue, and a big MFC function can make thousands of calls in between —
     * its entry then falls outside a 400-row window even though the ring (65536)
     * still holds it. `ARET_TRACE_DUMP=N` widens the window to reach it; 0 means
     * dump everything the ring has. */
    uint32_t cap = 400u;
    { const char *s = getenv("ARET_TRACE_DUMP");
      if (s && *s) { unsigned long v = strtoul(s, NULL, 0); cap = (v == 0) ? ARET_TRACE_N : (uint32_t)v; } }
    if (n > cap) n = cap;
    fprintf(stderr, "=== ARET execution trace (last %u function entries, newest last) ===\n", n);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = aret_trace_head - n + k;
        struct aret_trace_ent *e = &aret_trace_buf[idx & (ARET_TRACE_N - 1u)];
        fprintf(stderr,
                "  sub_%x esp=%#x eax=%#x ecx=%#x edx=%#x ebp=%#x esi=%#x edi=%#x ebx=%#x\n",
                e->va, e->esp, e->eax, e->ecx, e->edx, e->ebp, e->esi, e->edi, e->ebx);
    }
    fprintf(stderr, "=== end trace ===\n");
}

void aret_unmodelled(const char *insn) {
    fprintf(stderr, "ARET: reached an unmodelled instruction: %s\n", insn);
    fprintf(stderr, "ARET: aborting — translation is incomplete here; refusing to guess.\n");
    aret_trace_dump();
    abort();
}

/* Bound-away startup glue: do nothing, return 0. */
uint32_t aret_noop(uint32_t esp) { (void)esp; return 0; }

/* The x87 fp return channel (st(0) across calls). One shared definition; the
 * __x87_retstore/__x87_retload helpers (emit::FLOAT_HELPERS) read/write it. */
long double __aret_x87_ret;
/* Set by any fp-channel writer (static ret, host libm), cleared by any consumer.
 * Lets a runtime-mode caller tell, after an INDIRECT call, whether the callee
 * returned an fp value via the channel (→ push it) or not. */
int __aret_x87_ret_valid;

/* The runtime x87 stack model (fallback for depth-bailed functions). ONE shared
 * definition — it is the physical FPU stack, so a value one runtime-mode function
 * leaves in st(0) is visible to its runtime-mode caller across chunk boundaries. */
long double __x87rt_s[16];
int __x87rt_p;
int __x87rt_rc;
unsigned short __x87rt_sw;

/* Runtime x87 stack under/overflow. The guard itself is old and SOUND (§0: an
 * inconsistent stack traps rather than reading a stale slot); what was missing is
 * that it was MUTE — a bare __builtin_trap gives no reason, and, worse, kills the
 * process with stdout still buffered, so the program's output up to that point is
 * LOST. A run that had in fact progressed a long way then looks like it produced
 * nothing at all, which sends the reader hunting for a phantom early failure.
 *
 * So: flush stdout FIRST (preserve the evidence of how far it got), then say which
 * op faulted at what depth, then dump the trace and abort exactly as before. The
 * abort is unchanged — this only makes the existing loud failure diagnostic.
 *
 * `depth` is the live __x87rt_p; `i` is the st(i) index requested (-1 for a push,
 * which has no index). A negative depth means the model popped more than it pushed
 * (a value an unrecognised callee consumed); depth >= 16 means genuine overflow. */
__attribute__((noreturn))
void aret_x87_stack_error(const char *op, int i, int depth) {
    fflush(stdout);
    fprintf(stderr, "ARET: x87 runtime stack %s in %s: ",
            depth < 0 || (i >= 0 && depth - 1 - i < 0) ? "UNDERFLOW" : "OVERFLOW", op);
    if (i >= 0) {
        fprintf(stderr, "requested st(%d) at depth %d (slot %d)\n", i, depth, depth - 1 - i);
    } else {
        fprintf(stderr, "push at depth %d\n", depth);
    }
    fprintf(stderr, "ARET: the modelled FPU stack is inconsistent — most likely an "
                    "unrecognised callee left/consumed a value in st(0) that the model "
                    "never saw. Refusing to read a stale slot.\n");
    aret_trace_dump();
    abort();
}

/* ------------------------------------------------------------------ */
/* setjmp/longjmp support.                                            */
/*                                                                    */
/* The actual setjmp()/longjmp() are expanded at the lifted call site */
/* (macros in aret_decls.h), so they run in the lifted function's own */
/* native frame — the native call stack mirrors the program's logical */
/* stack 1:1, so a native longjmp unwinds it exactly as intended. A   */
/* shim function could not: its setjmp frame would have returned       */
/* before the longjmp, which is undefined.                            */
/*                                                                    */
/* This maps the program's own jmp_buf address (the key) to a host    */
/* jmp_buf, so a longjmp finds the context its matching setjmp saved.  */
/* Reused by address, matching the LIFO nesting of protected calls.   */
/* Excluded from WASM: WASI has no usable setjmp/longjmp, and the     */
/* macro block that needs these is emitted only for the native target. */
/* ------------------------------------------------------------------ */
#ifndef __wasm__
#include <setjmp.h>
jmp_buf *aret_jmpbuf_for(uint32_t key) {
    enum { N = 512 };
    static uint32_t keys[N];
    static jmp_buf bufs[N];
    static int n = 0;
    for (int i = 0; i < n; i++)
        if (keys[i] == key) return &bufs[i];
    if (n < N) { keys[n] = key; return &bufs[n++]; }
    keys[0] = key;   /* extreme nesting: reuse rather than overflow */
    return &bufs[0];
}

/* longjmp is safe to wrap in a function (unlike setjmp): it unwinds *to* a
 * setjmp elsewhere, so its own frame need not survive. */
void aret_longjmp_do(uint32_t key, int val) {
    longjmp(*aret_jmpbuf_for(key), val ? val : 1);
}
#endif /* !__wasm__ */

/* ------------------------------------------------------------------ */
/* C runtime bring-up (mingw/MSVC startup)                            */
/* ------------------------------------------------------------------ */

/* kernel32 process/module/sync (best-effort) */
uint32_t aret_GetModuleHandleA(uint32_t esp) { (void)esp; return 0x00400000u; }
uint32_t aret_GetModuleHandleW(uint32_t esp) { (void)esp; return 0x00400000u; }
uint32_t aret_GetProcAddress(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_LoadLibraryA(uint32_t esp) { (void)esp; return 0x10000000u; }
/* LoadLibraryExA(name, hFile, flags): like LoadLibraryA, return a non-NULL fake
 * handle. The msvcrt delay-load glue probes for kernel32.dll/etc.; a NULL here
 * makes it fall back to a disk search that builds a path from a NULL name and
 * crashes. The actual symbols are already intercepted as imports/shims. */
uint32_t aret_LoadLibraryExA(uint32_t esp) { (void)esp; return 0x10000000u; }
uint32_t aret_LoadLibraryExW(uint32_t esp) { (void)esp; return 0x10000000u; }
uint32_t aret_FreeLibrary(uint32_t esp) { (void)esp; return 1; }
/* The process top-level unhandled-exception filter (SetUnhandledExceptionFilter
 * installs it, UnhandledExceptionFilter runs it). Stateful: the setter returns the
 * previously installed filter, like Wine — a program that saves and restores it must
 * get its own pointer back. */
static uint32_t g_top_exception_filter = 0;
uint32_t aret_SetUnhandledExceptionFilter(uint32_t esp) {
    uint32_t prev = g_top_exception_filter;
    g_top_exception_filter = arg(esp, 0);
    return prev;
}
/* UnhandledExceptionFilter(ExceptionInfo) -> LONG. The CRT's top-level __except calls
 * this when an exception reaches the outermost frame unhandled: run the installed
 * top-level filter if any (its LONG disposition is returned), else return
 * EXCEPTION_EXECUTE_HANDLER (1) so the CRT runs its terminate handler — there is no
 * debugger to attach to. The filter is WINAPI(EXCEPTION_POINTERS*); lay its single
 * argument on the free machine stack below esp and dispatch through aret_call. */
uint32_t aret_UnhandledExceptionFilter(uint32_t esp) {
    if (g_top_exception_filter) {
        uint32_t hesp = esp - 0x40;
        uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = arg(esp, 0);         /* ExceptionInfo @ [esp+4] */
        return (uint32_t)aret_call(g_top_exception_filter, hesp, 0, 0, 0, 0, 0, 0, 0);
    }
    return 1;                        /* EXCEPTION_EXECUTE_HANDLER */
}
uint32_t aret_VirtualProtect(uint32_t esp) { (void)esp; return 1; }
/* VirtualQuery(lpAddress, lpBuffer, dwLength): report the page containing
 * lpAddress as committed, image-backed, executable-readwrite memory. mingw-w64's
 * pseudo-relocation runtime (`_pei386_runtime_relocator` -> `mark_section_writable`)
 * calls this to find the region to VirtualProtect writable before applying import
 * relocations; a 0 return makes it print "VirtualQuery failed ..." and abort().
 * Our address space is a flat, always-writable host mapping (VirtualProtect is a
 * no-op), so any plausible non-zero descriptor lets startup proceed. */
uint32_t aret_VirtualQuery(uint32_t esp) {
    uint32_t addr = arg(esp, 0);
    uint32_t buf = arg(esp, 1);
    uint32_t len = arg(esp, 2);
    if (!buf || len < 28) return 0;
    uint32_t *mbi = (uint32_t *)(uintptr_t)buf;
    uint32_t base = addr & ~0xFFFu;   /* page-aligned base */
    mbi[0] = base;        /* BaseAddress                      */
    mbi[1] = base;        /* AllocationBase                   */
    mbi[2] = 0x40;        /* AllocationProtect = EXEC_READWRITE */
    mbi[3] = 0x10000;     /* RegionSize (64 KiB granularity)  */
    mbi[4] = 0x1000;      /* State = MEM_COMMIT               */
    mbi[5] = 0x40;        /* Protect = PAGE_EXECUTE_READWRITE */
    mbi[6] = 0x1000000;   /* Type = MEM_IMAGE                 */
    return 28;
}
/* Thread-local storage (doc 80 incr. 4): index allocation is process-global, but
 * the *values* are PER-FIBER — each fiber sees its own slot values (a fresh thread
 * starts with all slots NULL). Rows indexed by the running fiber (0 = main). */
#define ARET_TLS_MAX 1088
#define ARET_TLS_FIBERS 64          /* must match U32_MAX_FIBER in aret_win32.c */
static uint32_t aret_tls[ARET_TLS_FIBERS][ARET_TLS_MAX];
static char aret_tls_used[ARET_TLS_MAX];
uint32_t aret_TlsAlloc(uint32_t esp) {
    (void)esp;
    for (int i = 0; i < ARET_TLS_MAX; i++) if (!aret_tls_used[i]) {
        aret_tls_used[i] = 1;
        for (int f = 0; f < ARET_TLS_FIBERS; f++) aret_tls[f][i] = 0;  /* NULL in every fiber */
        return (uint32_t)i;
    }
    return 0xFFFFFFFFu; /* TLS_OUT_OF_INDEXES */
}
uint32_t aret_TlsGetValue(uint32_t esp) {
    uint32_t i = arg(esp, 0);
    return i < ARET_TLS_MAX ? aret_tls[aret_current_fiber()][i] : 0;
}
uint32_t aret_TlsSetValue(uint32_t esp) {
    uint32_t i = arg(esp, 0);
    if (i >= ARET_TLS_MAX) return 0;
    aret_tls[aret_current_fiber()][i] = arg(esp, 1);
    return 1;
}
uint32_t aret_TlsFree(uint32_t esp) {
    uint32_t i = arg(esp, 0);
    if (i >= ARET_TLS_MAX) return 0;
    aret_tls_used[i] = 0;
    return 1;
}
/* EncodePointer/DecodePointer obfuscate a pointer with a per-process cookie; the
 * value is opaque to the program, so identity is correct and round-trips. */
uint32_t aret_EncodePointer(uint32_t esp) { return arg(esp, 0); }
uint32_t aret_DecodePointer(uint32_t esp) { return arg(esp, 0); }
/* CriticalSection (Initialize/Enter/Leave/Try/Delete) lives in aret_win32.c with
 * the cooperative-fiber scheduler it must drive under contention (doc 80 incr. 2). */
/* InitializeSListHead(PSLIST_HEADER): zero the lock-free list header so later
 * SList queries see an empty list. The header is 8 bytes on x86 (a union with a
 * 64-bit Alignment); zero 16 to be safe. A no-op stub left it uninitialised, so
 * the first query dereferenced garbage. */
uint32_t aret_InitializeSListHead(uint32_t esp) {
    void *h = (void *)(uintptr_t)arg(esp, 0);
    if (h) memset(h, 0, 16);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Version info (VS_VERSIONINFO): a program reading its own version.   */
/* We serve the PE's own RT_VERSION resource, located in the mapped    */
/* image, and parse the VS_VERSIONINFO tree for VerQueryValue.         */
/* ------------------------------------------------------------------ */
extern uint32_t aret_image_lo, aret_image_hi;

static uint16_t vi_w(const uint8_t *p, int o) { return (uint16_t)(p[o] | (p[o + 1] << 8)); }
static int vi_pad4(int x) { return (x + 3) & ~3; }
/* Bytes of the UTF-16 key (incl. NUL), measured from the node's szKey at +6. */
static int vi_keybytes(const uint8_t *node) { int i = 6; while (node[i] || node[i + 1]) i += 2; return i + 2 - 6; }
static int vi_value_off(const uint8_t *node) { return vi_pad4(6 + vi_keybytes(node)); }
/* Value size in bytes: wValueLength is in WCHARs for text nodes (wType==1). */
static int vi_value_bytes(const uint8_t *node) { int v = vi_w(node, 2); return vi_w(node, 4) == 1 ? v * 2 : v; }
static int vi_children_off(const uint8_t *node) { return vi_pad4(vi_value_off(node) + vi_value_bytes(node)); }
/* node key (UTF-16 at +6) == ANSI s ? Case-insensitive, as the real
 * VerQueryValue: the language-codepage block key is stored lowercase
 * ("040904b0") but callers build the query from `Translation` in uppercase. */
static int vi_keyeq(const uint8_t *node, const char *s) {
    int i = 6;
    for (; *s; s++, i += 2) {
        if (node[i + 1]) return 0;
        char a = (char)node[i], b = *s;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return node[i] == 0 && node[i + 1] == 0;
}
static const uint8_t *vi_child(const uint8_t *node, const char *key) {
    int nlen = vi_w(node, 0);
    const uint8_t *c = node + vi_children_off(node), *end = node + nlen;
    while (c + 6 <= end) {
        int clen = vi_w(c, 0);
        if (clen < 6) break;
        if (vi_keyeq(c, key)) return c;
        c += vi_pad4(clen);
    }
    return NULL;
}
/* Locate VS_VERSIONINFO in the mapped image by the VS_FIXEDFILEINFO signature
 * 0xFEEF04BD, which sits at offset 0x28 from the VS_VERSIONINFO start. */
static const uint8_t *aret_find_versioninfo(void) {
    if (!aret_image_lo || aret_image_hi <= aret_image_lo) return NULL;
    const uint8_t *base = (const uint8_t *)(uintptr_t)aret_image_lo;
    size_t n = aret_image_hi - aret_image_lo;
    for (size_t i = 0x28; i + 4 <= n; i += 4) {
        if (base[i] == 0xBD && base[i + 1] == 0x04 && base[i + 2] == 0xEF && base[i + 3] == 0xFE) {
            const uint8_t *vi = base + i - 0x28;
            if (vi_w(vi, 0) >= 0x28 && vi_keyeq(vi, "VS_VERSION_INFO")) return vi;
        }
    }
    return NULL;
}
uint32_t aret_GetFileVersionInfoSizeA(uint32_t esp) {
    if (arg(esp, 1)) *(uint32_t *)(uintptr_t)arg(esp, 1) = 0; /* lpdwHandle */
    const uint8_t *vi = aret_find_versioninfo();
    return vi ? vi_w(vi, 0) : 0;
}
/* The ANSI GetFileVersionInfoA returns a buffer whose StringFileInfo *string
 * values* are ANSI (the W variant keeps them UTF-16). Narrow each in place: the
 * value region is left the same size (wValueLength stays in WCHARs so the tree
 * offsets are unchanged), only its first wValueLength bytes are rewritten as the
 * low byte of each WCHAR — so a `printf("%s", value)` reads the real text. */
static void vi_narrow_strings(uint8_t *vi) {
    const uint8_t *sfi = vi_child(vi, "StringFileInfo");
    if (!sfi) return;
    int sfilen = vi_w(sfi, 0);
    const uint8_t *lang = sfi + vi_children_off(sfi);
    while (lang + 6 < sfi + sfilen) {
        int langlen = vi_w(lang, 0);
        if (langlen < 6) break;
        const uint8_t *str = lang + vi_children_off(lang);
        while (str + 6 < lang + langlen) {
            int strlen_ = vi_w(str, 0);
            if (strlen_ < 6) break;
            int vlen = vi_w(str, 2); /* WCHARs */
            if (vi_w(str, 4) == 1 && vlen > 0) {
                uint8_t *val = (uint8_t *)str + vi_value_off(str);
                for (int j = 0; j < vlen; j++) val[j] = val[j * 2];
            }
            str += vi_pad4(strlen_);
        }
        lang += vi_pad4(langlen);
    }
}
uint32_t aret_GetFileVersionInfoA(uint32_t esp) {
    const uint8_t *vi = aret_find_versioninfo();
    void *data = (void *)(uintptr_t)arg(esp, 3);
    uint32_t len = arg(esp, 2);
    if (!vi || !data) return 0;
    uint32_t n = vi_w(vi, 0);
    if (n > len) n = len;
    memcpy(data, vi, n);
    vi_narrow_strings((uint8_t *)data);
    return 1;
}
/* VerQueryValueA(block, "\Sub\Block", &buf, &len): navigate the tree. "\" -> the
 * VS_FIXEDFILEINFO; "\StringFileInfo\<lang>\<key>" / "\VarFileInfo\Translation"
 * -> the leaf value. Buffer/len are returned as for the real API. */
uint32_t aret_VerQueryValueA(uint32_t esp) {
    const uint8_t *root = (const uint8_t *)(uintptr_t)arg(esp, 0);
    const char *sub = (const char *)(uintptr_t)arg(esp, 1);
    uint32_t *pbuf = (uint32_t *)(uintptr_t)arg(esp, 2);
    uint32_t *plen = (uint32_t *)(uintptr_t)arg(esp, 3);
    if (!root || !sub || !pbuf || !plen) return 0;
    const uint8_t *node = root;
    if (sub[0] == '\\' && sub[1] == 0) {
        /* root value = VS_FIXEDFILEINFO */
        *pbuf = (uint32_t)(uintptr_t)(node + vi_value_off(node));
        *plen = vi_w(node, 2);
        return 1;
    }
    const char *p = sub;
    while (*p == '\\') {
        p++;
        char comp[128];
        int k = 0;
        while (*p && *p != '\\' && k < 127) comp[k++] = *p++;
        comp[k] = 0;
        if (k == 0) break;
        node = vi_child(node, comp);
        if (!node) return 0;
    }
    /* leaf value: length in chars (text) or bytes (binary), as the real API. */
    *pbuf = (uint32_t)(uintptr_t)(node + vi_value_off(node));
    *plen = vi_w(node, 2);
    return 1;
}
uint32_t aret_IsDBCSLeadByteEx(uint32_t esp) { (void)esp; return 0; }
/* IsDBCSLeadByte(byte): true iff `byte` starts a double-byte char in the ANSI code
 * page. The modelled ACP is CP1252 (Western, GetACP()=1252), a single-byte codepage,
 * so no byte is ever a lead byte -> always 0 (measured vs Wine). */
uint32_t aret_IsDBCSLeadByte(uint32_t esp) { (void)esp; return 0; }
/* CP_ACP narrow<->wide: model the code page as Latin-1 (each byte is one WCHAR).
 * Enough for ASCII text — the common case — and a faithful identity round-trip.
 * srclen < 0 means a NUL-terminated string (the terminator is included); a zero
 * destination length means "measure" (return the count that would be written). */
uint32_t aret_MultiByteToWideChar(uint32_t esp) {
    const char *src = (const char *)(uintptr_t)arg(esp, 2);
    int srclen = (int)arg(esp, 3);
    uint16_t *dst = (uint16_t *)(uintptr_t)arg(esp, 4);
    int dstlen = (int)arg(esp, 5);
    if (!src) return 0;
    int n = (srclen < 0) ? (int)strlen(src) + 1 : srclen;
    if (dstlen == 0 || !dst) return (uint32_t)n;
    int w = n < dstlen ? n : dstlen;
    for (int i = 0; i < w; i++) dst[i] = (unsigned char)src[i];
    return (uint32_t)w;
}
uint32_t aret_WideCharToMultiByte(uint32_t esp) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)arg(esp, 2);
    int srclen = (int)arg(esp, 3);
    char *dst = (char *)(uintptr_t)arg(esp, 4);
    int dstlen = (int)arg(esp, 5);
    if (!src) return 0;
    int n;
    if (srclen < 0) { n = 0; while (src[n]) n++; n++; } else n = srclen;
    if (dstlen == 0 || !dst) return (uint32_t)n;
    int w = n < dstlen ? n : dstlen;
    for (int i = 0; i < w; i++) dst[i] = (char)(src[i] & 0xff);
    return (uint32_t)w;
}
/* GetCommandLineA/W: rebuild the command line from the real argv (the native
 * process's arguments, captured in aret_real_argc/argv), quoting any argument
 * that contains whitespace — the inverse of CommandLineToArgv, so a program that
 * re-parses it sees the same arguments it would on Windows. */
static void aret_build_cmdline(char *out, size_t cap) {
    extern int aret_real_argc;
    extern char **aret_real_argv;
    size_t o = 0;
    for (int i = 0; i < aret_real_argc && o + 2 < cap; i++) {
        const char *a = aret_real_argv[i];
        int quote = (a[0] == 0) || strpbrk(a, " \t") != NULL;
        if (i && o + 1 < cap) out[o++] = ' ';
        if (quote && o + 1 < cap) out[o++] = '"';
        for (const char *p = a; *p && o + 2 < cap; p++) out[o++] = *p;
        if (quote && o + 1 < cap) out[o++] = '"';
    }
    out[o] = 0;
}
uint32_t aret_GetCommandLineA(uint32_t esp) {
    (void)esp;
    static char cmd[8192];
    static int built = 0;
    if (!built) { built = 1; aret_build_cmdline(cmd, sizeof cmd); }
    return (uint32_t)(uintptr_t)cmd;
}
uint32_t aret_GetCommandLineW(uint32_t esp) {
    (void)esp;
    static uint16_t wcmd[8192];
    static int built = 0;
    if (!built) {
        built = 1;
        char tmp[8192];
        aret_build_cmdline(tmp, sizeof tmp);
        size_t i = 0;
        for (; tmp[i] && i < sizeof(wcmd) / sizeof(wcmd[0]) - 1; i++) wcmd[i] = (unsigned char)tmp[i];
        wcmd[i] = 0;
    }
    return (uint32_t)(uintptr_t)wcmd;
}

/* __p__acmdln / __p__wcmdln: msvcrt exposes the raw command line as `char* _acmdln`
 * / `wchar_t* _wcmdln`; a statically-linked CRT parses it into argv/wargv before
 * main. Return the address of a pointer to our rebuilt command line (same content
 * as GetCommandLineA). Returning 0 (weak stub) left the CRT with no command line,
 * so it built an empty argv and the program saw no arguments (NASM printed nothing
 * for `-v`, every file argument was lost). */
uint32_t aret_p__acmdln(uint32_t esp) {
    (void)esp;
    static char cmd[8192];
    static char *ptr;
    static int built = 0;
    if (!built) { built = 1; aret_build_cmdline(cmd, sizeof cmd); ptr = cmd; }
    return (uint32_t)(uintptr_t)&ptr;
}
uint32_t aret_p__wcmdln(uint32_t esp) {
    (void)esp;
    static uint16_t wcmd[8192];
    static uint16_t *ptr;
    static int built = 0;
    if (!built) {
        built = 1;
        char tmp[8192];
        aret_build_cmdline(tmp, sizeof tmp);
        size_t i = 0;
        for (; tmp[i] && i < sizeof(wcmd) / sizeof(wcmd[0]) - 1; i++) wcmd[i] = (unsigned char)tmp[i];
        wcmd[i] = 0;
        ptr = wcmd;
    }
    return (uint32_t)(uintptr_t)&ptr;
}
/* __lconv_init: CRT locale-conv table init. Under our HLE the C locale is fixed
 * ("C"), so there is nothing to initialise — a no-op (return 0) is correct. */
uint32_t aret_lconv_init(uint32_t esp) { (void)esp; return 0; }

/* msvcrt internal globals exposed as pointer-returning functions. Names match
 * sanitize_import() (leading underscores stripped), so the generator's call to
 * e.g. `aret_errno` for import `_errno` binds here (these strong defs override
 * the weak unimplemented stubs). */
static int aret_fmode = 0, aret_commode = 0;
uint32_t aret_p__fmode(uint32_t esp) { (void)esp; return (uint32_t)(uintptr_t)&aret_fmode; }
uint32_t aret_p__commode(uint32_t esp) { (void)esp; return (uint32_t)(uintptr_t)&aret_commode; }
/* __p___initenv / __p__environ: msvcrt exposes the initial/current environment
 * as `char**`; these return its address (`char***`). Point at the real environ
 * so getenv/environ walks the process's actual variables. Returning 0 (the weak
 * stub) makes the CRT null-deref the env and bail before main runs. */
uint32_t aret_p___initenv(uint32_t esp) {
    (void)esp; extern char **environ; static char **initenv; initenv = environ;
    return (uint32_t)(uintptr_t)&initenv;
}
uint32_t aret_p__environ(uint32_t esp) {
    (void)esp; extern char **environ; return (uint32_t)(uintptr_t)&environ;
}
/* aret_errno / aret_onexit live in aret_crt.c (the canonical sanitized names). */
uint32_t aret_set_app_type(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_setusermatherr(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_amsg_exit(uint32_t esp) { _exit((int)arg(esp, 0)); return 0; }
uint32_t aret_cexit(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_lock(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_unlock(uint32_t esp) { (void)esp; return 0; }
/* signal(sig, handler) — msvcrt/C89 semantics: record the new disposition and
 * return the PREVIOUS one (SIG_DFL == 0 initially). We do not *deliver* signals
 * (the shared-stack model has no async delivery — unchanged from the old stub),
 * but faithfully returning the prior handler is what the mingw/gnulib
 * signal-blocking bookkeeping requires: it installs a `_blocked_handler` on
 * block and, on unblock, calls signal() again and asserts the return equals the
 * handler it last set. The old stub always returned 0, breaking that invariant
 * (GNU m4 aborted in `_sigprocmask`). SIG_ERR (-1) for an out-of-range signal. */
#define ARET_NSIG 64
static uint32_t aret_sig_handlers[ARET_NSIG]; /* 0 = SIG_DFL */
uint32_t aret_signal(uint32_t esp) {
    uint32_t sig = arg(esp, 0);
    uint32_t handler = arg(esp, 1);
    if (sig >= ARET_NSIG) return 0xFFFFFFFFu; /* SIG_ERR */
    uint32_t prev = aret_sig_handlers[sig];
    aret_sig_handlers[sig] = handler;
    return prev;
}
/* atexit: the registered callbacks are transpiled sub_<va> using the machine-
 * stack ABI, so dispatch them through aret_call (like the qsort trampoline) from
 * a single host atexit handler. LIFO order, as C requires. */
#define ARET_ATEXIT_MAX 64
static uint32_t aret_atexit_fns[ARET_ATEXIT_MAX];
static int aret_atexit_n;
static void aret_run_atexit(void) {
    static uint8_t scratch[64 * 1024];
    uint32_t *f = (uint32_t *)(void *)(scratch + sizeof(scratch) - 64);
    for (int i = aret_atexit_n - 1; i >= 0; i--)
        aret_call(aret_atexit_fns[i], (uint32_t)(uintptr_t)f, 0, 0, 0, 0, 0, 0, 0);
}
/* Shared by aret_atexit and aret_onexit (mingw's static `atexit` registers its
 * callback through the msvcrt `_onexit` import, so that path must register too). */
void aret_register_atexit(uint32_t va) {
    if (aret_atexit_n == 0) atexit(aret_run_atexit);
    if (aret_atexit_n < ARET_ATEXIT_MAX) aret_atexit_fns[aret_atexit_n++] = va;
}
uint32_t aret_atexit(uint32_t esp) { aret_register_atexit(arg(esp, 0)); return 0; }
uint32_t aret_setlocale(uint32_t esp) { (void)esp; return (uint32_t)(uintptr_t)"C"; }
uint32_t aret_abort(uint32_t esp) { (void)esp; abort(); return 0; }
/* _assert(expr, file, line) — msvcrt assertion-failure reporter. MEASURED vs Wine:
 * writes "Assertion failed: <expr>, file <file>, line <line>\n" to stderr, then
 * aborts. The weak stub would return 0, letting the program run on past a violated
 * invariant (a silent wrong result) — this is the sound fix: report and terminate.
 * (_wassert is the wide-arg twin.) */
uint32_t aret_assert(uint32_t esp) {
    const char *expr = (const char *)(uintptr_t)arg(esp, 0);
    const char *file = (const char *)(uintptr_t)arg(esp, 1);
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n",
            expr ? expr : "", file ? file : "", (unsigned)arg(esp, 2));
    abort();
    return 0;
}
uint32_t aret_wassert(uint32_t esp) {
    char e[512], f[1024];
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 0), e, sizeof e);
    aret_w2n((const uint16_t *)(uintptr_t)arg(esp, 1), f, sizeof f);
    fprintf(stderr, "Assertion failed: %s, file %s, line %u\n", e, f, (unsigned)arg(esp, 2));
    abort();
    return 0;
}
uint32_t aret_exit(uint32_t esp) { exit((int)arg(esp, 0)); return 0; }
uint32_t aret__exit(uint32_t esp) { _exit((int)arg(esp, 0)); return 0; }
uint32_t aret_strerror(uint32_t esp) { return (uint32_t)(uintptr_t)strerror((int)arg(esp, 0)); }
uint32_t aret_atoi(uint32_t esp) { return (uint32_t)atoi((const char *)(uintptr_t)arg(esp, 0)); }
uint32_t aret_strchr(uint32_t esp) {
    return (uint32_t)(uintptr_t)strchr((const char *)(uintptr_t)arg(esp, 0), (int)arg(esp, 1));
}
uint32_t aret_strncmp(uint32_t esp) {
    return (uint32_t)(int32_t)strncmp((const char *)(uintptr_t)arg(esp, 0),
                                      (const char *)(uintptr_t)arg(esp, 1), arg(esp, 2));
}

/* __getmainargs(int* argc, char*** argv, char*** env, int wild, _startupinfo*)
 * Hand the program its REAL argc/argv/environ (published by aret_main). The
 * binary is built -m32, so those pointers are already 32-bit and the guest can
 * dereference them directly. A fixed fake argv (the old behaviour) starved
 * argv-routing programs like BusyBox of their command line. */
uint32_t aret_getmainargs(uint32_t esp) {
    extern int aret_real_argc;
    extern char **aret_real_argv;
    extern char **environ;
    uint32_t pargc = arg(esp, 0), pargv = arg(esp, 1), penv = arg(esp, 2);
    if (pargc) *(int32_t *)(uintptr_t)pargc = aret_real_argc;
    if (pargv) *(uint32_t *)(uintptr_t)pargv = (uint32_t)(uintptr_t)aret_real_argv;
    if (penv) *(uint32_t *)(uintptr_t)penv = (uint32_t)(uintptr_t)environ;
    return 0;
}

/* __wgetmainargs(int* argc, wchar_t*** argv, wchar_t*** env, int wild,
 * _startupinfo*): the WIDE counterpart used by `wmainCRTStartup` (a stripped
 * MSVC console app whose entry is wmain fetches its args through this, ignoring
 * any argv the loader passed). Build UTF-16 copies of the real args once (Windows
 * wchar_t is 16-bit; args are ASCII in practice, so a byte→u16 widening is
 * exact) and hand back a 32-bit-addressable argv/env. Without this the out-params
 * stay uninitialised and the shell dereferences a garbage argv → SIGABRT. */
uint32_t aret_wgetmainargs(uint32_t esp) {
    extern int aret_real_argc;
    extern char **aret_real_argv;
    static uint16_t wbuf[1u << 16];
    static uint32_t wargv[1024];
    static uint32_t wenv[1] = {0}; /* empty wide environ (NULL-terminated) */
    static int built = 0;
    int n = aret_real_argc;
    if (n > 1024) n = 1024;
    if (!built) {
        uint32_t wo = 0;
        for (int i = 0; i < n; i++) {
            wargv[i] = (uint32_t)(uintptr_t)&wbuf[wo];
            for (const unsigned char *p = (const unsigned char *)aret_real_argv[i];
                 *p && wo < (1u << 16) - 1; p++)
                wbuf[wo++] = *p;
            wbuf[wo++] = 0;
        }
        built = 1;
    }
    uint32_t pargc = arg(esp, 0), pargv = arg(esp, 1), penv = arg(esp, 2);
    if (pargc) *(int32_t *)(uintptr_t)pargc = n;
    if (pargv) *(uint32_t *)(uintptr_t)pargv = (uint32_t)(uintptr_t)wargv;
    if (penv) *(uint32_t *)(uintptr_t)penv = (uint32_t)(uintptr_t)wenv;
    return 0;
}

/* _initterm(first, last) walks a table of CRT/C++ initializer pointers in
 * [first, last) and calls each non-null one. These initializers are part of the
 * translated program (e.g. mingw's `_pre_c_init`/`_pre_cpp_init`, which call
 * `__setargv`/`__getmainargs` to populate argc/argv) — NOT MSVC-internal glue —
 * so they MUST run: a blanket no-op silently skips program initialization
 * (argv left empty → a main() that reads argv[0] crashes), i.e. wrong behaviour
 * presented as correct. We dispatch each entry through aret_call on the live
 * machine stack, exactly as the real _initterm would. An entry that resolves to
 * unrecovered/unmodelled code aborts loudly inside aret_call (sound), never a
 * silent skip. The table holds 32-bit guest function VAs (the binary is -m32). */
uint32_t aret_initterm(uint32_t esp) {
    uint32_t first = arg(esp, 0), last = arg(esp, 1);
    for (uint32_t p = first; p + 4 <= last; p += 4) {
        uint32_t fn = *(const uint32_t *)(uintptr_t)p;
        if (fn) aret_call(fn, esp, 0, 0, 0, 0, 0, 0, 0);
    }
    return 0;
}
/* _initterm_e: like _initterm but each initializer returns an int; a non-zero
 * return is an init failure that stops the walk and is propagated to the caller. */
uint32_t aret_initterm_e(uint32_t esp) {
    uint32_t first = arg(esp, 0), last = arg(esp, 1);
    for (uint32_t p = first; p + 4 <= last; p += 4) {
        uint32_t fn = *(const uint32_t *)(uintptr_t)p;
        if (fn) {
            uint32_t r = (uint32_t)aret_call(fn, esp, 0, 0, 0, 0, 0, 0, 0);
            if (r) return r;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Synthetic TEB / PEB (Windows process environment, x86)             */
/* ------------------------------------------------------------------ */

static uint8_t aret_teb[0x1000];
static uint8_t aret_peb[0x1000];
static uint8_t aret_procparams[0x400]; /* RTL_USER_PROCESS_PARAMETERS + slack */
static int aret_teb_ready = 0;

/* The real shared-machine-stack bounds, published by the emitted entry
 * (aret_main.c) before it runs the program. StackBase = highest address (top),
 * StackLimit = lowest. Zero until set (a lone-shim/test context with no entry) →
 * aret_teb_init falls back to a permissive placeholder range. */
static uint32_t aret_stack_base_va = 0, aret_stack_limit_va = 0;
void __aret_set_stack_bounds(uint32_t base, uint32_t limit) {
    aret_stack_base_va = base;
    aret_stack_limit_va = limit;
}
static void aret_teb_init(void) {
    if (aret_teb_ready) return;
    aret_teb_ready = 1;
    uint32_t teb = (uint32_t)(uintptr_t)aret_teb;
    uint32_t peb = (uint32_t)(uintptr_t)aret_peb;
    uint32_t *t = (uint32_t *)aret_teb;
    /* NT_TIB / TEB (x86 offsets) */
    t[0x00 / 4] = 0xFFFFFFFFu;       /* ExceptionList: end of SEH chain */
    /* StackBase (fs:[4]) / StackLimit (fs:[8]): the REAL machine-stack bounds when the
     * entry published them, so a CRT that reads fs:[4] and dereferences [StackBase-N]
     * (MSVC stack-cookie / range setup) hits real memory. A permissive placeholder
     * otherwise (no entry, e.g. a unit test) — never dereferenced there. */
    t[0x04 / 4] = aret_stack_base_va  ? aret_stack_base_va  : 0x7FFF0000u; /* StackBase  */
    t[0x08 / 4] = aret_stack_limit_va ? aret_stack_limit_va : 0x00010000u; /* StackLimit */
    t[0x18 / 4] = teb;               /* Self (linear TEB address)      */
    t[0x20 / 4] = (uint32_t)getpid();/* ClientId.UniqueProcess         */
    t[0x24 / 4] = (uint32_t)getpid();/* ClientId.UniqueThread          */
    t[0x2C / 4] = 0;                 /* ThreadLocalStoragePointer      */
    t[0x30 / 4] = peb;               /* ProcessEnvironmentBlock        */
    t[0x34 / 4] = 0;                 /* LastErrorValue                 */
    uint32_t *p = (uint32_t *)aret_peb;
    aret_peb[0x02] = 0;              /* BeingDebugged = FALSE          */
    p[0x08 / 4] = 0x00400000u;       /* ImageBaseAddress (typical)     */
    /* ProcessParameters (RTL_USER_PROCESS_PARAMETERS): the CRT reads its Flags and
     * standard-handle/path fields. A zeroed block is a valid "normalized, no
     * inherited handles" set and keeps those reads from dereferencing NULL. */
    uint32_t pp = (uint32_t)(uintptr_t)aret_procparams;
    p[0x10 / 4] = pp;                /* PEB.ProcessParameters          */
    uint32_t *q = (uint32_t *)aret_procparams;
    q[0x00 / 4] = sizeof(aret_procparams); /* MaximumLength            */
    q[0x04 / 4] = sizeof(aret_procparams); /* Length                   */
    q[0x08 / 4] = 1;                 /* Flags = RTL_USER_PROC_PARAMS_NORMALIZED */
}

uint32_t __aret_fs(void) {
    aret_teb_init();
    return (uint32_t)(uintptr_t)aret_teb;
}

uint32_t __aret_gs(void) {
    aret_teb_init();
    return 0; /* gs is unused in 32-bit Windows user mode */
}

/* Structured Exception Handling — software-raised dispatch (first EH brick).
 *
 * The SEH handler chain and its EXCEPTION_REGISTRATION frames already live on the
 * machine stack, linked through the synthetic TEB's ExceptionList (`fs:[0]`): the
 * lifted prologue `push handler; push scopetable; push -1; mov fs:[0],esp` writes
 * them, and reads model `fs:[ea]` as a real load from the TEB (see ir/lift.rs). All
 * that was missing was the DISPATCH. `RaiseException` walks that chain and calls
 * each handler cdecl — `handler(ExceptionRecord*, EstablisherFrame, Context*,
 * DispatcherContext)` — through `aret_call` (the handler is a transpiled function,
 * e.g. `__except_handler3` or a hand-written one). A handler that catches transfers
 * control non-locally (a `longjmp`, or `__except_handler3`'s scope-table jump) and
 * never returns here; one that declines returns ExceptionContinueSearch (1) and we
 * try the next frame. Chain exhausted with no catch ⇒ loud abort (the exception was
 * real — never silently continue past it, which the weak import stub used to do).
 *
 * Hardware faults (a real SIGSEGV/#DE) are NOT yet routed here — only the software
 * `RaiseException`/`_CxxThrowException` path. WASM has no SEH: there `RaiseException`
 * stays a sound abort. */
/* C++ exception code (`msc`) and the two-phase dispatch over an already-built C++
 * EXCEPTION_RECORD (defined with the rest of the C++ EH machinery below). A C++ throw — even
 * from a statically-linked CRT — funnels through the imported kernel32 `RaiseException` with
 * this code, so intercepting it here catches both the imported-`_CxxThrowException` and the
 * static-CRT cases with one path. */
#define ARET_CXX_EH_CODE 0xE06D7363u
static void aret_cxx_dispatch(uint32_t esp, uint32_t *rec);   /* fwd */
static const char *aret_cxx_thrown_name(uint32_t pthrow);     /* fwd */
uint32_t aret_RaiseException(uint32_t esp) {
    aret_teb_init();
    uint32_t code = arg(esp, 0), flags = arg(esp, 1);
    uint32_t nargs = arg(esp, 2), argp = arg(esp, 3);

    /* EXCEPTION_RECORD (x86 layout), on the host stack — a valid pointer the lifted
     * handler dereferences directly (shared-stack mode: memory is host memory). */
    uint32_t rec[20];
    memset(rec, 0, sizeof(rec));
    rec[0] = code;                       /* ExceptionCode            */
    rec[1] = flags;                      /* ExceptionFlags           */
    rec[4] = nargs > 15 ? 15 : nargs;    /* NumberParameters         */
    if (argp) {
        const uint32_t *ap = (const uint32_t *)(uintptr_t)argp;
        for (uint32_t i = 0; i < rec[4]; i++) rec[5 + i] = ap[i];
    }
    /* A C++ throw: params are {EH-magic, pObject, pThrowInfo} (now in rec[5..7]). Route to the
     * C++ two-phase dispatch (type match, unwind destructors, catch transfer) — the same path
     * as an imported _CxxThrowException, so a statically-linked CRT (throw funnels through this
     * imported RaiseException) is handled identically. Returns only if unhandled. */
    if (code == ARET_CXX_EH_CODE) {
        aret_cxx_dispatch(esp, rec);
        fprintf(stderr, "aret: unhandled C++ exception (type %s, ThrowInfo 0x%08x)\n",
                aret_cxx_thrown_name(rec[7]), (unsigned)rec[7]);
        abort();
    }
    uint32_t ctx[200];                   /* a zeroed CONTEXT (defined, benign)  */
    memset(ctx, 0, sizeof(ctx));

    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];             /* ExceptionList head       */
    while (frame != 0 && frame != 0xFFFFFFFFu) {
        uint32_t *f = (uint32_t *)(uintptr_t)frame;
        uint32_t handler = f[1];
        /* Lay a cdecl frame for the handler in the free machine stack below `esp`
         * ([esp+4]=rec, [esp+8]=frame, [esp+12]=ctx, [esp+16]=dispatch); the handler
         * runs on the real machine stack below it. */
        uint32_t hesp = esp - 0x80;
        uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec;
        cf[2] = frame;
        cf[3] = (uint32_t)(uintptr_t)ctx;
        cf[4] = frame;                   /* DispatcherContext (dummy)           */
        uint32_t disp = (uint32_t)aret_call(handler, hesp, 0, 0, 0, 0, 0, 0, 0);
        if (disp == 0) return 0;         /* ExceptionContinueExecution          */
        frame = f[0];                    /* ExceptionContinueSearch: next frame */
    }
    fprintf(stderr, "aret: unhandled exception %#x\n", (unsigned)code);
    abort();
    return 0;
}

/* Structured Exception Handling — local unwind (`RtlUnwind`).
 *
 * The primitive `__except_handler3` (via `__global_unwind2`) uses to run the cleanup
 * (`__finally`) handlers of every frame between the raise point and the frame that
 * catches. On i386 `RtlUnwind` IGNORES its TargetIp argument (that is an x64 concept):
 * it walks `fs:[0]` from the head up to — but not including — the TargetFrame, calls
 * each intervening handler cdecl with the EH_UNWINDING (0x2) flag set in the exception
 * record, pops each frame off the chain, and then RETURNS NORMALLY (leaving `fs:[0]`
 * at the TargetFrame). The non-local transfer to the __except block is performed by
 * the caller afterwards, not here — so a plain-returning shim is the faithful model
 * (verified bit-identical to Wine's ntdll RtlUnwind by winecorpus/seh_unwind.c).
 *
 * Signature (stdcall @16): RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue).
 * A NULL TargetFrame means an exit unwind (unwind the whole chain, EH_EXIT_UNWIND). A
 * NULL ExceptionRecord means synthesize a STATUS_UNWIND record. WASM: no SEH — the
 * import stays a sound abort there. */
#define ARET_EH_UNWINDING   0x02u
#define ARET_EH_EXIT_UNWIND 0x04u
uint32_t aret_RtlUnwind(uint32_t esp) {
    aret_teb_init();
    uint32_t target = arg(esp, 0);       /* TargetFrame (endframe)  */
    /* arg(esp,1) = TargetIp — unused on i386 (the caller does the transfer). */
    uint32_t recp   = arg(esp, 2);       /* ExceptionRecord (optional) */
    uint32_t retval = arg(esp, 3);       /* ReturnValue (-> eax)       */
    int exit_unwind = (target == 0 || target == 0xFFFFFFFFu);

    /* The exception record the intervening handlers see: the caller's if given (Wine
     * ORs the unwinding flag into it in place), else a synthesized STATUS_UNWIND. */
    uint32_t local_rec[20];
    uint32_t *rec;
    if (recp) {
        rec = (uint32_t *)(uintptr_t)recp;
    } else {
        memset(local_rec, 0, sizeof(local_rec));
        local_rec[0] = 0xC0000027u;      /* STATUS_UNWIND ExceptionCode */
        rec = local_rec;
    }
    rec[1] |= ARET_EH_UNWINDING | (exit_unwind ? ARET_EH_EXIT_UNWIND : 0u);

    uint32_t ctx[200];                   /* a zeroed CONTEXT (defined, benign) */
    memset(ctx, 0, sizeof(ctx));

    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];             /* ExceptionList head */
    /* Bound the walk: a correct chain reaches `target` (or the ~0 sentinel for an exit
     * unwind) well within this many frames; a cyclic/corrupt chain must abort loudly,
     * never spin or silently continue. */
    for (int guard = 0; guard < 100000; guard++) {
        if (frame == 0xFFFFFFFFu || frame == 0) {
            /* Reached the end of the chain. Fine for an exit unwind; for a targeted
             * unwind it means the target frame was never found — a corrupt SEH state
             * (STATUS_INVALID_UNWIND_TARGET on Windows). Abort loudly, never guess. */
            if (exit_unwind) { teb[0] = 0xFFFFFFFFu; return retval; }
            fprintf(stderr, "aret: RtlUnwind target frame %#x not on the SEH chain\n",
                    (unsigned)target);
            abort();
        }
        if (!exit_unwind && frame == target) break;   /* stop at (not incl.) target */

        uint32_t *f = (uint32_t *)(uintptr_t)frame;
        uint32_t handler = f[1];
        uint32_t next = f[0];
        /* Call the handler cdecl on the free machine stack below `esp`
         * ([esp+4]=rec, [esp+8]=frame, [esp+12]=ctx, [esp+16]=dispatch), exactly as
         * the dispatcher does; during unwind the disposition is advisory (the handler
         * ran its __finally side effects), so we pop and continue. */
        uint32_t hesp = esp - 0x80;
        uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec;
        cf[2] = frame;
        cf[3] = (uint32_t)(uintptr_t)ctx;
        cf[4] = frame;                   /* DispatcherContext (dummy) */
        (void)aret_call(handler, hesp, 0, 0, 0, 0, 0, 0, 0);

        teb[0] = next;                   /* pop this frame off the chain */
        frame = next;
    }
    /* fs:[0] now == target (the last pop left it there). Return the requested eax. */
    return retval;
}

/* Structured Exception Handling — the MSVC/clang scope-table handler `_except_handler3`.
 *
 * Every function with __try/__except/__finally installs one SEH frame whose handler is
 * `_except_handler3`; the frame is the 4-word registration {prev, handler, scopetable,
 * trylevel} on the machine stack, with the establisher's ebp just above it
 * (ebp = &registration + 16). The SEH dispatch (RaiseException / a hardware fault) walks
 * fs:[0] and calls this handler. It reads the frame's SCOPE TABLE — an array of
 * {EnclosingLevel, FilterFunc, HandlerFunc} indexed by trylevel — to find a matching
 * __except: from the current trylevel it walks EnclosingLevel-wards, runs each __except
 * FILTER (FilterFunc != 0) with the establisher's ebp, and on EXECUTE_HANDLER (1) it
 * unwinds (outer frames + this frame's __finally blocks) then transfers to the __except
 * block — a longjmp to the setjmp the lifter injects at the SEH-establish (keyed by the
 * registration address); the establisher then runs the recovered handler funclet
 * (`aret_seh_run`) and returns its value. Filter CONTINUE_SEARCH (0) walks outward;
 * CONTINUE_EXECUTION (-1) resumes at the fault. No __except matches here -> return
 * ExceptionContinueSearch (1) so the dispatcher tries the next frame.
 *
 * The recovered filter/handler/__finally blocks are separate lifted functions (the scope
 * table takes their address); they read the parent's locals via the threaded ebp
 * register-param, which aret_call conveys as its last argument. */

/* State stashed by an EH handler (SEH __except OR C++ catch) right before it longjmps to
 * the establisher's injected setjmp — a C local would be indeterminate across longjmp.
 * aret_seh_run (called from the establisher after the longjmp) reads these to run the
 * matched handler/catch funclet. Single-slot: set immediately before the longjmp, read
 * immediately after; no intervening setjmp. */
uint32_t g_seh_frame = 0;       /* the establisher frame (for the setjmp key symmetry) */
uint32_t g_seh_handler_va = 0;  /* the __except / catch funclet to run */
uint32_t g_seh_ebp = 0;         /* the establisher ebp to run the funclet with */
int      g_seh_is_cxx = 0;      /* 0 = SEH __except (funclet return = fn return); 1 = C++ catch
                                 * (funclet returns a continuation VA to resume in the establisher) */

/* Call a recovered __try funclet (filter / __except body / __finally) with the
 * establisher's ebp threaded in aret_call's ebp slot; return its eax. */
static uint32_t aret_seh_funclet(uint32_t va, uint32_t ebp) {
    return (uint32_t)aret_call(va, ebp - 0x400 /*free stack below the frame*/, 0, 0, 0, ebp, 0, 0, 0);
}
/* Run the __finally blocks for the levels in (to, from]: from the current trylevel `from`
 * down to — but not including — `to`, following EnclosingLevel. A scope entry is a
 * __finally when FilterFunc==0 (HandlerFunc is then the cleanup block). */
static void aret_seh_local_unwind(uint32_t scopetable, int from, int to, uint32_t ebp) {
    for (int lvl = from; lvl != -1 && lvl != to; ) {
        const uint32_t *e = (const uint32_t *)(uintptr_t)(scopetable + (uint32_t)lvl * 12u);
        if (e[1] == 0 && e[2] != 0) aret_seh_funclet(e[2], ebp);   /* __finally cleanup */
        lvl = (int)e[0];
    }
}
/* Global unwind: pop fs:[0] from its head up to (not incl.) `target`, calling each
 * intervening handler with EH_UNWINDING so its __finally blocks run (same walk as
 * RtlUnwind, inlined so this handler is self-contained). */
static void aret_seh_global_unwind(uint32_t esp, uint32_t target) {
    uint32_t rec[20]; memset(rec, 0, sizeof rec);
    rec[0] = 0xC0000027u; rec[1] = ARET_EH_UNWINDING;          /* STATUS_UNWIND */
    uint32_t ctx[200]; memset(ctx, 0, sizeof ctx);
    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];
    for (int g = 0; g < 100000 && frame && frame != 0xFFFFFFFFu && frame != target; g++) {
        uint32_t *f = (uint32_t *)(uintptr_t)frame; uint32_t next = f[0];
        uint32_t hesp = esp - 0x80; uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec; cf[2] = frame; cf[3] = (uint32_t)(uintptr_t)ctx; cf[4] = frame;
        (void)aret_call(f[1], hesp, 0, 0, 0, 0, 0, 0, 0);
        teb[0] = next; frame = next;
    }
}
uint32_t aret_except_handler3(uint32_t esp) {
    uint32_t recp = arg(esp, 0), framep = arg(esp, 1);
    uint32_t *rec = (uint32_t *)(uintptr_t)recp;
    uint32_t *frame = (uint32_t *)(uintptr_t)framep;          /* {prev, handler, scopetable, trylevel} */
    uint32_t flags = rec ? rec[1] : 0;
    uint32_t scopetable = frame[2];
    uint32_t ebp = framep + 16;                          /* funclet ebp = EstablisherFrame + 16 */
    if (flags & (ARET_EH_UNWINDING | ARET_EH_EXIT_UNWIND)) {
        aret_seh_local_unwind(scopetable, (int)frame[3], -1, ebp);   /* run all __finally */
        return 1;                                                     /* ExceptionContinueSearch */
    }
    /* GetExceptionInformation: an __except filter reads a PEXCEPTION_POINTERS from
     * [establisher_ebp - 0x14] = [EstablisherFrame - 4]; _except_handler3 must publish it
     * there before calling filters (else the filter double-derefs an unpopulated slot and
     * faults). The pointer is a 2-word EXCEPTION_POINTERS {ExceptionRecord, ContextRecord}
     * — everything is 32-bit here (the recompiled program is -m32). */
    static uint32_t xp[2];
    xp[0] = recp; xp[1] = arg(esp, 2) /* ContextRecord */;
    *(uint32_t *)(uintptr_t)(framep - 4) = (uint32_t)(uintptr_t)xp;
    for (int lvl = (int)frame[3]; lvl != -1; ) {
        const uint32_t *e = (const uint32_t *)(uintptr_t)(scopetable + (uint32_t)lvl * 12u);
        int enclosing = (int)e[0];
        if (e[1] != 0) {                                             /* __except (has a filter) */
            int32_t d = (int32_t)aret_seh_funclet(e[1], ebp);
            if (d < 0) return 0;                                     /* CONTINUE_EXECUTION */
            if (d > 0) {                                             /* EXECUTE_HANDLER */
                aret_seh_global_unwind(esp, framep);
                aret_seh_local_unwind(scopetable, (int)frame[3], lvl, ebp); /* this frame's __finally */
                frame[3] = (uint32_t)enclosing;                      /* trylevel := enclosing */
                g_seh_frame = framep; g_seh_handler_va = e[2]; g_seh_ebp = ebp; g_seh_is_cxx = 0;  /* __except handler funclet */
                aret_longjmp_do(framep, lvl + 1);                    /* -> establisher setjmp */
                /* not reached */
            }
        }
        lvl = enclosing;
    }
    return 1;                                                        /* no match -> next frame */
}
/* Runs from the setjmp the lifter injects at the SEH-establish when a longjmp from
 * _except_handler3 lands (an __except caught): `level` is the matched scope index — run
 * its handler funclet with the establisher's ebp and return its value as the establisher
 * function's return. */
uint64_t aret_seh_run(uint32_t framep, uint32_t level) {
    (void)framep; (void)level;   /* the funclet VA + establisher ebp were stashed pre-longjmp */
    if (g_seh_is_cxx) {
        /* C++ catch: run the catch funclet (it copies the caught object, runs the catch
         * body, and returns — in eax — the CONTINUATION address in the establisher, i.e.
         * where execution resumes after the try/catch). Then resume there. The funclet and
         * the continuation both take the establisher frame base in ebp (Wine calls them with
         * ebp = &frame->ebp = EstablisherFrame + 0xc). A nested throw inside the continuation
         * longjmps back to the establisher's setjmp (not here), so this call returns only when
         * the continuation runs to the function's natural end. */
        uint32_t cont = (uint32_t)aret_call(g_seh_handler_va, g_seh_ebp - 0x400, 0, 0, 0, g_seh_ebp, 0, 0, 0);
        return aret_call(cont, g_seh_ebp - 0x400, 0, 0, 0, g_seh_ebp, 0, 0, 0);
    }
    return aret_call(g_seh_handler_va, g_seh_ebp - 0x400, 0, 0, 0, g_seh_ebp, 0, 0, 0);
}

/* ---- C++ exceptions (MSVC _CxxThrowException / __CxxFrameHandler) ------------------
 * A C++ `throw` calls _CxxThrowException(pObject, pThrowInfo), which raises a software
 * exception (code 0xE06D7363 'msc', params {EH-magic, pObject, pThrowInfo}) down the
 * fs:[0] chain — the same dispatch as RaiseException. Each frame with try/catch installs
 * a SEH frame whose handler is __CxxFrameHandler3 (via a `mov eax,&FuncInfo; jmp` thunk);
 * that handler reads the function's FuncInfo (state map + TryBlockMap) and the throw's
 * ThrowInfo (the list of types the object can be caught as), finds a try block covering
 * the current state whose catch type matches, copies the object into the catch parameter,
 * and transfers to the catch funclet — reusing brick C's non-local transfer (the setjmp
 * injected at the SEH-establish + aret_seh_run). C++ exception structures are all in guest
 * memory (32-bit, the program is -m32). ARET_CXX_EH_CODE is defined above aret_RaiseException. */
uint32_t aret_CxxFrameHandler3(uint32_t esp);   /* fwd */
/* Call one frame's SEH handler with the call frame at hesp ({_, rec, frame, ctx, frame}).
 * The frame's handler is a `mov eax,&FuncInfo; jmp __CxxFrameHandler[3]` thunk — guest code
 * (the image's .text is mapped as data, so the 0xB8 lead byte is readable), but *not* a
 * recovered function we can aret_call. Detect that lead byte and route straight to our
 * __CxxFrameHandler3 (it recovers FuncInfo from the thunk's imm). A non-C++ frame's handler is
 * a recovered function (SEH `_except_handler3`) reached via aret_call. The shim reads its cdecl
 * args at [esp+0], so pass hesp+4 (skip the return-addr slot), as aret_call's dispatchers do;
 * aret_call adds that +4 itself. */
static uint32_t aret_cxx_call_handler(uint32_t handler, uint32_t hesp) {
    return (*(const uint8_t *)(uintptr_t)handler == 0xB8u)
        ? aret_CxxFrameHandler3(hesp + 4)
        : (uint32_t)aret_call(handler, hesp, 0, 0, 0, 0, 0, 0, 0);
}
/* Phase-2 global unwind for a C++ throw: pop fs:[0] from its head up to (not incl.) `target`
 * (the catching frame), calling each intervening frame's handler with EH_UNWINDING so its C++
 * destructors run (each C++ handler's unwind pass calls aret_cxx_local_unwind). Innermost
 * first, matching the machine-stack order. */
static void aret_cxx_global_unwind(uint32_t esp, uint32_t target) {
    uint32_t rec[20]; memset(rec, 0, sizeof rec);
    rec[0] = 0xC0000027u; rec[1] = ARET_EH_UNWINDING;              /* STATUS_UNWIND */
    uint32_t ctx[200]; memset(ctx, 0, sizeof ctx);
    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];
    for (int g = 0; g < 100000 && frame && frame != 0xFFFFFFFFu && frame != target; g++) {
        uint32_t *f = (uint32_t *)(uintptr_t)frame; uint32_t next = f[0];
        uint32_t hesp = esp - 0x80; uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec; cf[2] = frame; cf[3] = (uint32_t)(uintptr_t)ctx; cf[4] = frame;
        aret_cxx_call_handler(f[1], hesp);
        teb[0] = next; frame = next;
    }
}
/* Phase-1 search of the fs:[0] chain for a C++ throw whose EXCEPTION_RECORD is `rec`
 * (rec[0]=0xE06D7363, rec[6]=pObject, rec[7]=pThrowInfo). Calls each frame's handler (no side
 * effects); a handler that catches performs phase-2 global unwind + local unwind + the
 * non-local transfer itself (never returns). Returns 0 if a handler continued execution, 1 if
 * the chain was exhausted with no catch (the caller then aborts — unhandled). Shared by the
 * imported `_CxxThrowException` shim and the `RaiseException(0xE06D7363)` path (static CRT). */
static void aret_cxx_dispatch(uint32_t esp, uint32_t *rec) {
    uint32_t ctx[200]; memset(ctx, 0, sizeof ctx);
    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];
    while (frame != 0 && frame != 0xFFFFFFFFu) {
        uint32_t *f = (uint32_t *)(uintptr_t)frame;
        uint32_t hesp = esp - 0x80; uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec; cf[2] = frame; cf[3] = (uint32_t)(uintptr_t)ctx; cf[4] = frame;
        /* Phase-1 search. A frame that *catches* transfers control by longjmp (the 0xB8
         * __CxxFrameHandler3 path) and never returns here; any frame whose handler *returns*
         * simply did not catch, so keep walking — whatever disposition it returned. (An SEH
         * `_except_handler3/4` in the chain, e.g. the CRT top-level frame, returns a value; a
         * software C++ throw has no valid ContinueExecution, so its return must not stop the
         * search — otherwise a lifted CRT handler returning 0 would abort the search early and
         * a genuinely-unhandled throw would fall through to the noreturn-throw `int3` instead
         * of the clean "unhandled" report below.) */
        (void)aret_cxx_call_handler(f[1], hesp);
        frame = f[0];
    }
    /* chain exhausted with no catch -> the throw is unhandled */
}
/* First thrown-type name from the ThrowInfo (for the unhandled diagnostic): ThrowInfo{+0xc:
 * CatchableTypeArray} -> [count, CatchableType*...]; CatchableType{+4: TypeDescriptor};
 * TypeDescriptor{+8: decorated name}. All in guest image memory. Best-effort — "?" if the
 * chain doesn't hold up (we are aborting regardless). */
static const char *aret_cxx_thrown_name(uint32_t pthrow) {
    if (!pthrow) return "?";
    uint32_t pCTA = *(const uint32_t *)(uintptr_t)(pthrow + 0xc);
    if (!pCTA) return "?";
    if (*(const uint32_t *)(uintptr_t)pCTA == 0) return "?";
    uint32_t pCT = *(const uint32_t *)(uintptr_t)(pCTA + 4);
    if (!pCT) return "?";
    uint32_t pType = *(const uint32_t *)(uintptr_t)(pCT + 4);
    if (!pType) return "?";
    return (const char *)(uintptr_t)(pType + 8);
}
uint32_t aret_CxxThrowException(uint32_t esp) {
    aret_teb_init();
    uint32_t pobj = arg(esp, 0), pthrow = arg(esp, 1);
    uint32_t rec[20]; memset(rec, 0, sizeof rec);
    rec[0] = ARET_CXX_EH_CODE; rec[1] = 1u /*NONCONTINUABLE*/; rec[4] = 3u /*NumberParameters*/;
    rec[5] = 0x19930520u /*EH magic*/; rec[6] = pobj; rec[7] = pthrow;
    aret_cxx_dispatch(esp, rec);   /* a catch never returns (longjmp); returning here == unhandled */
    fprintf(stderr, "aret: unhandled C++ exception (type %s, ThrowInfo 0x%08x)\n",
            aret_cxx_thrown_name(pthrow), (unsigned)pthrow);
    abort();
    return 0;
}
/* Find the thrown object's CatchableType matching a catch whose TypeDescriptor is pCatchType,
 * by mangled name — the CatchableTypeArray enumerates the exact type + every base the object
 * can be caught as. Returns the matching CatchableType VA (holds the size / copy-function /
 * this-displacement needed to bind the catch parameter), or 0 if none. (catch(...) — a NULL
 * pCatchType — is handled by the caller: it matches any and binds no object.) */
static uint32_t aret_cxx_catchable_match(uint32_t pCatchType, const uint32_t *cta) {
    const char *cname = (const char *)(uintptr_t)(pCatchType + 8);
    int n = (int)cta[0];
    for (int i = 0; i < n; i++) {
        uint32_t pct = cta[1 + i];
        uint32_t ctd = ((const uint32_t *)(uintptr_t)pct)[1];        /* CatchableType.pType */
        if (!strcmp(cname, (const char *)(uintptr_t)(ctd + 8))) return pct;
    }
    return 0;
}
/* Run the C++ destructors for the states unwound from `from` down to (excl.) `to`, following
 * the UnwindMap toState chain — mirrors Wine's cxx_local_unwind. Each UnwindMapEntry
 * {toState, action} with action != 0 is a destructor funclet, called with the establisher ebp
 * (frame + 0xc). frame->state is advanced BEFORE each call so a throw inside a destructor
 * continues the unwind from the right point rather than re-running it. */
static void aret_cxx_local_unwind(const uint32_t *fi, uint32_t framep, int from, int to) {
    uint32_t pUnwind = fi[2]; int maxState = (int)fi[1];
    uint32_t *pstate = &((uint32_t *)(uintptr_t)framep)[2];         /* frame->state @ +8 */
    uint32_t ebp = framep + 0xc;
    int s = from;
    for (int g = 0; g < maxState + 2 && s != to && s >= 0; g++) {
        const uint32_t *ue = (const uint32_t *)(uintptr_t)(pUnwind + (uint32_t)s * 8u);
        int next = (int)ue[0]; uint32_t action = ue[1];
        *pstate = (uint32_t)next;                                  /* advance state before the dtor */
        if (action) aret_seh_funclet(action, ebp);
        s = next;
    }
    *pstate = (uint32_t)to;
}
uint32_t aret_CxxFrameHandler3(uint32_t esp) {
    uint32_t recp = arg(esp, 0), framep = arg(esp, 1);
    const uint32_t *rec = (const uint32_t *)(uintptr_t)recp;
    /* FuncInfo = the imm of the frame handler's `mov eax, imm32` (0xB8) thunk. */
    uint32_t thunk = ((const uint32_t *)(uintptr_t)framep)[1];
    if (*(const uint8_t *)(uintptr_t)thunk != 0xB8u) return 1;
    const uint32_t *fi = (const uint32_t *)(uintptr_t)(*(const uint32_t *)(uintptr_t)(thunk + 1));
    int state = (int)((const uint32_t *)(uintptr_t)framep)[2];       /* frame->state @ +8 */
    if (rec[1] & (ARET_EH_UNWINDING | ARET_EH_EXIT_UNWIND)) {
        aret_cxx_local_unwind(fi, framep, state, -1);               /* phase 2: run this frame's dtors */
        return 1;
    }
    if (rec[0] != ARET_CXX_EH_CODE) return 1;
    uint32_t nTry = fi[3], pTryMap = fi[4];                          /* nTryBlocks, pTryBlockMap */
    const uint32_t *ti = (const uint32_t *)(uintptr_t)rec[7];        /* ThrowInfo */
    const uint32_t *cta = (const uint32_t *)(uintptr_t)ti[3];        /* CatchableTypeArray */
    /* Wine's call_catch_block invokes the catch funclet with ebp = &frame->ebp =
     * EstablisherFrame + 0xc (the 4th dword of the cxx_exception_frame). dispCatchObj and
     * the funclet's local accesses are all relative to that base. */
    uint32_t pObject = rec[6], ebp = framep + 0xc;
    for (uint32_t t = 0; t < nTry; t++) {
        const uint32_t *tb = (const uint32_t *)(uintptr_t)(pTryMap + t * 20u); /* TryBlockMapEntry */
        if (state < (int)tb[0] || state > (int)tb[1]) continue;      /* tryLow..tryHigh */
        uint32_t nCatch = tb[3], pH = tb[4];
        for (uint32_t h = 0; h < nCatch; h++) {
            const uint32_t *ht = (const uint32_t *)(uintptr_t)(pH + h * 16u); /* HandlerType */
            uint32_t adj = ht[0], pCatchType = ht[1]; int dispObj = (int)ht[2]; uint32_t handlerVA = ht[3];
            uint32_t matchCT;
            if (pCatchType == 0) matchCT = 0;                        /* catch(...) — matches any, no object */
            else if (!(matchCT = aret_cxx_catchable_match(pCatchType, cta))) continue;
            if (dispObj && matchCT) {                                /* bind the catch parameter */
                uint32_t *slot = (uint32_t *)(uintptr_t)(ebp + (uint32_t)dispObj);
                /* CatchableType {properties, pType, PMD{mdisp,pdisp,vdisp}, sizeOrOffset, copyFn}
                 * (dwords). PMD adjusts the thrown pointer to the caught (base) subobject. */
                const uint32_t *ct = (const uint32_t *)(uintptr_t)matchCT;
                int32_t mdisp = (int32_t)ct[2], pdisp = (int32_t)ct[3];
                uint32_t sz = ct[5], copyFn = ct[6];
                if (pdisp != -1)                                     /* virtual-base adjustment */
                    aret_unmodelled("C++ catch: virtual-base object adjustment not modelled");
                uint32_t src = pObject + (uint32_t)mdisp;            /* this-adjust to the base subobject */
                if (adj & 0x08u) *slot = src;                        /* by reference: the (adjusted) pointer */
                else if (copyFn != 0)                                /* by value with a non-trivial copy ctor */
                    aret_unmodelled("C++ catch by value: copy constructor not modelled");
                else memcpy(slot, (const void *)(uintptr_t)src, sz); /* trivially copyable: copy size bytes */
            }
            /* Phase 2: global-unwind the frames between the throw and here (their destructors),
             * then unwind this frame to the try's low state (its destructors), set the state to
             * just past the try block, and transfer to the catch. */
            aret_cxx_global_unwind(esp, framep);
            aret_cxx_local_unwind(fi, framep, state, (int)tb[0]);
            ((uint32_t *)(uintptr_t)framep)[2] = tb[1] + 1;         /* frame->state := tryHigh + 1 */
            g_seh_frame = framep; g_seh_handler_va = handlerVA; g_seh_ebp = ebp; g_seh_is_cxx = 1;
            aret_longjmp_do(framep, 1);                              /* -> establisher setjmp -> run the catch funclet */
            /* not reached */
        }
    }
    return 1;   /* no catch here (phase-1 search) -> next frame; destructors run in the phase-2 pass */
}


/* Structured Exception Handling — hardware faults (native only).
 *
 * On Windows a CPU trap (access violation, integer divide-by-zero, …) is turned by
 * the kernel into an SEH dispatch: it walks fs:[0] calling each handler, exactly like
 * a software RaiseException. ARET runs the lifted program's memory accesses as real
 * host loads/stores, so such a trap arrives as a host signal (SIGSEGV/SIGFPE). This
 * routes that signal into the same fs:[0] dispatch: a program that wraps a faulting
 * access in __try/__except (or a hand-installed SEH frame) catches it and continues,
 * instead of the process dying — matching Wine.
 *
 * The dispatch runs the handler on a DEDICATED scratch stack (aret_eh_stack), not the
 * machine stack: at fault time the lifted program's machine `esp` is a value buried in
 * a host register and cannot be recovered from the signal context, but it is not
 * needed — a handler that catches restores esp from its own registration record (the
 * __except_handler3 scope jump, or the fixture's longjmp), driven by the frame data,
 * not by the dispatcher's stack. A handler that catches never returns here (it
 * longjmps / scope-jumps out; SA_NODEFER keeps the signal unblocked across that jump,
 * so later faults are caught too). Chain exhausted with no catch ⇒ the fault was real
 * and unhandled: restore the default disposition and let the instruction re-fault, so
 * the process dies with the authentic signal (loud, never silently swallowed).
 *
 * WASM has no POSIX signals and no hardware-fault SEH ⇒ this whole mechanism is
 * native-only (guarded), and a genuine fault there stays a sound trap. */
#ifndef __wasm__
#include <signal.h>
#include <ucontext.h>
static uint8_t aret_eh_stack[1 << 16];   /* scratch stack for the fault handler run */

static void aret_hw_fault(int sig, siginfo_t *si, void *uctx) {
    static volatile int depth = 0;       /* re-entrancy guard: a fault inside dispatch */
    if (++depth > 8) { signal(sig, SIG_DFL); return; }

    uint32_t *teb = (uint32_t *)(uintptr_t)__aret_fs();
    uint32_t frame = teb[0];             /* ExceptionList head */

    /* Map the host signal to the NT exception code Windows would raise. */
    uint32_t code = (sig == SIGFPE) ? 0xC0000094u   /* STATUS_INTEGER_DIVIDE_BY_ZERO */
                                    : 0xC0000005u;  /* STATUS_ACCESS_VIOLATION       */

    /* Resume-loop guard (soundness): a handler that returns ExceptionContinueExecution
     * makes us retry the faulting instruction — correct when the handler actually fixed
     * the cause (e.g. committed a guard page). But if the SAME instruction keeps faulting,
     * the "fix" is not working and we would spin forever: a *silent hang*, which is worse
     * than a loud stop (§0 — fail loud, never silent). This happens when a handler
     * mis-reports a hard fault as resumable — notably an unimplemented `_except_handler4_common`
     * (weak stub → returns 0 = ExceptionContinueExecution) sitting on the SEH chain over a
     * genuine access violation. Detect no-progress (same faulting host PC N times running)
     * and abort with the address instead of hanging. A real fix-and-resume moves the PC on,
     * so the counter resets; only a true loop trips it. */
    {
        /* Keyed on the faulting address (si_addr) — always present in siginfo, unlike the
         * host PC (REG_EIP needs _GNU_SOURCE, undefined here). A `-1` sentinel means "no
         * fault yet" so even a NULL deref (si_addr == 0) is tracked correctly; the retried
         * instruction re-faults at the same address every time, so the count climbs. */
        static uintptr_t s_last_addr = (uintptr_t)-1;
        static int s_same = 0;
        uintptr_t addr = si ? (uintptr_t)si->si_addr : (uintptr_t)-2;
        if (addr == s_last_addr) {
            if (++s_same >= 16) {
                fprintf(stderr,
                        "aret: hardware fault %#x at %p keeps re-faulting without progress "
                        "(ExceptionContinueExecution loop) — aborting instead of hanging\n",
                        (unsigned)code, si ? si->si_addr : (void *)0);
                aret_trace_dump();
                abort();
            }
        } else {
            s_last_addr = addr;
            s_same = 0;
        }
    }

    uint32_t rec[20];
    memset(rec, 0, sizeof(rec));
    rec[0] = code;                       /* ExceptionCode  */
    rec[3] = si ? (uint32_t)(uintptr_t)si->si_addr : 0; /* ExceptionAddress (approx) */
    if (sig == SIGSEGV) {
        rec[4] = 2;                      /* NumberParameters                        */
        uint32_t write = 0;              /* ExceptionInformation[0]: 0 read / 1 write */
#ifdef REG_ERR
        write = (((ucontext_t *)uctx)->uc_mcontext.gregs[REG_ERR] & 2u) ? 1u : 0u;
#else
        (void)uctx;
#endif
        rec[5] = write;
        rec[6] = si ? (uint32_t)(uintptr_t)si->si_addr : 0; /* faulting address */
    }
    uint32_t ctx[200];
    memset(ctx, 0, sizeof(ctx));

    /* Run each handler cdecl on the dedicated scratch stack (see the note above). */
    uint32_t hesp = (uint32_t)(uintptr_t)(aret_eh_stack + sizeof(aret_eh_stack) - 0x100);
    while (frame != 0 && frame != 0xFFFFFFFFu) {
        uint32_t *f = (uint32_t *)(uintptr_t)frame;
        uint32_t handler = f[1];
        uint32_t *cf = (uint32_t *)(uintptr_t)hesp;
        cf[1] = (uint32_t)(uintptr_t)rec;
        cf[2] = frame;
        cf[3] = (uint32_t)(uintptr_t)ctx;
        cf[4] = frame;
        uint32_t disp = (uint32_t)aret_call(handler, hesp, 0, 0, 0, 0, 0, 0, 0);
        /* ExceptionContinueExecution(0): retry the faulting instruction (the handler
         * fixed the cause). ExceptionContinueSearch(1): next frame. A handler that
         * catches never returns (longjmp / scope jump). */
        if (disp == 0) { depth--; return; }
        frame = f[0];
    }
    /* No handler caught it: a genuine, unhandled hardware fault. Let it re-fault with
     * the default disposition so the process dies with the real signal. */
    fprintf(stderr, "aret: unhandled hardware exception %#x at %p\n",
            (unsigned)code, si ? si->si_addr : (void *)0);
    aret_trace_dump();
    signal(sig, SIG_DFL);
    depth--;
}

__attribute__((constructor)) static void aret_hw_fault_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = aret_hw_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;   /* NODEFER: stay catchable across longjmp */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
#endif /* !__wasm__ */
