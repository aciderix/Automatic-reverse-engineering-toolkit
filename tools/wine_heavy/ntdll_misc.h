#ifndef __WINE_COMPAT_NTDLL_MISC_H
#define __WINE_COMPAT_NTDLL_MISC_H
#include <limits.h>
#include <winternl.h>   /* guarantees NTSTATUS/WCHAR/UNICODE_STRING before the floor prototypes */
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#ifndef I64_MIN
#define I64_MIN  (-9223372036854775807LL - 1)
#define I64_MAX  9223372036854775807LL
#define UI64_MAX 0xffffffffffffffffULL
#endif
#include "ntdll_floor.h"
#endif
