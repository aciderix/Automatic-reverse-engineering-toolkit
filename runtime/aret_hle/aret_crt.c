/* ARET CRT forwarding layer — "brancher le vrai CRT" (doc 30, reco #1).
 *
 * Rather than reimplementing the C runtime by hand, each msvcrt entry point is
 * forwarded to the *genuine host libc* with thin, ABI-accurate marshalling:
 * arguments are read from the shared machine stack at [esp+0], [esp+4], … (the
 * cdecl model — see aret_hle.h), passed to the real libc function, and the
 * result is returned in the 32-bit `eax` slot.
 *
 * These are strong definitions of `aret_<name>`; they override the weak stubs
 * the builder emits (aret_stubs.c), so simply linking this unit broadens CRT
 * coverage. msvcrt ≈ libc for the standard-C subset, so this is the real
 * runtime, not a shim of it.
 *
 * Honest scope: functions taking a *callback* (qsort/bsearch comparators,
 * sscanf write-back, atexit handlers beyond what aret_hle models) are NOT here —
 * a transpiled callback is a `sub_<va>` with the machine-stack ABI, not a native
 * cdecl function, so it cannot be handed to libc directly. Those need the
 * aret_call dispatch path and are left to dedicated shims.
 */

#include "aret_hle.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <locale.h>
#include <math.h>
#include <errno.h>
#include <sys/stat.h>   /* gettext catalog-existence guard (stat a real .mo) */

/* Read cdecl argument `i` (a 32-bit word) from the modelled stack. */
static inline uint32_t a32(uint32_t esp, int i) {
    return ((const uint32_t *)(uintptr_t)esp)[i];
}

/* Typed views of an argument slot. */
#define AI(i)  ((int)a32(esp, (i)))             /* int          */
#define AU(i)  (a32(esp, (i)))                  /* unsigned     */
#define AP(i)  ((void *)(uintptr_t)a32(esp, (i)))       /* void*        */
#define AS(i)  ((char *)(uintptr_t)a32(esp, (i)))       /* char*        */
#define ACS(i) ((const char *)(uintptr_t)a32(esp, (i))) /* const char*  */
/* Return a pointer as the 32-bit eax slot. */
#define RP(x)  ((uint32_t)(uintptr_t)(x))

/* The shared variadic formatter (defined in aret_hle.c). `a` points at the first
 * variadic slot. Returns the number of characters written (excluding the NUL). */
size_t aret_vformat(char *out, size_t cap, const char *fmt, const uint32_t *a);

/* ------------------------------------------------------------------ */
/* <direct.h> / <unistd.h> — working directory & path resolution      */
/* ------------------------------------------------------------------ */
/* Guest pointers are flat native addresses (the binary is -m32), so the host
 * calls write straight into the guest's buffers. The guest frees results with
 * the msvcrt `free` import (-> aret_free -> host free), so a host-allocated
 * return (NULL-buffer form) is freed consistently. */

/* char *_getcwd(char *buf, int size): fill buf with the cwd; if buf is NULL,
 * msvcrt allocates — emulate with glibc's getcwd(NULL, 0). */
uint32_t aret_getcwd(uint32_t esp) {
    char *buf = AS(0);
    int size = AI(1);
    if (!buf) return RP(getcwd(NULL, 0));
    return RP(getcwd(buf, (size_t)size));
}
/* int _chdir(const char *path): 0 on success, -1 on error. */
uint32_t aret_chdir(uint32_t esp) { return (uint32_t)chdir(ACS(0)); }
/* char *_fullpath(char *abs, const char *rel, size_t len): resolve rel to an
 * absolute path in abs. realpath is the POSIX equivalent but requires the path
 * to exist; on failure fall back to a best-effort absolute join so callers that
 * only need a normalized name still proceed. */
uint32_t aret_fullpath(uint32_t esp) {
    char *abs = AS(0);
    const char *rel = ACS(1);
    if (!abs || !rel) return 0;
    if (realpath(rel, abs)) return RP(abs);
    if (rel[0] == '/' || rel[0] == '\\') {
        strncpy(abs, rel, AU(2) ? AU(2) - 1 : 259);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(abs, AU(2) ? AU(2) : 260, "%s/%s", cwd, rel);
    }
    return RP(abs);
}

/* ------------------------------------------------------------------ */
/* <string.h> — the subset not already in aret_hle.c                  */
/* ------------------------------------------------------------------ */

uint32_t aret_strncpy(uint32_t esp) { return RP(strncpy(AS(0), ACS(1), AU(2))); }
uint32_t aret_strcat(uint32_t esp)  { return RP(strcat(AS(0), ACS(1))); }
uint32_t aret_strncat(uint32_t esp) { return RP(strncat(AS(0), ACS(1), AU(2))); }
uint32_t aret_strrchr(uint32_t esp) { return RP(strrchr(ACS(0), AI(1))); }
uint32_t aret_strstr(uint32_t esp)  { return RP(strstr(ACS(0), ACS(1))); }
uint32_t aret_strspn(uint32_t esp)  { return (uint32_t)strspn(ACS(0), ACS(1)); }
uint32_t aret_strcspn(uint32_t esp) { return (uint32_t)strcspn(ACS(0), ACS(1)); }
uint32_t aret_strpbrk(uint32_t esp) { return RP(strpbrk(ACS(0), ACS(1))); }
uint32_t aret_strtok(uint32_t esp)  { return RP(strtok(AS(0), ACS(1))); }
/* strdup and _strdup both sanitize to aret_strdup (leading _ stripped). */
uint32_t aret_strdup(uint32_t esp)  { return RP(strdup(ACS(0))); }
uint32_t aret_strcoll(uint32_t esp) { return (uint32_t)(int32_t)strcoll(ACS(0), ACS(1)); }
uint32_t aret_memcmp(uint32_t esp)  { return (uint32_t)(int32_t)memcmp(AP(0), AP(1), AU(2)); }
uint32_t aret_memchr(uint32_t esp)  { return RP(memchr(AP(0), AI(1), AU(2))); }
/* Case-insensitive compares. The import sanitizer strips the leading underscore,
 * so msvcrt's _stricmp/_strnicmp arrive as aret_stricmp/aret_strnicmp. */
uint32_t aret_stricmp(uint32_t esp)   { return (uint32_t)(int32_t)strcasecmp(ACS(0), ACS(1)); }
uint32_t aret_strcasecmp(uint32_t esp){ return (uint32_t)(int32_t)strcasecmp(ACS(0), ACS(1)); }
uint32_t aret_strnicmp(uint32_t esp)  { return (uint32_t)(int32_t)strncasecmp(ACS(0), ACS(1), AU(2)); }
uint32_t aret_strncasecmp(uint32_t esp){ return (uint32_t)(int32_t)strncasecmp(ACS(0), ACS(1), AU(2)); }

/* ------------------------------------------------------------------ */
/* <stdlib.h> — conversions, environment, RNG                         */
/* ------------------------------------------------------------------ */

uint32_t aret_atol(uint32_t esp)   { return (uint32_t)atol(ACS(0)); }
uint32_t aret_abs(uint32_t esp)    { return (uint32_t)abs(AI(0)); }
uint32_t aret_labs(uint32_t esp)   { return (uint32_t)labs(AI(0)); }
/* Windows msvcrt uses a specific LCG; a program that seeds and reads `rand()`
 * expects exactly this sequence, so reproduce it rather than forwarding to the
 * host's rand() (which gives a different stream). */
static uint32_t aret_rand_seed = 1;
uint32_t aret_rand(uint32_t esp)   { (void)esp; aret_rand_seed = aret_rand_seed * 214013u + 2531011u; return (aret_rand_seed >> 16) & 0x7fff; }
uint32_t aret_srand(uint32_t esp)  { aret_rand_seed = AU(0); return 0; }
/* _controlfp(new, mask) -> the (new) control word. The msvcrt FP control word in its
 * platform-independent encoding; the process default is 0x0008001f (all exceptions
 * masked, 53-bit precision, round-to-nearest) — measured from Wine. Stateful: set
 * updates (cur & ~mask)|(new & mask) and returns it; a query (mask 0) returns the
 * current word. Matches Wine's observable values. (The actual FP rounding ARET uses
 * is the host default; a program that SETS a non-default rounding and relies on it is
 * the same bounded x87 limitation as a custom fldcw — rare.) */
static uint32_t aret_fp_cw = 0x0008001fu;
uint32_t aret_controlfp(uint32_t esp) {
    uint32_t neu = AU(0), mask = AU(1);
    aret_fp_cw = (aret_fp_cw & ~mask) | (neu & mask);
    return aret_fp_cw;
}
/* _controlfp_s(&cur, new, mask): the secure form — writes the control word through
 * the first arg (if non-NULL) and returns 0 (success). */
uint32_t aret_controlfp_s(uint32_t esp) {
    uint32_t out = AU(0), neu = AU(1), mask = AU(2);
    aret_fp_cw = (aret_fp_cw & ~mask) | (neu & mask);
    if (out) *(uint32_t *)(uintptr_t)out = aret_fp_cw;
    return 0;
}
uint32_t aret_getenv(uint32_t esp) { return RP(getenv(ACS(0))); }
/* Windows `_putenv` copies the "NAME=value" string; libc `putenv` stores the
 * pointer, so strdup to match (and avoid a dangling guest-stack pointer). */
uint32_t aret_putenv(uint32_t esp) { const char *s = ACS(0); return (uint32_t)putenv(s ? strdup(s) : (char *)""); }
uint32_t aret_tzset(uint32_t esp)  { (void)esp; tzset(); return 0; }
uint32_t aret_strtol(uint32_t esp) {
    char *end; long v = strtol(ACS(0), &end, AI(2));
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    return (uint32_t)v;
}
uint32_t aret_strtoul(uint32_t esp) {
    char *end; unsigned long v = strtoul(ACS(0), &end, AI(2));
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    return (uint32_t)v;
}
/* 64-bit results: returned in the edx:eax pair (low | high<<32). The builder
 * declares these shims `uint64_t` (see import_returns_u64) so the call site keeps
 * both halves. */
uint64_t aret_strtoll(uint32_t esp) {
    char *end; long long v = strtoll(ACS(0), &end, AI(2));
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    return (uint64_t)v;
}
uint64_t aret_strtoull(uint32_t esp) {
    char *end; unsigned long long v = strtoull(ACS(0), &end, AI(2));
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    return (uint64_t)v;
}
/* msvcrt names (mingw maps strtoll/strtoull onto these) — same semantics. */
uint64_t aret_strtoi64(uint32_t esp)  { return aret_strtoll(esp); }
uint64_t aret_strtoui64(uint32_t esp) { return aret_strtoull(esp); }
/* div_t/ldiv_t are 8-byte structs returned in edx:eax: quot in eax, rem in edx. */
uint64_t aret_div(uint32_t esp) {
    int num = AI(0), den = AI(1);
    return (uint32_t)(num / den) | ((uint64_t)(uint32_t)(num % den) << 32);
}
uint64_t aret_ldiv(uint32_t esp) {
    long num = (int32_t)AU(0), den = (int32_t)AU(1);
    return (uint32_t)(num / den) | ((uint64_t)(uint32_t)(num % den) << 32);
}

/* <stdlib.h> Windows path helpers. _splitpath breaks a path into drive ("C:"),
 * directory (through the last separator), filename and extension (with dot); any
 * output buffer may be NULL. _makepath is its inverse; _fullpath resolves to an
 * absolute path against the cwd. */
