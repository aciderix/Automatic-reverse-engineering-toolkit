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
extern long double __aret_x87_ret;
uint32_t aret_strtod(uint32_t esp) {
    char *end; double v = strtod(ACS(0), &end);
    if (AU(1)) *(uint32_t *)AP(1) = (uint32_t)(uintptr_t)end;
    __aret_x87_ret = v;
    return 0;
}
uint32_t aret_atof(uint32_t esp) { __aret_x87_ret = atof(ACS(0)); return 0; }

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

/* ------------------------------------------------------------------ */
/* <math.h> — the transcendental libm functions. Their statically-linked */
/* bodies are dense x87 (and don't model), so bind them to the host libm.*/
/* Args are cdecl `double`s on the machine stack ([esp], [esp+8]); the    */
/* result is returned in st(0), which our caller recovers from the x87    */
/* return channel (__aret_x87_ret, see aret_hle.c / FLOAT_HELPERS).       */
/* ------------------------------------------------------------------ */
extern long double __aret_x87_ret;
#define AD(i)  (*(double *)(uintptr_t)(esp + (unsigned)(i) * 8))
#define MATH1(name, fn) uint32_t aret_##name(uint32_t esp) { __aret_x87_ret = fn(AD(0)); return 0; }
#define MATH2(name, fn) uint32_t aret_##name(uint32_t esp) { __aret_x87_ret = fn(AD(0), AD(1)); return 0; }
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
    __aret_x87_ret = difftime((time_t)(int32_t)AU(0), (time_t)(int32_t)AU(1));
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
