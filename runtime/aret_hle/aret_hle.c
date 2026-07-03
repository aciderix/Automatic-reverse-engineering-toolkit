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
 * them to send writes to a synthetic `_iob` stream to the right fd). */
static int iob_fd(uint32_t file);
static void stdio_write(uint32_t file, const char *buf, size_t len);

/* ------------------------------------------------------------------ */
/* kernel32.dll                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_last_error = 0;

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
    usleep((useconds_t)arg(esp, 0) * 1000u);
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
            case 'c': n = snprintf(tmp, sizeof tmp, spec, (int)a[ai++]); break;
            case 'p': n = snprintf(tmp, sizeof tmp, spec, (void *)(uintptr_t)a[ai++]); break;
            case 's': {
                const char *v = (const char *)(uintptr_t)a[ai++];
                n = snprintf(tmp, sizeof tmp, spec, v ? v : "(null)");
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

uint32_t aret_printf(uint32_t esp) {
    const char *fmt = (const char *)(uintptr_t)arg(esp, 0);
    const uint32_t *a = &((const uint32_t *)(uintptr_t)esp)[1]; /* args after fmt */
    char out[8192];
    size_t o = aret_vformat(out, sizeof out, fmt, a);
    ssize_t w = write(1, out, o);
    (void)w;
    return (uint32_t)o;
}

/* Synthetic msvcrt `_iob` (stdin/out/err FILE array). The CRT computes
 * `&_iob[idx]`; we only need the address arithmetic to recover idx -> fd. */
#define ARET_FILE_SIZE 32
static uint8_t aret_iob[3 * ARET_FILE_SIZE];

/* fd for a FILE* that is one of our _iob entries, else -1 (a real FILE*). */
static int iob_fd(uint32_t file) {
    uintptr_t base = (uintptr_t)aret_iob, f = (uintptr_t)file;
    if (f >= base && f < base + sizeof(aret_iob)) {
        return (int)((f - base) / ARET_FILE_SIZE); /* 0/1/2 */
    }
    return -1;
}

static void stdio_write(uint32_t file, const char *buf, size_t len) {
    int fd = iob_fd(file);
    if (fd >= 0) {
        ssize_t w = write(fd, buf, len);
        (void)w;
    } else if (file) {
        fwrite(buf, 1, len, (FILE *)(uintptr_t)file);
    }
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
    int fd = iob_fd(arg(esp, 0));
    return fd >= 0 ? (uint32_t)fd : (uint32_t)fileno((FILE *)(uintptr_t)arg(esp, 0));
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
    if (fd <= 2) return 0; /* never close the std streams */
    return (uint32_t)close(fd);
}
/* _lseek(fd, offset, origin) -> new offset. origin 0/1/2 == SEEK_SET/CUR/END. */
uint32_t aret_lseek(uint32_t esp) {
    off_t r = lseek((int)arg(esp, 0), (int32_t)arg(esp, 1), (int)arg(esp, 2));
    return (uint32_t)r;
}
uint32_t aret_isatty(uint32_t esp) { return (uint32_t)isatty((int)arg(esp, 0)); }
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

uint32_t aret_fopen(uint32_t esp) {
    const char *name = (const char *)(uintptr_t)arg(esp, 0);
    const char *mode = (const char *)(uintptr_t)arg(esp, 1);
    char path[1024];
    translate_path(name, path, sizeof path);
    if (path_is_write_mode(mode)) make_parents(path);
    FILE *f = fopen(path, mode ? mode : "rb");
    return (uint32_t)(uintptr_t)f;
}

uint32_t aret_fclose(uint32_t esp) {
    return (uint32_t)fclose((FILE *)(uintptr_t)arg(esp, 0));
}

uint32_t aret_fread(uint32_t esp) {
    void *ptr = (void *)(uintptr_t)arg(esp, 0);
    return (uint32_t)fread(ptr, arg(esp, 1), arg(esp, 2), (FILE *)(uintptr_t)arg(esp, 3));
}

uint32_t aret_fwrite(uint32_t esp) {
    const char *ptr = (const char *)(uintptr_t)arg(esp, 0);
    uint32_t size = arg(esp, 1), nmemb = arg(esp, 2), file = arg(esp, 3);
    if (iob_fd(file) >= 0) { stdio_write(file, ptr, (size_t)size * nmemb); return nmemb; }
    return (uint32_t)fwrite(ptr, size, nmemb, (FILE *)(uintptr_t)file);
}