uint32_t aret_splitpath(uint32_t esp) {
    const char *path = ACS(0);
    char *drive = AS(1), *dir = AS(2), *fname = AS(3), *ext = AS(4);
    const char *p = path ? path : "";
    if (p[0] && p[1] == ':') { if (drive) { drive[0] = p[0]; drive[1] = ':'; drive[2] = 0; } p += 2; }
    else if (drive) drive[0] = 0;
    const char *sep = NULL;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') sep = q;
    const char *fn;
    if (sep) { if (dir) { size_t n = (size_t)(sep - p + 1); memcpy(dir, p, n); dir[n] = 0; } fn = sep + 1; }
    else { if (dir) dir[0] = 0; fn = p; }
    const char *dot = NULL;
    for (const char *q = fn; *q; q++) if (*q == '.') dot = q;
    if (dot) { if (fname) { size_t n = (size_t)(dot - fn); memcpy(fname, fn, n); fname[n] = 0; } if (ext) strcpy(ext, dot); }
    else { if (fname) strcpy(fname, fn); if (ext) ext[0] = 0; }
    return 0;
}
uint32_t aret_makepath(uint32_t esp) {
    char *path = AS(0);
    const char *drive = ACS(1), *dir = ACS(2), *fname = ACS(3), *ext = ACS(4);
    char *o = path;
    if (drive && drive[0]) { *o++ = drive[0]; *o++ = ':'; }
    if (dir && dir[0]) { size_t n = strlen(dir); memcpy(o, dir, n); o += n; if (o[-1] != '/' && o[-1] != '\\') *o++ = '\\'; }
    if (fname) { size_t n = strlen(fname); memcpy(o, fname, n); o += n; }
    if (ext && ext[0]) { if (ext[0] != '.') *o++ = '.'; size_t n = strlen(ext); memcpy(o, ext, n); o += n; }
    *o = 0;
    return 0;
}
/* strtod/atof: David Gay's bignum strtod in the binary is huge and doesn't lift
 * cleanly; forward to the host. The double result is returned in st(0) via the
 * x87 fp channel (the caller recovers it after an fp-returning call). */
extern long double __aret_x87_ret; extern int __aret_x87_ret_valid;
uint32_t aret_strtod(uint32_t esp) {
    char *end; double v = strtod(ACS(0), &end);
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    __aret_x87_ret = v; __aret_x87_ret_valid = 1;
    return 0;
}
uint32_t aret_atof(uint32_t esp) { __aret_x87_ret = atof(ACS(0)); __aret_x87_ret_valid = 1; return 0; }

/* msvcrt `_errno()` returns `int *` to the (thread-local) errno; our libc shims
 * set the host errno, and this hands back the same location so callers that
 * check it (e.g. strtod overflow → ERANGE) read a consistent value. */
uint32_t aret_errno(uint32_t esp) { (void)esp; return RP(&errno); }

/* ------------------------------------------------------------------ */
/* <stdio.h> — buffered string/char I/O not already in aret_hle.c     */
/* ------------------------------------------------------------------ */

uint32_t aret_sprintf(uint32_t esp) {
    char *dst = AS(0);
    const char *fmt = ACS(1);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[2];
    size_t n = aret_vformat(dst, (size_t)1 << 30, fmt, va);
    return (uint32_t)n;
}
/* wsprintfA(buf, fmt, ...) — USER32's sprintf; same cdecl/variadic layout. */
uint32_t aret_wsprintfA(uint32_t esp) {
    char *dst = AS(0);
    const char *fmt = ACS(1);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[2];
    return (uint32_t)aret_vformat(dst, (size_t)1 << 30, fmt, va);
}
/* wvsprintfA(buf, fmt, arglist) — the va_list form of wsprintfA (the measured gap):
 * arglist is a pointer to the packed arguments, so aret_vformat reads from there. */
uint32_t aret_wvsprintfA(uint32_t esp) {
    char *dst = AS(0);
    const char *fmt = ACS(1);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(2);
    return (uint32_t)aret_vformat(dst, (size_t)1 << 30, fmt, va);
}
/* snprintf / _snprintf: (dst, n, fmt, ...). cap of 0 means "measure".
 * C99 returns the number of chars that *would* have been written (the full
 * formatted length), not the truncated count — so format into a measuring
 * buffer first, then copy the part that fits and NUL-terminate within cap. */
uint32_t aret_snprintf(uint32_t esp) {
    char *dst = AS(0);
    size_t cap = AU(1);
    const char *fmt = ACS(2);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[3];
    char tmp[8192];
    size_t n = aret_vformat(tmp, sizeof tmp, fmt, va);
    if (cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(dst, tmp, c); dst[c] = 0; }
    return (uint32_t)n;
}
/* snprintf and _snprintf both sanitize to aret_snprintf. */

/* The `v*` variants take an explicit va_list as the final argument. On 32-bit
 * cdecl a va_list is just a pointer into the caller's stack at the first
 * variadic word, so `aret_vformat` reads from there directly. */
uint32_t aret_vsnprintf(uint32_t esp) {
    char *dst = AS(0);
    size_t cap = AU(1);
    const char *fmt = ACS(2);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(3);
    char tmp[8192];
    size_t n = aret_vformat(tmp, sizeof tmp, fmt, va);
    if (cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(dst, tmp, c); dst[c] = 0; }
    return (uint32_t)n;
}
uint32_t aret_vsprintf(uint32_t esp) {
    char *dst = AS(0);
    const char *fmt = ACS(1);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(2);
    size_t n = aret_vformat(dst, (size_t)1 << 30, fmt, va);
    return (uint32_t)n;
}
/* __stdio_common_vsprintf(uint64 options, char* buf, size_t count, const char* fmt,
 * _locale_t locale, va_list va) — the UCRT vsnprintf core (comctl32 socle). The 64-bit
 * options occupy slots 0-1; buffer=2, count=3, format=4, locale=5, va=6. We honour the
 * truncating vsnprintf contract (options/locale ignored — the C locale is our default). */
uint32_t aret_stdio_common_vsprintf(uint32_t esp) {
    char *dst = AS(2); size_t cap = AU(3);
    const char *fmt = ACS(4);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(6);
    char tmp[8192];
    size_t n = aret_vformat(tmp, sizeof tmp, fmt, va);
    if (dst && cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(dst, tmp, c); dst[c] = 0; }
    return (uint32_t)n;
}

/* sscanf core — parse `in` per `fmt`, writing to the pointer args `a[]`. Returns the
 * number of items assigned, or EOF(-1) if input ran out before the first assignment.
 * Numeric conversions defer to strtoll/strtoull/strtod (correct base/sign/overflow);
 * %s/%c/%[…] are scanned directly. Handles width, suppression (%*), length (h/l/ll)
 * and %n. Output pointer size follows the length modifier. */
