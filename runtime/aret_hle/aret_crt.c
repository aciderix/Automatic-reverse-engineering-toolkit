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
    return (int)(int32_t)aret_call(aret_cmp_va, (uint32_t)(uintptr_t)f, 0, 0, 0, 0);
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
/* system(cmd): faithfully forward — the original program intends to run it. */
uint32_t aret_system(uint32_t esp) { return (uint32_t)system(ACS(0)); }

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