uint32_t aret_fputs(uint32_t esp) {
    const char *s = (const char *)(uintptr_t)arg(esp, 0);
    uint32_t file = arg(esp, 1);
    if (iob_fd(file) >= 0) { stdio_write(file, s, strlen(s)); return 0; }
    return (uint32_t)fputs(s, (FILE *)(uintptr_t)file);
}

uint32_t aret_fgets(uint32_t esp) {
    char *buf = (char *)(uintptr_t)arg(esp, 0);
    int n = (int)arg(esp, 1);
    uint32_t file = arg(esp, 2);
    /* stdin is a synthetic `_iob` handle, not a host FILE* — reading it via host
     * fgets dereferences a bogus stream and segfaults (the sqlite3 shell reads
     * its script from stdin this way). Read the underlying fd a byte at a time,
     * matching fgets' semantics (up to n-1 chars, stop after '\n', NUL-end). */
    int fd = iob_fd(file);
    if (fd >= 0) {
        if (n <= 0) return 0;
        int i = 0;
        while (i < n - 1) {
            unsigned char c;
            if (read(fd, &c, 1) != 1) break; /* EOF or error */
            buf[i++] = (char)c;
            if (c == '\n') break;
        }
        if (i == 0) return 0;                /* nothing read → NULL (EOF) */
        buf[i] = '\0';
        return (uint32_t)(uintptr_t)buf;
    }
    char *r = fgets(buf, n, (FILE *)(uintptr_t)file);
    return r ? (uint32_t)(uintptr_t)buf : 0;
}

/* Character input + stream state, used by Lua's lexer to read a script file or
 * stdin. For a real host FILE* (e.g. an fopen'd script) forward directly; for a
 * synthetic _iob stream read the underlying fd. EOF is -1. */
uint32_t aret_getc(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    int fd = iob_fd(file);
    if (fd >= 0) { unsigned char c; return read(fd, &c, 1) == 1 ? c : 0xFFFFFFFFu; }
    return file ? (uint32_t)getc((FILE *)(uintptr_t)file) : 0xFFFFFFFFu;
}
uint32_t aret_fgetc(uint32_t esp) { return aret_getc(esp); }
uint32_t aret_ungetc(uint32_t esp) {
    uint32_t file = arg(esp, 1);
    if (iob_fd(file) >= 0) return arg(esp, 0); /* (no pushback on the raw fd path) */
    return file ? (uint32_t)ungetc((int)arg(esp, 0), (FILE *)(uintptr_t)file) : 0xFFFFFFFFu;
}
uint32_t aret_feof(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (iob_fd(file) >= 0) return 0;            /* raw fd: EOF surfaces via getc==-1 */
    return file ? (uint32_t)feof((FILE *)(uintptr_t)file) : 1;
}
uint32_t aret_ferror(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (iob_fd(file) >= 0) return 0;
    return file ? (uint32_t)ferror((FILE *)(uintptr_t)file) : 0;
}
uint32_t aret_clearerr(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (file && iob_fd(file) < 0) clearerr((FILE *)(uintptr_t)file);
    return 0;
}

uint32_t aret_fflush(uint32_t esp) {
    uint32_t file = arg(esp, 0);
    if (!file) { fflush(NULL); return 0; }      /* flush every host stream */
    if (iob_fd(file) >= 0) return 0;            /* _iob: unbuffered write(), nothing to flush */
    return (uint32_t)fflush((FILE *)(uintptr_t)file);
}

uint32_t aret_fseek(uint32_t esp) {
    return (uint32_t)fseek((FILE *)(uintptr_t)arg(esp, 0), (long)(int32_t)arg(esp, 1), (int)arg(esp, 2));
}
uint32_t aret_rewind(uint32_t esp) {
    rewind((FILE *)(uintptr_t)arg(esp, 0));
    return 0;
}

uint32_t aret_ftell(uint32_t esp) {
    return (uint32_t)ftell((FILE *)(uintptr_t)arg(esp, 0));
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
    FILE *f = fopen(path, mode ? mode : "rb");
    if (f && iob_fd(stream) < 0 && stream) fclose((FILE *)(uintptr_t)stream);
    return (uint32_t)(uintptr_t)f;
}

