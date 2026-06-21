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
uint32_t aret_rand(uint32_t esp)   { (void)esp; return (uint32_t)rand(); }
uint32_t aret_srand(uint32_t esp)  { srand(AU(0)); return 0; }
uint32_t aret_getenv(uint32_t esp) { return RP(getenv(ACS(0))); }
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
/* snprintf / _snprintf: (dst, n, fmt, ...). cap of 0 means "measure". */
uint32_t aret_snprintf(uint32_t esp) {
    char *dst = AS(0);
    size_t cap = AU(1);
    const char *fmt = ACS(2);
    const uint32_t *va = &((const uint32_t *)(uintptr_t)esp)[3];
    char tmp[8192], *out = (cap && cap <= sizeof tmp) ? dst : tmp;
    size_t room = (cap && cap <= sizeof tmp) ? cap : sizeof tmp;
    size_t n = aret_vformat(out, room, fmt, va);
    if (out != dst && cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(dst, tmp, c); dst[c] = 0; }
    return (uint32_t)n;
}
/* snprintf and _snprintf both sanitize to aret_snprintf. */

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
uint32_t aret_onexit(uint32_t esp) { return AU(0); }

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