static int aret_sscanf_core(const char *in, const char *fmt, const uint32_t *a) {
    int ai = 0, assigned = 0, attempted = 0;
    const char *ip = in;
    for (const char *p = fmt; *p; ) {
        if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*ip)) ip++; p++; continue; }
        if (*p != '%') { if (*ip != *p) break; ip++; p++; continue; }
        p++;
        if (*p == '%') { if (*ip != '%') { break; } ip++; p++; continue; }
        int suppress = 0; if (*p == '*') { suppress = 1; p++; }
        int width = 0; while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        int lng = 0, sht = 0;
        for (;;) {
            if (*p == 'l') { lng++; p++; }
            else if (*p == 'h') { sht++; p++; }
            else if (*p == 'L' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'I') { p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        if (!conv) break;
        int w = width ? width : 0x3fffffff;

        if (conv == 'n') { if (!suppress) *(int *)(uintptr_t)a[ai++] = (int)(ip - in); continue; }
        if (conv == 'c') {
            int cnt = width ? width : 1;
            attempted = 1;
            char *dst = suppress ? 0 : (char *)(uintptr_t)a[ai];
            int got = 0;
            for (; got < cnt && *ip; got++) { if (dst) dst[got] = *ip; ip++; }
            if (got < cnt) break;                         /* not enough chars */
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        if (conv == 's') {
            while (isspace((unsigned char)*ip)) ip++;
            attempted = 1;
            char *dst = suppress ? 0 : (char *)(uintptr_t)a[ai];
            int got = 0;
            while (*ip && !isspace((unsigned char)*ip) && got < w) { if (dst) dst[got] = *ip; ip++; got++; }
            if (got == 0) break;
            if (dst) dst[got] = 0;
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        if (conv == '[') {
            int negate = 0; if (*p == '^') { negate = 1; p++; }
            char set[256]; memset(set, 0, sizeof set);
            if (*p == ']') { set[(unsigned char)']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') { for (int c = (unsigned char)p[0]; c <= (unsigned char)p[2]; c++) set[c] = 1; p += 3; }
                else { set[(unsigned char)*p] = 1; p++; }
            }
            if (*p == ']') p++;
            attempted = 1;
            char *dst = suppress ? 0 : (char *)(uintptr_t)a[ai];
            int got = 0;
            while (*ip && got < w) { int m = set[(unsigned char)*ip]; if (negate) m = !m; if (!m) break; if (dst) dst[got] = *ip; ip++; got++; }
            if (got == 0) break;
            if (dst) dst[got] = 0;
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        /* numeric: skip leading whitespace, then convert */
        while (isspace((unsigned char)*ip)) ip++;
        attempted = 1;
        char buf[130]; const char *src = ip;
        if (width && width < 129) { int k = 0; while (ip[k] && k < width) { buf[k] = ip[k]; k++; } buf[k] = 0; src = buf; }
        char *end;
        if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            double d = strtod(src, &end);
            if (end == src) break;
            ip += (end - src);
            if (!suppress) { if (lng) *(double *)(uintptr_t)a[ai] = d; else *(float *)(uintptr_t)a[ai] = (float)d; ai++; assigned++; }
            continue;
        }
        int base = conv == 'i' ? 0 : (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : conv == 'p' ? 16 : 10;
        int isu = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p');
        unsigned long long uv = 0; long long sv = 0;
        if (isu) uv = strtoull(src, &end, base); else sv = strtoll(src, &end, base);
        if (end == src) break;
        ip += (end - src);
        if (!suppress) {
            void *dp = (void *)(uintptr_t)a[ai];
            unsigned long long val = isu ? uv : (unsigned long long)sv;
            if (lng >= 2) *(long long *)dp = (long long)val;
            else if (sht >= 2) *(char *)dp = (char)val;
            else if (sht) *(short *)dp = (short)val;
            else *(int *)dp = (int)val;
            ai++; assigned++;
        }
    }
    if (assigned == 0 && attempted && *ip == 0) return -1;    /* EOF before first assignment */
    return assigned;
}
uint32_t aret_sscanf(uint32_t esp) {
    const char *in = ACS(0), *fmt = ACS(1);
    const uint32_t *a = &((const uint32_t *)(uintptr_t)esp)[2];
    if (!in || !fmt) return (uint32_t)-1;
    return (uint32_t)(int32_t)aret_sscanf_core(in, fmt, a);
}
/* swscanf — the wide (16-bit) analog of sscanf. The format is ASCII (read as 16-bit);
 * the input is 16-bit. Numeric tokens are copied to a narrow buffer and parsed with
 * the same strtoll/strtod path; %s/%c/%[…] write 16-bit output (%hs/%S = narrow). */
static int aret_swscanf_core(const uint16_t *in, const uint16_t *fmt, const uint32_t *a) {
    int ai = 0, assigned = 0, attempted = 0;
    const uint16_t *ip = in;
    for (const uint16_t *p = fmt; *p; ) {
        if (*p < 128 && isspace((int)*p)) { while (*ip < 128 && isspace((int)*ip)) ip++; p++; continue; }
        if (*p != '%') { if (*ip != *p) break; ip++; p++; continue; }
        p++;
        if (*p == '%') { if (*ip != '%') break; ip++; p++; continue; }
        int suppress = 0; if (*p == '*') { suppress = 1; p++; }
        int width = 0; while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int lng = 0, sht = 0, narrow_str = 0;
        for (;;) {
            if (*p == 'l') { lng++; p++; }
            else if (*p == 'h') { sht++; narrow_str = 1; p++; }
            else if (*p == 'L' || *p == 'j' || *p == 'z' || *p == 't' || *p == 'I') { p++; }
            else break;
        }
        char conv = *p ? (char)*p++ : 0;
        if (!conv) break;
        int w = width ? width : 0x3fffffff;
        if (conv == 'S') narrow_str = 1;                  /* %S = narrow in a wide scanf */

        if (conv == 'n') { if (!suppress) *(int *)(uintptr_t)a[ai++] = (int)(ip - in); continue; }
        if (conv == 'c') {
            int cnt = width ? width : 1; attempted = 1;
            void *dst = suppress ? 0 : (void *)(uintptr_t)a[ai];
            int got = 0;
            for (; got < cnt && *ip; got++) { if (dst) { if (narrow_str) ((char *)dst)[got] = (char)*ip; else ((uint16_t *)dst)[got] = *ip; } ip++; }
            if (got < cnt) break;
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        if (conv == 's' || conv == 'S') {
            while (*ip < 128 && isspace((int)*ip)) ip++;
            attempted = 1;
            void *dst = suppress ? 0 : (void *)(uintptr_t)a[ai];
            int got = 0;
            while (*ip && !(*ip < 128 && isspace((int)*ip)) && got < w) { if (dst) { if (narrow_str) ((char *)dst)[got] = (char)*ip; else ((uint16_t *)dst)[got] = *ip; } ip++; got++; }
            if (got == 0) break;
            if (dst) { if (narrow_str) ((char *)dst)[got] = 0; else ((uint16_t *)dst)[got] = 0; }
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        if (conv == '[') {
            int negate = 0; if (*p == '^') { negate = 1; p++; }
            char set[256]; memset(set, 0, sizeof set);
            if (*p == ']') { set[']'] = 1; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') { for (int cc = (int)p[0]; cc <= (int)p[2]; cc++) if (cc < 256) set[cc] = 1; p += 3; }
                else { if (*p < 256) set[*p] = 1; p++; }
            }
            if (*p == ']') p++;
            attempted = 1;
            void *dst = suppress ? 0 : (void *)(uintptr_t)a[ai];
            int got = 0;
            while (*ip && got < w) { int m = (*ip < 256) ? set[*ip] : 0; if (negate) m = !m; if (!m) break; if (dst) { if (narrow_str) ((char *)dst)[got] = (char)*ip; else ((uint16_t *)dst)[got] = *ip; } ip++; got++; }
            if (got == 0) break;
            if (dst) { if (narrow_str) ((char *)dst)[got] = 0; else ((uint16_t *)dst)[got] = 0; }
            if (!suppress) { ai++; assigned++; }
            continue;
        }
        while (*ip < 128 && isspace((int)*ip)) ip++;
        attempted = 1;
        char buf[130]; int bn = 0;
        while (ip[bn] && ip[bn] < 128 && bn < 129 && (!width || bn < width)) { buf[bn] = (char)ip[bn]; bn++; }
        buf[bn] = 0;
        char *end;
        if (conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
            double d = strtod(buf, &end);
            if (end == buf) break;
            ip += (end - buf);
            if (!suppress) { if (lng) *(double *)(uintptr_t)a[ai] = d; else *(float *)(uintptr_t)a[ai] = (float)d; ai++; assigned++; }
            continue;
        }
        int base = conv == 'i' ? 0 : (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : conv == 'p' ? 16 : 10;
        int isu = (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p');
        unsigned long long uv = 0; long long sv = 0;
        if (isu) uv = strtoull(buf, &end, base); else sv = strtoll(buf, &end, base);
        if (end == buf) break;
        ip += (end - buf);
        if (!suppress) {
            void *dp = (void *)(uintptr_t)a[ai];
            unsigned long long val = isu ? uv : (unsigned long long)sv;
            if (lng >= 2) *(long long *)dp = (long long)val;
            else if (sht >= 2) *(char *)dp = (char)val;
            else if (sht) *(short *)dp = (short)val;
            else *(int *)dp = (int)val;
            ai++; assigned++;
        }
    }
    if (assigned == 0 && attempted && *ip == 0) return -1;
    return assigned;
}
uint32_t aret_swscanf(uint32_t esp) {
    const uint16_t *in = (const uint16_t *)AP(0), *fmt = (const uint16_t *)AP(1);
    const uint32_t *a = &((const uint32_t *)(uintptr_t)esp)[2];
    if (!in || !fmt) return (uint32_t)-1;
    return (uint32_t)(int32_t)aret_swscanf_core(in, fmt, a);
}

/* Wide character classification. mingw's own wide scanf/CRT drives all its tests
 * through iswctype(c, desc) with the msvcrt ctype bit mask. For c < 128 we compute
 * the exact C-locale type bits; non-ASCII is unclassified (0) — the C/default-locale
 * behaviour. The isw* helpers delegate to the narrow ctype (ASCII-exact). */
static uint32_t w_ctype_mask(uint32_t c) {
    if (c >= 128) return 0;
    uint32_t m = 0;
    if (c >= 'A' && c <= 'Z') m |= 0x0001 | 0x0100;                 /* _UPPER | alpha */
    if (c >= 'a' && c <= 'z') m |= 0x0002 | 0x0100;                 /* _LOWER | alpha */
    if (c >= '0' && c <= '9') m |= 0x0004 | 0x0080;                 /* _DIGIT | _HEX  */
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= 0x0080; /* _HEX        */
    if (c == ' ' || (c >= '\t' && c <= '\r')) m |= 0x0008;          /* _SPACE         */
    if (c == ' ' || c == '\t') m |= 0x0040;                        /* _BLANK         */
    if (c < 0x20 || c == 0x7f) m |= 0x0020;                        /* _CONTROL       */
    if (isprint((int)c) && !isalnum((int)c) && c != ' ') m |= 0x0010; /* _PUNCT      */
    return m;
}
uint32_t aret_iswctype(uint32_t esp) { return w_ctype_mask(AU(0)) & AU(1); }
uint32_t aret_iswspace(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isspace((int)c) ? 1 : 0; }
uint32_t aret_iswdigit(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isdigit((int)c) ? 1 : 0; }
uint32_t aret_iswalpha(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isalpha((int)c) ? 1 : 0; }
uint32_t aret_iswalnum(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isalnum((int)c) ? 1 : 0; }
uint32_t aret_iswupper(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isupper((int)c) ? 1 : 0; }
uint32_t aret_iswlower(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && islower((int)c) ? 1 : 0; }
uint32_t aret_iswpunct(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && ispunct((int)c) ? 1 : 0; }
uint32_t aret_iswxdigit(uint32_t esp) { uint32_t c = AU(0); return c < 128 && isxdigit((int)c) ? 1 : 0; }
uint32_t aret_iswcntrl(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && iscntrl((int)c) ? 1 : 0; }
uint32_t aret_iswprint(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isprint((int)c) ? 1 : 0; }
uint32_t aret_iswgraph(uint32_t esp)  { uint32_t c = AU(0); return c < 128 && isgraph((int)c) ? 1 : 0; }
/* MBCS lead/trail byte tests (msvcrt). In a single-byte (C/en-US) locale — the only
 * one modelled, matching IsDBCSLeadByte=0 — no byte is a multibyte lead or trail. */
uint32_t aret_ismbblead(uint32_t esp)  { (void)esp; return 0; }
uint32_t aret_ismbbtrail(uint32_t esp) { (void)esp; return 0; }
/* isleadbyte(c): true iff `c` is the first byte of a multibyte character in the
 * current locale. In the single-byte C/en-US locale (the only one modelled,
 * matching IsDBCSLeadByte=0) no byte is ever a lead byte -> 0. */
uint32_t aret_isleadbyte(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_ismbblead_l(uint32_t esp)  { (void)esp; return 0; }
uint32_t aret_ismbbtrail_l(uint32_t esp) { (void)esp; return 0; }

/* Wide (16-bit) printf family. The wide formatter lives in aret_hle.c beside
 * aret_vformat. Windows wchar_t is 16-bit, so buffers/format strings are uint16_t. */
size_t aret_wvformat(uint16_t *out, size_t cap, const uint16_t *fmt, const uint32_t *a);
/* NOTE: `swprintf` is deliberately NOT modelled — its signature is CRT-version
 * ambiguous (legacy `(buf, fmt, ...)` vs the C99/secure `(buf, count, fmt, ...)`
 * that Wine's msvcrt uses), so guessing one silently mis-parses arguments (Wine
 * itself faults on the wrong form). It stays a sound abort; the unambiguous
 * `_snwprintf`/`wsprintfW`/`_vsnwprintf` cover wide formatting. */
/* wsprintfW(dst, fmt, ...) — USER32's wide sprintf (max 1024 wchars). */
uint32_t aret_wsprintfW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    const uint16_t *fmt = (const uint16_t *)AP(1);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[2];
    return (uint32_t)aret_wvformat(dst, 1024, fmt, va);
}
/* wvsprintfW(dst, fmt, arglist) — va_list form; arglist points at the packed args. */
uint32_t aret_wvsprintfW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    const uint16_t *fmt = (const uint16_t *)AP(1);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(2);
    return (uint32_t)aret_wvformat(dst, 1024, fmt, va);
}
/* _snwprintf(dst, count, fmt, ...) — msvcrt: returns the count written, or -1 if
 * the output was truncated (does NOT NUL-terminate on an exact fill). */
static uint32_t aret_wsn_finish(uint16_t *dst, size_t cap, const uint16_t *tmp, size_t n) {
    if (cap == 0) return (uint32_t)-1;
    if (n < cap) { memcpy(dst, tmp, n * 2); dst[n] = 0; return (uint32_t)n; }
    memcpy(dst, tmp, cap * 2);            /* fill, no terminator */
    return (uint32_t)-1;
}
uint32_t aret_snwprintf(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    size_t cap = AU(1);
    const uint16_t *fmt = (const uint16_t *)AP(2);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[3];
    static uint16_t tmp[8192];
    size_t n = aret_wvformat(tmp, 8192, fmt, va);
    return aret_wsn_finish(dst, cap, tmp, n);
}
/* swprintf_s(dst, sizeOfBuffer, fmt, ...) and vswprintf_s(dst, sizeOfBuffer, fmt,
 * arglist) — the secure wide sprintf with NO separate count.
 *
 * Note the contrast with `swprintf` two comments above, which stays a sound abort:
 * that one's signature is CRT-version ambiguous, so guessing mis-parses arguments.
 * `swprintf_s` has exactly one shape (`buf, count, fmt, …`), so refusing to guess on
 * the first does not stop us serving the second — different questions, different
 * answers.
 *
 * MEASURED against Wine's msvcrt by sweeping the capacity 0..8 on a 5-character
 * result, on a poisoned buffer, and the failure path is the whole point:
 *   - capacity 0 leaves the buffer COMPLETELY untouched and returns -1;
 *   - a capacity between 1 and 5 (too small) returns -1 and ZERO-FILLS exactly
 *     `capacity` units — not just `dst[0]`, and not the whole array. A shim that
 *     wrote only the terminator would look right to any caller that reads a string;
 *   - it fits -> the text plus its NUL, and the return is the LENGTH, not the size.
 * ⚠️ The narrow twins `sprintf_s`/`vsprintf_s` are deliberately NOT implemented
 * here. Measured through a `.def`-forced msvcrt import, Wine's narrow versions do
 * something else entirely — they leave PARTIAL output on failure instead of
 * zero-filling, and at an exact fit they return the length while writing NO
 * terminator, which no caller could use safely. That is self-inconsistent with the
 * wide twin, so it is not a contract we are willing to reproduce from one oracle:
 * they stay a loud abort and the question goes to the Windows runner
 * (bench/winoracle/crt_sprintfs_disputed.c). */
static uint32_t aret_wsprintf_s_finish(uint16_t *dst, size_t cap,
                                       const uint16_t *tmp, size_t n) {
    if (!dst || cap == 0) return (uint32_t)-1;      /* buffer untouched: measured */
    if (n < cap) {
        memcpy(dst, tmp, n * 2);
        dst[n] = 0;
        return (uint32_t)n;
    }
    for (size_t i = 0; i < cap; i++) dst[i] = 0;    /* exactly `cap` units, measured */
    return (uint32_t)-1;
}
uint32_t aret_swprintf_s(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    size_t cap = AU(1);
    const uint16_t *fmt = (const uint16_t *)AP(2);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[3];
    static uint16_t tmp[8192];
    if (!fmt) return (uint32_t)-1;
    size_t n = aret_wvformat(tmp, 8192, fmt, va);
    return aret_wsprintf_s_finish(dst, cap, tmp, n);
}
uint32_t aret_vswprintf_s(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    size_t cap = AU(1);
    const uint16_t *fmt = (const uint16_t *)AP(2);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(3);
    static uint16_t tmp[8192];
    if (!fmt) return (uint32_t)-1;
    size_t n = aret_wvformat(tmp, 8192, fmt, va);
    return aret_wsprintf_s_finish(dst, cap, tmp, n);
}

/* _snwprintf_s(dst, sizeOfBuffer, count, fmt, ...) — the secure wide snprintf.
 * MEASURED against Wine across nine shapes, which is what separates FOUR regimes that
 * a single "truncate and return -1" implementation would blur together:
 *   - it fits within both `count` and the buffer  -> writes it, returns the LENGTH;
 *   - `count` is the binding limit and the buffer could have held more -> writes
 *     `count` characters plus a NUL, KEEPS them, returns -1;
 *   - `count` is _TRUNCATE ((size_t)-1) and the text is longer than the buffer ->
 *     writes size-1 characters plus a NUL, KEEPS them, returns -1;
 *   - the BUFFER is what is too small (count >= size) -> ZEROES THE WHOLE BUFFER, all
 *     `size` elements, not just the first, and returns -1. This is the one that is
 *     genuinely surprising: the two "too long" cases differ by whether the caller
 *     asked for truncation, and only the un-asked-for one destroys the output.
 * Edges: size 0 leaves the buffer completely untouched and returns -1; a NULL fmt
 * empties the buffer and returns -1; a NULL BUFFER is a length QUERY — it returns how
 * many characters the result would take (measured 3 for L"abc"), it is not an error. */
uint32_t aret_snwprintf_s(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    size_t size = AU(1);
    uint32_t count = AU(2);
    const uint16_t *fmt = (const uint16_t *)AP(3);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[4];
    static uint16_t tmp[8192];
    if (!fmt) { if (dst && size) dst[0] = 0; return (uint32_t)-1; }
    size_t n = aret_wvformat(tmp, 8192, fmt, va);
    if (!dst) return (uint32_t)n;                  /* length query, not a failure */
    if (size == 0) return (uint32_t)-1;            /* buffer untouched */
    int trunc = (count == 0xffffffffu);
    size_t limit = size - 1;
    if (!trunc && (size_t)count < limit) limit = count;
    if (n <= limit) {
        for (size_t i = 0; i < n; i++) dst[i] = tmp[i];
        dst[n] = 0;
        return (uint32_t)n;
    }
    if (!trunc && (size_t)count >= size) {         /* the BUFFER was too small */
        for (size_t i = 0; i < size; i++) dst[i] = 0;
        return (uint32_t)-1;
    }
    for (size_t i = 0; i < limit; i++) dst[i] = tmp[i];  /* asked-for truncation */
    dst[limit] = 0;
    return (uint32_t)-1;
}

/* _vsnwprintf(dst, count, fmt, va_list) — the va_list is a pointer to the args. */
uint32_t aret_vsnwprintf(uint32_t esp) {
    uint16_t *dst = (uint16_t *)AP(0);
    size_t cap = AU(1);
    const uint16_t *fmt = (const uint16_t *)AP(2);
    const uint32_t *va = (const uint32_t *)(uintptr_t)AU(3);
    static uint16_t tmp[8192];
    size_t n = aret_wvformat(tmp, 8192, fmt, va);
    return aret_wsn_finish(dst, cap, tmp, n);
}

/* aret_fflush lives in aret_hle.c (next to the synthetic _iob machinery): a
 * flush of a stdin/out/err stream must NOT be passed to the host fflush as a
 * raw pointer — those are our unbuffered _iob entries, not host FILE*. */

/* ------------------------------------------------------------------ */
/* <time.h> — wall-clock seconds and process clock (msvcrt uses 32-bit */
/* time_t for `time`; the result is the 32-bit eax slot).             */
/* ------------------------------------------------------------------ */

uint32_t aret_time(uint32_t esp) {
    int32_t t = (int32_t)time(NULL);
    if (AU(0)) *(int32_t *)AP(0) = t;   /* optional time_t* out-param */
    return (uint32_t)t;
}
uint32_t aret_clock(uint32_t esp) { (void)esp; return (uint32_t)clock(); }

/* localeconv: both msvcrt and glibc put `char *decimal_point` first in `struct
 * lconv`, which is all number formatting (Lua's lua_number2str) reads. Forward to
 * the host's "C"-locale lconv. */
uint32_t aret_localeconv(uint32_t esp) { (void)esp; return RP(localeconv()); }

/* _onexit(func): register an at-exit callback (a transpiled `sub_`, not a native
 * function pointer), returning `func` on success. We do not run these at exit, so
 * accept and return it — matching the documented startup-glue limitation that C++
 * global dtors are not executed. */
/* _onexit(func): register an exit callback (mingw's static atexit routes here),
 * returning the function pointer on success. */
uint32_t aret_onexit(uint32_t esp) { aret_register_atexit(AU(0)); return AU(0); }

/* _encode_pointer/_decode_pointer (MSVC security): obfuscate a pointer with a per-process
 * cookie so a stored function pointer can't be trivially overwritten; DecodePointer reverses
 * it. Only the round-trip matters to the program (encode on store, decode on use), so an
 * identity passthrough is exact and sound (no weak-stub garbage). */
uint32_t aret_encode_pointer(uint32_t esp) { return AU(0); }
uint32_t aret_decode_pointer(uint32_t esp) { return AU(0); }
/* (kernel32 EncodePointer/DecodePointer already live in aret_hle.c, same passthrough.) */

/* __dllonexit(func, pbegin, pend): the DLL form of _onexit — register an at-exit callback and
 * return it. Route to the same atexit table as _onexit (the callback list bookkeeping in
 * pbegin/pend is the CRT's; we own the registration). */
uint32_t aret_dllonexit(uint32_t esp) { aret_register_atexit(AU(0)); return AU(0); }

/* ------------------------------------------------------------------ */
/* <math.h> — the transcendental libm functions. Their statically-linked */
/* bodies are dense x87 (and don't model), so bind them to the host libm.*/
/* Args are cdecl `double`s on the machine stack ([esp], [esp+8]); the    */
/* result is returned in st(0), which our caller recovers from the x87    */
/* return channel (__aret_x87_ret, see aret_hle.c / FLOAT_HELPERS).       */
/* ------------------------------------------------------------------ */
extern long double __aret_x87_ret;
#define AD(i)  (*(double *)(uintptr_t)(esp + (unsigned)(i) * 8))
#define MATH1(name, fn) uint32_t aret_##name(uint32_t esp) { __aret_x87_ret = fn(AD(0)); __aret_x87_ret_valid = 1; return 0; }
#define MATH2(name, fn) uint32_t aret_##name(uint32_t esp) { __aret_x87_ret = fn(AD(0), AD(1)); __aret_x87_ret_valid = 1; return 0; }
MATH2(pow, pow)
MATH1(exp, exp)
MATH1(log, log)
MATH1(log10, log10)
MATH1(log2, log2)
MATH1(sin, sin)
MATH1(cos, cos)
MATH1(tan, tan)
MATH1(asin, asin)
MATH1(acos, acos)
MATH1(atan, atan)
MATH2(atan2, atan2)
MATH1(sinh, sinh)
MATH1(cosh, cosh)
MATH1(tanh, tanh)
MATH2(fmod, fmod)
MATH2(hypot, hypot)
MATH1(cbrt, cbrt)
MATH1(exp2, exp2)
MATH1(expm1, expm1)
MATH1(log1p, log1p)
MATH1(floor, floor)   /* comctl32 socle (fp-returning: recovered via the x87 channel) */
#undef MATH1
#undef MATH2
#undef AD

/* ------------------------------------------------------------------ */
/* <ctype.h> — classification / case (often called, not inlined)      */
/* ------------------------------------------------------------------ */

#define CTYPE1(name, fn) uint32_t aret_##name(uint32_t esp) { return (uint32_t)fn(AI(0)); }
CTYPE1(toupper, toupper)
CTYPE1(tolower, tolower)
CTYPE1(isalpha, isalpha)
CTYPE1(isdigit, isdigit)
CTYPE1(isalnum, isalnum)
CTYPE1(isspace, isspace)
CTYPE1(isupper, isupper)
CTYPE1(islower, islower)
CTYPE1(ispunct, ispunct)
CTYPE1(iscntrl, iscntrl)
CTYPE1(isprint, isprint)
CTYPE1(isgraph, isgraph)
CTYPE1(isxdigit, isxdigit)
#undef CTYPE1

/* msvcrt string case-fold in place, returning the same buffer. _strlwr/_strupr
 * are locale-sensitive on Windows; in the C locale (the transpiled default) they
 * fold ASCII A-Z / a-z, exactly what tolower/toupper give. `str*` and `_str*`
 * both sanitize to these (leading underscore stripped). */
uint32_t aret_strlwr(uint32_t esp) { char *s = AS(0); if (s) for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return RP(s); }
uint32_t aret_strupr(uint32_t esp) { char *s = AS(0); if (s) for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p); return RP(s); }

/* msvcrt _umask sets the file-creation permission mask and returns the previous
 * one. ARET's file shims create through the host, so applying the host umask is
 * the consistent behavior (Set-then-restore round-trips, Set returns the old
 * mask). msvcrt's _S_IWRITE (0x80) / _S_IREAD (0x100) mask bits coincide with the
 * POSIX owner bits S_IWUSR (0200) / S_IRUSR (0400), so the value maps straight. */
uint32_t aret_umask(uint32_t esp) { return (uint32_t)umask((mode_t)AU(0)); }

/* ------------------------------------------------------------------ */
/* <stdlib.h> — qsort / bsearch (callback into transpiled code)       */
/* ------------------------------------------------------------------ */
/* The comparator is a transpiled `sub_<va>` using the machine-stack ABI, not a
 * native cdecl function, so it cannot be handed to libc directly. Forward to the
 * real qsort/bsearch with a trampoline that invokes the guest comparator through
 * aret_call, laying a cdecl frame ([esp+4]=a, [esp+8]=b) on a scratch stack with
 * room below for the callee's own frame. Not reentrant — one active sort/search
 * at a time, which qsort/bsearch satisfy (they do not nest the comparator). */
static uint32_t aret_cmp_va;
static int aret_cmp_tramp(const void *a, const void *b) {
    static uint8_t scratch[64 * 1024];
    uint32_t *f = (uint32_t *)(void *)(scratch + sizeof(scratch) - 64);
    f[1] = (uint32_t)(uintptr_t)a; /* arg0 @ [esp+4] */
    f[2] = (uint32_t)(uintptr_t)b; /* arg1 @ [esp+8] */
    return (int)(int32_t)aret_call(aret_cmp_va, (uint32_t)(uintptr_t)f, 0, 0, 0, 0, 0, 0, 0);
}
uint32_t aret_qsort(uint32_t esp) {
    aret_cmp_va = AU(3);
    qsort(AP(0), AU(1), AU(2), aret_cmp_tramp);
    return 0;
}
uint32_t aret_bsearch(uint32_t esp) {
    aret_cmp_va = AU(4);
    return (uint32_t)(uintptr_t)bsearch(AP(0), AP(1), AU(2), AU(3), aret_cmp_tramp);
}

/* <wchar.h> — Windows wchar_t is 16-bit (host's is 32-bit), so count 16-bit
 * units directly rather than forwarding to host wcslen. */
uint32_t aret_wcslen(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
uint32_t aret_wcscpy(uint32_t esp) {
    uint16_t *d = (uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint16_t *r = d;
    while ((*d++ = *s++)) {}
    return (uint32_t)(uintptr_t)r;
}
uint32_t aret_wcscat(uint32_t esp) {
    uint16_t *d = (uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint16_t *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return (uint32_t)(uintptr_t)r;
}
/* wcscat_s(dest, destsz, src) -> errno_t. Annex K / MSVC secure append, with the
 * exact msvcrt behaviour MEASURED against Wine (7 cases, `winecorpus/crt_wcscat_s.c`):
 *   - destsz == 0 or dest == NULL      -> EINVAL(22), destination left UNTOUCHED;
 *   - src == NULL                      -> EINVAL(22), dest[0] = 0;
 *   - no terminator within destsz      -> ERANGE(34), dest[0] = 0;
 *   - result would not fit             -> ERANGE(34), dest[0] = 0 — note the copy has
 *     already written as much as fitted, so those elements ARE clobbered (observable,
 *     and reproduced here rather than idealised away);
 *   - otherwise append and return 0 (an exact fit, NUL landing on the last element,
 *     succeeds).
 * Windows wchar_t is 16-bit, so this walks uint16_t units (as the wcs* shims above). */
uint32_t aret_wcscat_s(uint32_t esp) {
    uint16_t *d = (uint16_t *)(uintptr_t)a32(esp, 0);
    uint32_t size = a32(esp, 1);
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 2);
    if (!d || size == 0) return 22;            /* EINVAL — destination untouched */
    if (!s) { d[0] = 0; return 22; }           /* EINVAL — destination emptied   */
    uint32_t i = 0;
    while (i < size && d[i]) i++;
    if (i == size) { d[0] = 0; return 34; }    /* ERANGE — dest not terminated    */
    for (;;) {
        if (i == size) { d[0] = 0; return 34; } /* ERANGE — no room for the NUL   */
        d[i] = *s;
        if (!*s) return 0;
        i++; s++;
    }
}

/* _strlwr_s / _strupr_s / _wcslwr_s / _wcsupr_s — in-place case conversion with a
 * buffer-size check. Behaviour MEASURED against Wine; three details are not guessable:
 *   - a size too small for the string + its NUL: EINVAL **and** str[0] set to 0
 *     (emptied), while the REST of the buffer keeps its previous contents;
 *   - the failure code is 22 (EINVAL) in every failing case, including too-small;
 *   - ⚠️ size == 0 does NOT behave the same at both widths: the narrow pair leaves the
 *     buffer completely untouched, the wide pair still zeroes d[0]. Measured on all
 *     four — it is a width difference, not a lower/upper one, and no amount of
 *     reasoning would have produced it.
 * Folding is ASCII-only, exact in the C locale — same footing as _wcsicmp (70 §4.5);
 * ⚠️ cf. §P1bis: setlocale accepts any locale and answers "C", so nothing yet proves we
 * are in it. */
#define ARET_LWR_S_BODY(TYPE, LO, HI, DELTA, ZERO_ON_SIZE0)                        \
    TYPE *d = (TYPE *)(uintptr_t)a32(esp, 0);                                      \
    uint32_t size = a32(esp, 1);                                                   \
    if (!d) return 22;                                                             \
    if (size == 0) { if (ZERO_ON_SIZE0) d[0] = 0; return 22; }                     \
    uint32_t n = 0;                                                                \
    while (n < size && d[n]) n++;                                                  \
    if (n == size) { d[0] = 0; return 22; }      /* EINVAL — emptied, rest kept */ \
    for (uint32_t i = 0; i < n; i++)                                               \
        if (d[i] >= (LO) && d[i] <= (HI)) d[i] = (TYPE)(d[i] + (DELTA));           \
    return 0;

uint32_t aret_strlwr_s(uint32_t esp) { ARET_LWR_S_BODY(uint8_t,  'A', 'Z',  32, 0) }
uint32_t aret_strupr_s(uint32_t esp) { ARET_LWR_S_BODY(uint8_t,  'a', 'z', -32, 0) }
uint32_t aret_wcslwr_s(uint32_t esp) { ARET_LWR_S_BODY(uint16_t, 'A', 'Z',  32, 1) }
uint32_t aret_wcsupr_s(uint32_t esp) { ARET_LWR_S_BODY(uint16_t, 'a', 'z', -32, 1) }
#undef ARET_LWR_S_BODY

/* strcpy_s / wcscpy_s — bounded copy. MEASURED against Wine's msvcrt (the statically
 * linked mingw version was checked to agree, and the exe does import msvcrt's, ordinal
 * 1084 — worth confirming, since mingw supplies its own body for some secure-CRT names
 * and one would then be measuring mingw rather than the oracle).
 *   - dst NULL, or size 0: EINVAL(22), destination untouched;
 *   - src NULL: EINVAL(22), destination EMPTIED (d[0] = 0);
 *   - too small for src + NUL: ERANGE(34) — note 34 here, where the _strlwr_s family
 *     answers 22 for its own too-small case; the codes are per-family, not global;
 *   - ⚠️ and the too-small case is NOT symmetric across widths: the narrow version has
 *     already copied the characters that fit before emptying (`00 69 58 65 44` for
 *     size 5), while the wide version copies NOTHING and only zeroes d[0]. Measured on
 *     both; this is the second width asymmetry in this area, after size==0 in the
 *     _strlwr_s family. */
uint32_t aret_strcpy_s(uint32_t esp) {
    uint8_t *d = (uint8_t *)(uintptr_t)a32(esp, 0);
    uint32_t size = a32(esp, 1);
    const uint8_t *s = (const uint8_t *)(uintptr_t)a32(esp, 2);
    if (!d || size == 0) return 22;
    if (!s) { d[0] = 0; return 22; }
    for (uint32_t i = 0; i < size; i++) {
        d[i] = s[i];
        if (!s[i]) return 0;
    }
    d[0] = 0;                    /* copied what fitted, then emptied — measured */
    return 34;
}

uint32_t aret_wcscpy_s(uint32_t esp) {
    uint16_t *d = (uint16_t *)(uintptr_t)a32(esp, 0);
    uint32_t size = a32(esp, 1);
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 2);
    if (!d || size == 0) return 22;
    if (!s) { d[0] = 0; return 22; }
    uint32_t n = 0;
    while (s[n]) n++;
    if (n + 1 > size) { d[0] = 0; return 34; }   /* nothing copied — measured */
    for (uint32_t i = 0; i <= n; i++) d[i] = s[i];
    return 0;
}

/* strncpy_s / wcsncpy_s (dest, size, src, count) — bounded copy of at most `count`
 * characters. MEASURED against Wine at BOTH widths, which mattered: the two previous
 * secure-CRT families each had a narrow/wide asymmetry, and this one has NONE. So
 * "the widths differ" is not a rule either — each family has to be measured.
 *   - size 0 (or dest NULL): EINVAL(22), destination untouched;
 *   - src NULL: EINVAL(22), destination emptied;
 *   - count 0: SUCCESS(0) with the destination emptied — not an error;
 *   - count >= strlen(src): stops at the source NUL, no error;
 *   - dest too small: ERANGE(34), having copied what fitted, then d[0] = 0;
 *   - count == (size_t)-1 (_TRUNCATE): truncates to size-1 and KEEPS the result,
 *     returning 80 (STRUNCATE) — a distinct code, and the only failing case whose
 *     output is meant to be used. */
#define ARET_NCPY_S_BODY(TYPE)                                                     \
    TYPE *d = (TYPE *)(uintptr_t)a32(esp, 0);                                      \
    uint32_t size = a32(esp, 1);                                                   \
    const TYPE *s = (const TYPE *)(uintptr_t)a32(esp, 2);                          \
    uint32_t count = a32(esp, 3);                                                  \
    if (!d || size == 0) return 22;                                                \
    if (!s) { d[0] = 0; return 22; }                                               \
    int trunc = (count == 0xffffffffu);                                            \
    uint32_t n = 0;                                                                \
    while (s[n] && (trunc || n < count)) n++;      /* chars the source can give */ \
    if (n + 1 <= size) {                                                           \
        for (uint32_t i = 0; i < n; i++) d[i] = s[i];                              \
        d[n] = 0;                                                                  \
        return 0;                                                                  \
    }                                                                              \
    if (trunc) {                                   /* keep what fits, report 80 */ \
        for (uint32_t i = 0; i + 1 < size; i++) d[i] = s[i];                       \
        d[size - 1] = 0;                                                           \
        return 80;                                                                 \
    }                                                                              \
    for (uint32_t i = 0; i < size && i < n; i++) d[i] = s[i];                      \
    d[0] = 0;                                      /* copied, then emptied      */ \
    return 34;

uint32_t aret_strncpy_s(uint32_t esp) { ARET_NCPY_S_BODY(uint8_t) }
uint32_t aret_wcsncpy_s(uint32_t esp) { ARET_NCPY_S_BODY(uint16_t) }
#undef ARET_NCPY_S_BODY

/* _isctype(c, mask) -> the character's ctype bits ANDed with `mask` (NOT a boolean:
 * `_isctype('5', 0xffff)` answers 0x84 = _DIGIT|_HEX). Table MEASURED against Wine by
 * sweeping all 256 codes against every mask, then the return shape confirmed
 * separately — a boolean implementation would satisfy every `if (_isctype(...))` caller
 * and quietly break any that uses the value.
 * Pure ASCII, C locale. Two measured details worth keeping: _BLANK(0x40) covers ONLY
 * the space, not the tab (unlike C's isblank), and the _ALPHA 0x100 bit is not stored
 * in the table at all — `_isctype('A', 0x103)` answers 0x1 because 0x103 contains
 * _UPPER, not because a 0x100 bit matched. Anything outside 0..255 (EOF included)
 * answers 0. */
static uint16_t aret_ctype_bits(int c) {
    if (c < 0 || c > 255) return 0;
    uint16_t b = 0;
    if (c >= 'A' && c <= 'Z') b |= 0x01;                        /* _UPPER   */
    if (c >= 'a' && c <= 'z') b |= 0x02;                        /* _LOWER   */
    if (c >= '0' && c <= '9') b |= 0x04;                        /* _DIGIT   */
    if ((c >= 9 && c <= 13) || c == 32) b |= 0x08;              /* _SPACE   */
    if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
        (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) b |= 0x10; /* _PUNCT */
    if (c < 32 || c == 127) b |= 0x20;                          /* _CONTROL */
    if (c == 32) b |= 0x40;                                     /* _BLANK: space only */
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
        (c >= 'a' && c <= 'f')) b |= 0x80;                      /* _HEX     */
    return b;
}
uint32_t aret_isctype(uint32_t esp) {
    int c = (int)a32(esp, 0);
    uint32_t mask = a32(esp, 1);
    return (uint32_t)(aret_ctype_bits(c) & mask);
}

/* strnlen / wcsnlen(s, maxlen) -> min(length, maxlen). MEASURED against Wine: the
 * length when the NUL is inside the window, and maxlen when it is not — WITHOUT
 * reading past maxlen, which is the entire point of the bounded form and the one
 * thing a strlen-then-clamp implementation would get wrong (it would run off the end
 * of a buffer that has no terminator, exactly the case these exist to make safe).
 * maxlen 0 answers 0 and touches nothing, including for a NULL pointer. */
#define ARET_NLEN_BODY(TYPE)                                                   \
    const TYPE *s = (const TYPE *)(uintptr_t)a32(esp, 0);                      \
    uint32_t maxlen = a32(esp, 1);                                             \
    if (!s || maxlen == 0) return 0;                                           \
    uint32_t n = 0;                                                            \
    while (n < maxlen && s[n]) n++;                                            \
    return n;

uint32_t aret_strnlen(uint32_t esp) { ARET_NLEN_BODY(uint8_t) }
uint32_t aret_wcsnlen(uint32_t esp) { ARET_NLEN_BODY(uint16_t) }
#undef ARET_NLEN_BODY

uint32_t aret_wcscmp(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *b = (const uint16_t *)(uintptr_t)a32(esp, 1);
    while (*a && *a == *b) { a++; b++; }
    return (uint32_t)(int32_t)((int)*a - (int)*b);
}
uint32_t aret_wcsdup(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    uint32_t n = 0;
    while (s[n]) n++;
    uint16_t *d = (uint16_t *)malloc((size_t)(n + 1) * 2);
    if (!d) return 0;
    for (uint32_t i = 0; i <= n; i++) d[i] = s[i];
    return (uint32_t)(uintptr_t)d;
}
/* Case fold for a 16-bit code unit. In the C/default locale (which this runtime
 * fixes) msvcrt's towlower/_wcsicmp fold ONLY ASCII A-Z — so ASCII folding is the
 * exact behaviour, not an approximation. Non-ASCII units pass through unchanged. */
static uint16_t w_lower(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }
uint32_t aret_towlower(uint32_t esp) { uint16_t c = (uint16_t)a32(esp, 0); return w_lower(c); }
uint32_t aret_towupper(uint32_t esp) {
    uint16_t c = (uint16_t)a32(esp, 0);
    return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c;
}
uint32_t aret_wcsncmp(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *b = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint32_t n = a32(esp, 2);
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint32_t)(int32_t)((int)a[i] - (int)b[i]);
        if (!a[i]) break;
    }
    return 0;
}
/* _wcsicmp / _wcsnicmp — case-insensitive (ASCII fold, exact for the C locale). */
uint32_t aret_wcsicmp(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *b = (const uint16_t *)(uintptr_t)a32(esp, 1);
    while (*a && w_lower(*a) == w_lower(*b)) { a++; b++; }
    return (uint32_t)(int32_t)((int)w_lower(*a) - (int)w_lower(*b));
}
uint32_t aret_wcsnicmp(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *b = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint32_t n = a32(esp, 2);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = w_lower(a[i]), y = w_lower(b[i]);
        if (x != y) return (uint32_t)(int32_t)((int)x - (int)y);
        if (!a[i]) break;
    }
    return 0;
}
/* _wcsicoll / wcscoll — the LOCALE-collating wide compares. MEASURED against Wine's
 * msvcrt in the default ("C") locale, which is the locale a program has before it
 * calls setlocale: they are plain ORDINAL compares, identical to _wcsicmp / wcscmp
 * respectively (`winecorpus/crt_wcscoll.c`).
 *
 * This is worth stating because the intuitive wiring is wrong: `coll` suggests the
 * linguistic sort-key machinery behind lstrcmpiW/CompareStringW, and routing them
 * there would DIVERGE on exactly the cases that machinery exists for — measured
 * `_wcsicoll("readme","read-me") = +1` where `lstrcmpiW` gives -1, likewise "~" vs
 * "a" and "O'Brien" vs "OBrien".
 *
 * Caveat, stated honestly: under a REAL non-C locale these would collate
 * differently, and `aret_setlocale` currently reports "C" for every request instead
 * of refusing the ones it cannot model — so a program that selects e.g. a French
 * locale and then collates would silently get C-locale ordering. That is a
 * pre-existing gap in setlocale, not one introduced here; it is recorded in doc 70
 * §5 rather than papered over. */
uint32_t aret_wcsicoll(uint32_t esp) { return aret_wcsicmp(esp); }
uint32_t aret_wcscoll(uint32_t esp) { return aret_wcscmp(esp); }

/* ---- gettext (libintl) — measured post-lift wall (doc 90: libintl_gettext 78 bins,
 * bindtextdomain 70, textdomain 63, dgettext 44…).
 *
 * CONTRACT (§0, "correct or abort — never a silent false): gettext returns the msgid
 * UNCHANGED only when we can PROVE no translation would occur — the C/POSIX locale (no
 * translations by definition) OR no real .mo catalog present at the resolved path. That
 * is gettext's own specified result, not a guess. If a REAL applicable catalog exists
 * (a non-C locale with the .mo file gettext would load), we do NOT model message
 * translation, so we ABORT LOUDLY rather than silently return the untranslated string
 * as if it were the translation (e.g. "hello" where the truth is "bonjour").
 *   Root-cause note (b): the residual way to reach a translating locale at all is the
 * documented setlocale gap P1bis (setlocale reports "C" for every locale instead of
 * aborting on ones it cannot model). Fixing P1bis makes gettext identity provably safe
 * across the whole locale family; tracked in doc 70 §5 P1bis.
 * The `libintl_*` names are the actual mingw exports (`gettext(x)` => `libintl_gettext`).
 * All cdecl (no @N pop). */
static char g_td_domain[256] = "messages";
static char g_td_dir[1024] = "";
/* Does a real .mo catalog that gettext WOULD load exist for `domain`? (=> translation
 * would happen and identity would be WRONG). 0 when the locale is C/POSIX or no matching
 * .mo is on disk — the cases where gettext itself returns the msgid. Mirrors gettext's
 * path resolution <dir>/<locale>/LC_MESSAGES/<domain>.mo with the standard locale
 * fallbacks (strip @modifier, .codeset, _TERRITORY). */
static int aret_mo_on_disk(const char *dir, const char *loc, const char *domain) {
    char path[2600]; struct stat st;
    snprintf(path, sizeof path, "%s/%s/LC_MESSAGES/%s.mo", dir, loc, domain);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
static int aret_gettext_would_translate(const char *domain) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || !*l) return 0;                                            /* no locale -> C -> no translation */
    if (!strcmp(l, "C") || !strcmp(l, "POSIX") ||
        !strncmp(l, "C.", 2) || !strncmp(l, "POSIX.", 6)) return 0;     /* C/POSIX(.codeset) -> no translation */
    if (!domain || !*domain) domain = g_td_domain;
    char hostdir[2048];
    if (g_td_dir[0]) translate_path(g_td_dir, hostdir, sizeof hostdir); /* bound dir (Windows->host) */
    else strcpy(hostdir, "/usr/share/locale");                         /* libintl's default search root */
    /* Try the locale as-is, then progressively normalized (matches gettext's fallbacks). */
    char cand[256];
    for (int form = 0; form < 4; form++) {
        strncpy(cand, l, sizeof cand - 1); cand[sizeof cand - 1] = 0;
        char *p;
        if (form >= 1 && (p = strchr(cand, '@'))) *p = 0;              /* drop @modifier */
        if (form >= 2 && (p = strchr(cand, '.'))) *p = 0;              /* drop .codeset */
        if (form >= 3 && (p = strchr(cand, '_'))) *p = 0;              /* drop _TERRITORY */
        if (aret_mo_on_disk(hostdir, cand, domain)) return 1;
    }
    return 0;
}
static void aret_gettext_guard(const char *domain) {
    if (aret_gettext_would_translate(domain))
        aret_unmodelled("gettext: a real .mo catalog applies for this locale/domain; "
                        "message translation is not modelled (returning the untranslated "
                        "msgid would be a silent false) — see doc 70 P1bis");
}
uint32_t aret_libintl_gettext(uint32_t esp)   { aret_gettext_guard(NULL);   return AU(0); }         /* (msgid) */
uint32_t aret_libintl_dgettext(uint32_t esp)  { aret_gettext_guard(ACS(0)); return AU(1); }         /* (domain, msgid) */
uint32_t aret_libintl_dcgettext(uint32_t esp) { aret_gettext_guard(ACS(0)); return AU(1); }         /* (domain, msgid, category) */
uint32_t aret_libintl_ngettext(uint32_t esp)  { aret_gettext_guard(NULL);   return AU(2) == 1 ? AU(0) : AU(1); }
uint32_t aret_libintl_dngettext(uint32_t esp) { aret_gettext_guard(ACS(0)); return AU(3) == 1 ? AU(1) : AU(2); }
uint32_t aret_libintl_dcngettext(uint32_t esp){ aret_gettext_guard(ACS(0)); return AU(3) == 1 ? AU(1) : AU(2); }
/* textdomain/bindtextdomain return a pointer the caller may read (not free); we keep
 * the current values in static buffers. Default domain "messages", as libintl. These
 * only record binding state (no translation) -> no guard needed. */
uint32_t aret_libintl_textdomain(uint32_t esp) {
    const char *d = ACS(0);
    if (d) { strncpy(g_td_domain, d, sizeof g_td_domain - 1); g_td_domain[sizeof g_td_domain - 1] = 0; }
    return RP(g_td_domain);
}
uint32_t aret_libintl_bindtextdomain(uint32_t esp) {
    const char *dir = ACS(1);
    if (dir) { strncpy(g_td_dir, dir, sizeof g_td_dir - 1); g_td_dir[sizeof g_td_dir - 1] = 0; return RP(g_td_dir); }
    return g_td_dir[0] ? RP(g_td_dir) : 0;              /* query with NULL: current binding, else NULL */
}
uint32_t aret_libintl_bind_textdomain_codeset(uint32_t esp) { return AU(1); }        /* (domain, codeset): identity (no transcode) */
/* libintl wrappers over CRT functions -> route to the existing shims. setlocale
 * shares the C-locale limitation documented at aret_wcscoll / doc 70 §5 (P1bis). */
uint32_t aret_setlocale(uint32_t esp);
uint32_t aret_fprintf(uint32_t esp);
uint32_t aret_free(uint32_t esp);
uint32_t aret_libintl_setlocale(uint32_t esp) { return aret_setlocale(esp); }
uint32_t aret_libintl_fprintf(uint32_t esp)   { return aret_fprintf(esp); }
uint32_t aret_libintl_free(uint32_t esp)      { return aret_free(esp); }

uint32_t aret_wcschr(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    uint16_t c = (uint16_t)a32(esp, 1);
    for (;; s++) { if (*s == c) return (uint32_t)(uintptr_t)s; if (!*s) return 0; }
}
uint32_t aret_wcsrchr(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    uint16_t c = (uint16_t)a32(esp, 1);
    const uint16_t *last = 0;
    for (;; s++) { if (*s == c) last = s; if (!*s) break; }
    return (uint32_t)(uintptr_t)last;
}
uint32_t aret_wcsncpy(uint32_t esp) {
    uint16_t *d = (uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint32_t n = a32(esp, 2), i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;                 /* pad with NULs, no forced terminator */
    return a32(esp, 0);
}
uint32_t aret_wcsstr(uint32_t esp) {
    const uint16_t *h = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *n = (const uint16_t *)(uintptr_t)a32(esp, 1);
    if (!n[0]) return (uint32_t)(uintptr_t)h;
    for (; *h; h++) {
        uint32_t i = 0;
        while (n[i] && h[i] == n[i]) i++;
        if (!n[i]) return (uint32_t)(uintptr_t)h;
    }
    return 0;
}
/* Wide scanning trio (`<wchar.h>`, 16-bit code units, ordinal like msvcrt). Same
 * standard semantics as their narrow twins: `wcspbrk` returns a pointer to the first
 * char of `s` that occurs in `accept` (NULL if none); `wcsspn`/`wcscspn` return the
 * length of the initial run of `s` made only of chars that are in / not in `set`. */
static int u32_wcs_in(uint16_t c, const uint16_t *set) {
    for (; *set; set++) if (*set == c) return 1;
    return 0;
}
uint32_t aret_wcspbrk(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 1);
    for (; *s; s++) if (u32_wcs_in(*s, a)) return (uint32_t)(uintptr_t)s;
    return 0;
}
uint32_t aret_wcsspn(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *a = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint32_t n = 0;
    while (s[n] && u32_wcs_in(s[n], a)) n++;
    return n;
}
uint32_t aret_wcscspn(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    const uint16_t *r = (const uint16_t *)(uintptr_t)a32(esp, 1);
    uint32_t n = 0;
    while (s[n] && !u32_wcs_in(s[n], r)) n++;
    return n;
}
/* Wide numeric parse (wcstol/wcstoul): copy the low byte of each 16-bit unit into a
 * narrow buffer (numeric text is ASCII), parse with the host strtol/strtoul, then map
 * the end pointer back to the original wide string (1 code unit == 1 narrow char). */
static uint32_t aret_wcsto_impl(uint32_t esp, int sign) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)a32(esp, 0);
    uint32_t pend = a32(esp, 1); int base = AI(2);
    char nb[128]; int i = 0;
    if (s) for (; s[i] && i < 127; i++) nb[i] = (char)(uint8_t)s[i];
    nb[i] = 0;
    char *end; unsigned long v = sign ? (unsigned long)strtol(nb, &end, base) : strtoul(nb, &end, base);
    if (pend) *(uint32_t *)(uintptr_t)pend = a32(esp, 0) + (uint32_t)(end - nb) * 2u;
    return (uint32_t)v;
}
uint32_t aret_wcstol(uint32_t esp)  { return aret_wcsto_impl(esp, 1); }
uint32_t aret_wcstoul(uint32_t esp) { return aret_wcsto_impl(esp, 0); }

