#ifndef __WINE_COMPAT_NTDLL_MISC_H
#define __WINE_COMPAT_NTDLL_MISC_H
/* Heavy-form compat shim (doc 82): the few Wine-header-provided defines that mingw's NT
 * headers omit, so a Wine ntdll .c compiles unchanged. Add here only what a compile asks. */
#include <limits.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
/* 64-bit integer bounds Wine's headers define (winnt.h); absent from mingw's. */
#ifndef I64_MIN
#define I64_MIN  (-9223372036854775807LL - 1)
#define I64_MAX  9223372036854775807LL
#define UI64_MAX 0xffffffffffffffffULL
#endif
#endif