uint32_t aret_tmpfile(uint32_t esp) { (void)esp; return (uint32_t)(uintptr_t)tmpfile(); }

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
    for (int i = 0; i < ARET_MAP_MAX; i++) if (aret_maps[i] && (uint32_t)(uintptr_t)aret_maps[i] == h) {
        free(aret_maps[i]); aret_maps[i] = NULL; return 1;
    }
    return close((int)h) == 0 ? 1 : 0;
}
#else /* __wasm__: no file mapping; a HANDLE is always an fd. */
uint32_t aret_CloseHandle(uint32_t esp) { return close((int)arg(esp, 0)) == 0 ? 1 : 0; }
#endif

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
void aret_unmodelled(const char *insn) {
    fprintf(stderr, "ARET: reached an unmodelled instruction: %s\n", insn);
    fprintf(stderr, "ARET: aborting — translation is incomplete here; refusing to guess.\n");
    abort();
}

/* Bound-away startup glue: do nothing, return 0. */
uint32_t aret_noop(uint32_t esp) { (void)esp; return 0; }

/* The x87 fp return channel (st(0) across calls). One shared definition; the
 * __x87_retstore/__x87_retload helpers (emit::FLOAT_HELPERS) read/write it. */
long double __aret_x87_ret;

/* The runtime x87 stack model (fallback for depth-bailed functions). ONE shared
 * definition — it is the physical FPU stack, so a value one runtime-mode function
 * leaves in st(0) is visible to its runtime-mode caller across chunk boundaries. */
long double __x87rt_s[16];
int __x87rt_p;
int __x87rt_rc;
unsigned short __x87rt_sw;

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
uint32_t aret_SetUnhandledExceptionFilter(uint32_t esp) { (void)esp; return 0; }
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
/* Thread-local storage (single-threaded model): a flat slot table. TlsAlloc
 * hands out distinct indices; Set/Get round-trip per index. */
#define ARET_TLS_MAX 1088
static uint32_t aret_tls[ARET_TLS_MAX];
static char aret_tls_used[ARET_TLS_MAX];
uint32_t aret_TlsAlloc(uint32_t esp) {
    (void)esp;
    for (int i = 0; i < ARET_TLS_MAX; i++) if (!aret_tls_used[i]) { aret_tls_used[i] = 1; aret_tls[i] = 0; return (uint32_t)i; }
    return 0xFFFFFFFFu; /* TLS_OUT_OF_INDEXES */
}
uint32_t aret_TlsGetValue(uint32_t esp) {
    uint32_t i = arg(esp, 0);
    return i < ARET_TLS_MAX ? aret_tls[i] : 0;
}
uint32_t aret_TlsSetValue(uint32_t esp) {
    uint32_t i = arg(esp, 0);
    if (i >= ARET_TLS_MAX) return 0;
    aret_tls[i] = arg(esp, 1);
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
uint32_t aret_InitializeCriticalSection(uint32_t esp) { (void)esp; return 0; }
/* Single-threaded model: the spin count is irrelevant and there is never
 * contention, so initialisation always succeeds. Returns BOOL=TRUE; a FALSE
 * here makes the CRT treat init as failed and __fastfail (int 0x29). */
uint32_t aret_InitializeCriticalSectionAndSpinCount(uint32_t esp) { (void)esp; return 1; }
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
uint32_t aret_InitializeCriticalSectionEx(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_DeleteCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_EnterCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_LeaveCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_IsDBCSLeadByteEx(uint32_t esp) { (void)esp; return 0; }
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
uint32_t aret_signal(uint32_t esp) { (void)esp; return 0; }
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
        aret_call(aret_atexit_fns[i], (uint32_t)(uintptr_t)f, 0, 0, 0, 0);
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
        if (fn) aret_call(fn, esp, 0, 0, 0, 0);
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
            uint32_t r = (uint32_t)aret_call(fn, esp, 0, 0, 0, 0);
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

static void aret_teb_init(void) {
    if (aret_teb_ready) return;
    aret_teb_ready = 1;
    uint32_t teb = (uint32_t)(uintptr_t)aret_teb;
    uint32_t peb = (uint32_t)(uintptr_t)aret_peb;
    uint32_t *t = (uint32_t *)aret_teb;
    /* NT_TIB / TEB (x86 offsets) */
    t[0x00 / 4] = 0xFFFFFFFFu;       /* ExceptionList: end of SEH chain */
    t[0x04 / 4] = 0x7FFF0000u;       /* StackBase  (permissive range)  */
    t[0x08 / 4] = 0x00010000u;       /* StackLimit                     */
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