/* ------------------------------------------------------------------ */
/* <time.h> calendar — struct tm marshalling (host/guest ABI)         */
/* ------------------------------------------------------------------ */
/* msvcrt `struct tm` is 9 ints {sec,min,hour,mday,mon,year,wday,yday,isdst} —
 * the same first 9 fields, in order, as the host's struct tm (which only adds
 * extra trailing members). So gmtime/localtime return a pointer to a 9-int guest
 * buffer filled from the host result, and mktime/strftime read that layout back
 * into a host struct tm. The static buffer matches msvcrt (whose gmtime/localtime
 * also return a pointer into per-thread static storage). */
static int32_t aret_tm[9];
static uint32_t aret_pack_tm(const struct tm *t) {
    if (!t) return 0;
    aret_tm[0] = t->tm_sec;  aret_tm[1] = t->tm_min;   aret_tm[2] = t->tm_hour;
    aret_tm[3] = t->tm_mday; aret_tm[4] = t->tm_mon;   aret_tm[5] = t->tm_year;
    aret_tm[6] = t->tm_wday; aret_tm[7] = t->tm_yday;  aret_tm[8] = t->tm_isdst;
    return (uint32_t)(uintptr_t)aret_tm;
}
static struct tm aret_unpack_tm(const int32_t *g) {
    struct tm t; memset(&t, 0, sizeof t);
    if (g) {
        t.tm_sec = g[0];  t.tm_min = g[1];   t.tm_hour = g[2];
        t.tm_mday = g[3]; t.tm_mon = g[4];   t.tm_year = g[5];
        t.tm_wday = g[6]; t.tm_yday = g[7];  t.tm_isdst = g[8];
    }
    return t;
}
uint32_t aret_gmtime(uint32_t esp) {
    const int32_t *p = (const int32_t *)AP(0);   /* const time_t* (32-bit) */
    time_t tt = p ? (time_t)*p : 0;
    return aret_pack_tm(gmtime(&tt));
}
uint32_t aret_localtime(uint32_t esp) {
    const int32_t *p = (const int32_t *)AP(0);
    time_t tt = p ? (time_t)*p : 0;
    return aret_pack_tm(localtime(&tt));
}
uint32_t aret_mktime(uint32_t esp) {
    struct tm t = aret_unpack_tm((const int32_t *)AP(0));
    return (uint32_t)(int32_t)mktime(&t);
}
uint32_t aret_strftime(uint32_t esp) {
    char *s = AS(0); size_t max = AU(1); const char *fmt = ACS(2);
    struct tm t = aret_unpack_tm((const int32_t *)AP(3));
    return (uint32_t)strftime(s, max, fmt ? fmt : "", &t);
}
/* ---- locale collation + wide time (measured OS wall, doc 82) --------------
 * With no locale set (ARET's model, and msvcrt's default "C"), strxfrm/wcsxfrm
 * transform IDENTICALLY: the required contract is strcoll(a,b) == strcmp of the
 * transforms, which the identity copy satisfies. Bounded copy into dst, return
 * the source length (excl. NUL) — exactly msvcrt's C-locale result. */
uint32_t aret_strxfrm(uint32_t esp) {
    char *d = AS(0); const char *s = ACS(1); size_t n = AU(2);
    size_t len = s ? strlen(s) : 0;
    if (d && n) { size_t c = len < n ? len : n - 1; if (s) memcpy(d, s, c); d[c] = 0; }
    return (uint32_t)len;
}
uint32_t aret_wcsxfrm(uint32_t esp) {
    uint16_t *d = (uint16_t *)AP(0); const uint16_t *s = (const uint16_t *)AP(1); size_t n = AU(2);
    size_t len = 0; if (s) while (s[len]) len++;
    if (d && n) { size_t c = len < n ? len : n - 1; for (size_t i = 0; i < c; i++) d[i] = s[i]; d[c] = 0; }
    return (uint32_t)len;
}
/* _wcsftime(wdst, maxcount, wfmt, tm): narrow the format, host strftime, widen the
 * result. C locale => deterministic and bit-identical to Wine's msvcrt. Returns the
 * wchar count (excl. NUL), or 0 if it did not fit (msvcrt contract). */
uint32_t aret_wcsftime(uint32_t esp) {
    uint16_t *wd = (uint16_t *)AP(0); size_t max = AU(1);
    const uint16_t *wf = (const uint16_t *)AP(2);
    struct tm t = aret_unpack_tm((const int32_t *)AP(3));
    char fmt[512]; size_t i = 0;
    if (wf) for (; wf[i] && i + 1 < sizeof fmt; i++) fmt[i] = (char)(wf[i] & 0xff);
    fmt[i] = 0;
    char out[2048];
    size_t r = strftime(out, sizeof out, fmt, &t);
    if (!wd || max == 0) return 0;
    if (r == 0 || r >= max) { wd[0] = 0; return 0; }
    for (size_t k = 0; k < r; k++) wd[k] = (uint16_t)(unsigned char)out[k];
    wd[r] = 0;
    return (uint32_t)r;
}

/* ---- small CRT leftovers (measured OS wall mop-up, doc 82) ---------------- */
/* _ultoa/_ltoa/_itoa(value, str, radix) -> str. Integer to string, radix 2..36. */
static char *aret_int2str(unsigned long v, char *s, int radix, int neg) {
    if (!s || radix < 2 || radix > 36) { if (s) s[0] = 0; return s; }
    char tmp[36]; int i = 0;
    do { unsigned d = (unsigned)(v % (unsigned)radix); tmp[i++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10); v /= (unsigned)radix; } while (v);
    char *p = s;
    if (neg) *p++ = '-';
    while (i) *p++ = tmp[--i];
    *p = 0;
    return s;
}
uint32_t aret_ultoa(uint32_t esp) { return RP(aret_int2str(AU(0), AS(1), AI(2), 0)); }
uint32_t aret_ltoa(uint32_t esp) {
    int32_t v = (int32_t)AU(0); int radix = AI(2); char *s = AS(1);
    if (radix == 10 && v < 0) return RP(aret_int2str((unsigned long)(-(int64_t)v), s, 10, 1));
    return RP(aret_int2str((unsigned long)(uint32_t)v, s, radix, 0));
}
uint32_t aret_itoa(uint32_t esp) { return aret_ltoa(esp); }
/* _p___mb_cur_max() -> int*: pointer to MB_CUR_MAX. In the C locale it is 1. */
uint32_t aret_p___mb_cur_max(uint32_t esp) { (void)esp; static int mb = 1; return RP(&mb); }
/* _aligned_malloc(size, alignment) / _aligned_free(ptr): over-allocate, align, and
 * stash the original malloc pointer just below the aligned block so free recovers it. */
uint32_t aret_aligned_malloc(uint32_t esp) {
    size_t size = AU(0), align = AU(1);
    if (align < sizeof(void *)) align = sizeof(void *);
    if (align & (align - 1)) return 0;                 /* alignment must be a power of two */
    void *raw = malloc(size + align + sizeof(void *));
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
    ((void **)a)[-1] = raw;
    return (uint32_t)a;
}
uint32_t aret_aligned_free(uint32_t esp) {
    void *p = (void *)(uintptr_t)AU(0);
    if (p) free(((void **)p)[-1]);
    return 0;
}
/* asctime(tm) -> "Www Mmm dd hh:mm:ss yyyy\n" in a static buffer. Formatted
 * explicitly (not via host asctime) because msvcrt zero-pads the day ("09")
 * while glibc space-pads it (" 9"). */
uint32_t aret_asctime(uint32_t esp) {
    static const char *const wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const mo[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec"};
    static char buf[32];
    const int32_t *g = (const int32_t *)AP(0);
    if (!g || g[6] < 0 || g[6] > 6 || g[4] < 0 || g[4] > 11) return 0;
    snprintf(buf, sizeof buf, "%s %s %02d %02d:%02d:%02d %d\n",
             wd[g[6]], mo[g[4]], g[3], g[2], g[1], g[0], g[5] + 1900);
    return (uint32_t)(uintptr_t)buf;
}
/* difftime returns a double — recovered through the x87 channel like libm. */
uint32_t aret_difftime(uint32_t esp) {
    __aret_x87_ret = difftime((time_t)(int32_t)AU(0), (time_t)(int32_t)AU(1)); __aret_x87_ret_valid = 1;
    return 0;
}
/* ctime(time_t*) = asctime(localtime(t)) -> "Www Mmm dd hh:mm:ss yyyy\n" in a static
 * buffer. Formatted explicitly (msvcrt zero-pads the day, glibc space-pads). localtime
 * is timezone-dependent but both engines read the same host TZ. */
static char *aret_fmt_tm(const struct tm *t, char *buf, size_t n) {
    static const char *const wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (!t || t->tm_wday < 0 || t->tm_wday > 6 || t->tm_mon < 0 || t->tm_mon > 11) return 0;
    snprintf(buf, n, "%s %s %02d %02d:%02d:%02d %d\n", wd[t->tm_wday], mo[t->tm_mon],
             t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, t->tm_year + 1900);
    return buf;
}
uint32_t aret_ctime(uint32_t esp) {
    const int32_t *p = (const int32_t *)AP(0);
    static char buf[32];
    if (!p) return 0;
    time_t tt = (time_t)*p;
    return aret_fmt_tm(localtime(&tt), buf, sizeof buf) ? RP(buf) : 0;
}
uint32_t aret_ctime32(uint32_t esp) { return aret_ctime(esp); }
uint32_t aret_ctime64(uint32_t esp) {
    const int64_t *p = (const int64_t *)AP(0);          /* __time64_t */
    static char buf[32];
    if (!p) return 0;
    time_t tt = (time_t)*p;
    return aret_fmt_tm(localtime(&tt), buf, sizeof buf) ? RP(buf) : 0;
}
/* _fpreset(): reinitialize the FP unit. ARET's x87 state is per-op / runtime-net
 * managed, with no persistent masked-exception state to clear -> a sound no-op. */
uint32_t aret_fpreset(uint32_t esp) { (void)esp; return 0; }
/* __fpecode(): pointer to the per-thread last FP-exception code. ARET does not raise
 * maskable FP exceptions (div/idiv #DE trap hard) -> the clean state is 0. */
uint32_t aret_fpecode(uint32_t esp) { (void)esp; static int code = 0; return RP(&code); }
/* __pxcptinfoptrs(): pointer to the per-thread pointer to CRT exception info. No
 * pending exception context in this model -> a valid slot holding NULL. */
uint32_t aret_pxcptinfoptrs(uint32_t esp) { (void)esp; static void *p = 0; return RP(&p); }

/* ---- CRT batch enabling the libglib lift (doc 82) --------------------------- */
/* isnan(double): the arg is a double on the cdecl stack (2 words). Returns nonzero
 * if NaN. (msvcrt exports `isnan`/`_isnan`.) */
uint32_t aret_isnan(uint32_t esp) {
    union { uint64_t u; double d; } v; v.u = (uint64_t)AU(0) | ((uint64_t)AU(1) << 32);
    return isnan(v.d) ? 1u : 0u;
}
uint32_t aret_finite(uint32_t esp) {
    union { uint64_t u; double d; } v; v.u = (uint64_t)AU(0) | ((uint64_t)AU(1) << 32);
    return isfinite(v.d) ? 1u : 0u;
}
/* _kbhit(): keyboard hit pending? Headless -> never (0). */
uint32_t aret_kbhit(uint32_t esp) { (void)esp; return 0; }
/* _getdrive(): current drive number (1=A:). We present C: (3), the modelled root. */
uint32_t aret_getdrive(uint32_t esp) { (void)esp; return 3; }
/* wctomb(char* s, wchar_t wc): C-locale single-byte encode. s==NULL -> 0 (no
 * state-dependent encoding). wc<256 fits one byte; above the C locale cannot encode -> -1. */
uint32_t aret_wctomb(uint32_t esp) {
    char *s = AS(0); uint32_t wc = AU(1);
    if (!s) return 0;
    if (wc < 256) { s[0] = (char)wc; return 1; }
    return (uint32_t)-1;
}
/* _ui64toa_s(unsigned __int64 v, char* buf, size_t size, int radix) -> 0 / errno.
 * v occupies two cdecl words. */
uint32_t aret_ui64toa_s(uint32_t esp) {
    uint64_t v = (uint64_t)AU(0) | ((uint64_t)AU(1) << 32);
    char *buf = AS(2); size_t size = AU(3); int radix = AI(4);
    if (!buf || size == 0 || radix < 2 || radix > 36) { if (buf && size) buf[0] = 0; return 22u; /* EINVAL */ }
    char tmp[65]; int i = 0;
    do { unsigned d = (unsigned)(v % (unsigned)radix); tmp[i++] = d < 10 ? (char)('0' + d) : (char)('a' + d - 10); v /= (unsigned)radix; } while (v);
    if ((size_t)i + 1 > size) { buf[0] = 0; return 34u; /* ERANGE */ }
    int o = 0; while (i) buf[o++] = tmp[--i];
    buf[o] = 0;
    return 0;
}
/* _wputenv(const wchar_t* "NAME=VALUE"): narrow it and putenv. */
uint32_t aret_wputenv(uint32_t esp) {
    const uint16_t *w = (const uint16_t *)AP(0);
    if (!w) return (uint32_t)-1;
    char buf[1024]; size_t i = 0;
    for (; w[i] && i + 1 < sizeof buf; i++) buf[i] = (char)(w[i] & 0xff);
    buf[i] = 0;
    static char *slots[64]; static int nslot;            /* putenv keeps the string; own it */
    char *s = (nslot < 64) ? (slots[nslot] = strdup(buf), slots[nslot++]) : strdup(buf);
    return (uint32_t)(s && putenv(s) == 0 ? 0 : -1);
}
/* _wgetenv(const wchar_t* name) -> wchar_t*: narrow the name, getenv, widen the value
 * into a static buffer (msvcrt returns a pointer into its own _wenviron cache, valid
 * until the next call — a single static buffer matches that contract). NULL if unset. */
uint32_t aret_wgetenv(uint32_t esp) {
    const uint16_t *wn = (const uint16_t *)AP(0);
    if (!wn) return 0;
    char name[512]; size_t i = 0;
    for (; wn[i] && i + 1 < sizeof name; i++) name[i] = (char)(wn[i] & 0xff);
    name[i] = 0;
    const char *v = getenv(name);
    if (!v) return 0;
    static uint16_t wbuf[4096]; size_t j = 0;
    for (; v[j] && j + 1 < sizeof wbuf / sizeof wbuf[0]; j++) wbuf[j] = (uint16_t)(unsigned char)v[j];
    wbuf[j] = 0;
    return (uint32_t)(uintptr_t)wbuf;
}
/* _set_error_mode(int mode) -> previous mode. CRT-level error reporting sink (distinct
 * from the Win32 SetErrorMode). _REPORT_ERRMODE(3) queries without changing. Tracked, so
 * a round-trip is faithful; the sink has no visible effect in the headless model. */
uint32_t aret_set_error_mode(uint32_t esp) {
    static int g_crt_error_mode = 1;                    /* _OUT_TO_STDERR (msvcrt default) */
    int mode = AI(0);
    int prev = g_crt_error_mode;
    if (mode != 3 /* _REPORT_ERRMODE */) g_crt_error_mode = mode;
    return (uint32_t)prev;
}
/* _wspawnvp/_wspawnvpe(mode, path, argv[, envp]): launch a child .exe. ARET is
 * single-process and cannot run a PE child (doc 70 §5.0) -> defined failure. */
uint32_t aret_wspawnvp(uint32_t esp)  { (void)esp; return (uint32_t)-1; }
uint32_t aret_wspawnvpe(uint32_t esp) { (void)esp; return (uint32_t)-1; }
/* libintl_sprintf -> the CRT sprintf. */
uint32_t aret_sprintf(uint32_t esp);
uint32_t aret_libintl_sprintf(uint32_t esp) { return aret_sprintf(esp); }
/* system(cmd): forward to the host shell (the program intends to run it). msvcrt returns
 * the command's EXIT CODE, not the raw wait-status, so extract it (WEXITSTATUS == (st>>8)
 * & 0xff on Linux; done inline to avoid a sys/wait.h dependency here). NULL command ->
 * non-zero ("a command processor is available"). */
uint32_t aret_system(uint32_t esp) {
    const char *cmd = ACS(0);
    if (!cmd) return 1;
    int st = system(cmd);
    return st < 0 ? (uint32_t)-1 : (uint32_t)((st >> 8) & 0xff);
}

/* _wsystem(cmd): the wide sibling of system(). ARET has no cmd.exe, so command
 * OUTPUT follows the host shell, but the returned EXIT CODE matches (both cmd and
 * sh return N for `exit N`). Narrow the 16-bit guest wide string to bytes (command
 * text is ASCII) then reuse the system() exit-code extraction. NULL -> non-zero. */
uint32_t aret_wsystem(uint32_t esp) {
    const uint16_t *w = (const uint16_t *)(uintptr_t)a32(esp, 0);
    if (!w) return 1;
    char buf[8192];
    size_t i = 0;
    for (; w[i] && i < sizeof buf - 1; i++) buf[i] = (char)(w[i] & 0xff);
    buf[i] = 0;
    int st = system(buf);
    return st < 0 ? (uint32_t)-1 : (uint32_t)((st >> 8) & 0xff);
}

/* _putenv_s(name, value) -> errno_t. Sets name=value in the environment; an EMPTY
 * value REMOVES the variable, exactly as msvcrt. NULL name/value -> EINVAL(22).
 * Maps onto host setenv/unsetenv (the environment is shared with getenv/_putenv). */
uint32_t aret_putenv_s(uint32_t esp) {
    const char *name = ACS(0), *val = ACS(1);
    if (!name || !val) { errno = 22; return 22u; }
    if (val[0] == '\0') { unsetenv(name); return 0u; }
    return setenv(name, val, 1) == 0 ? 0u : (uint32_t)errno;
}

/* _getch/_getche deliberately NOT shimmed: on Windows they read from the console
 * (CONIN$), ignoring stdin redirection — a semantics ARET has no console to honor.
 * Mapping them to stdin diverges from the Windows oracle (verified: Wine returns
 * console garbage on a redirected handle), so leave the weak stub's loud abort
 * rather than return bytes that no real console produced (§0). */

/* tmpnam([s]): a unique temporary name. msvcrt returns a bare name (used under
 * the temp dir); a monotonic counter suffices and is path-translated on open. */
uint32_t aret_tmpnam(uint32_t esp) {
    static unsigned ctr = 0;
    static char buf[64];
    char *out = AS(0);
    if (!out) out = buf;
    snprintf(out, 64, "aret_tmp_%u", ++ctr);
    return (uint32_t)(uintptr_t)out;
}
