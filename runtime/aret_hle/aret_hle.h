/* ARET HLE runtime — source-OS API shims (UBT Phase 3), implemented natively.
 *
 * Every shim is named `aret_<import>` (the transpiler rewrites each intercepted
 * import call to this name) and takes ONE argument: the modelled stack pointer
 * `esp` at the call site. The original 32-bit stdcall/cdecl arguments live on
 * that modelled stack, so each shim reads them ABI-accurately at esp+0, esp+4,
 * … (argument 0 is at esp+0 — the modelled call pushes no return address for a
 * shim). The `aret_` prefix keeps these names from colliding with the real libc
 * functions the shims call.
 *
 * This is the embryo of `aret_os_hle` from the design document: a native
 * implementation of the source OS's API, compiled INTO the final binary so the
 * program runs natively on the target — not under an emulator or Wine.
 */
#ifndef ARET_HLE_H
#define ARET_HLE_H

#include <stdint.h>

/* kernel32.dll — console / standard handles */
uint32_t aret_GetStdHandle(uint32_t esp);  /* (DWORD nStdHandle) -> HANDLE (a POSIX fd) */
uint32_t aret_WriteFile(uint32_t esp);      /* (h, buf, len, *written, overlapped) -> BOOL */
uint32_t aret_ReadFile(uint32_t esp);       /* (h, buf, len, *read, overlapped) -> BOOL */
uint32_t aret_WriteConsoleA(uint32_t esp);  /* (h, buf, nchars, *written, reserved) -> BOOL */

/* kernel32.dll — process / misc */
uint32_t aret_ExitProcess(uint32_t esp);    /* (UINT code) -> noreturn */
uint32_t aret_GetLastError(uint32_t esp);   /* () -> DWORD */
uint32_t aret_SetLastError(uint32_t esp);   /* (DWORD) -> void */
uint32_t aret_Sleep(uint32_t esp);          /* (DWORD ms) -> void */
uint32_t aret_GetCurrentProcessId(uint32_t esp); /* () -> DWORD */

/* msvcrt.dll — C runtime (UBT M4). Variadic functions read their arguments from
 * the shared machine stack at [esp+0], [esp+4], … (cdecl). */
uint32_t aret_printf(uint32_t esp);   /* (fmt, ...) -> int */
uint32_t aret_puts(uint32_t esp);     /* (s) -> int */
uint32_t aret_putchar(uint32_t esp);  /* (c) -> int */
uint32_t aret_malloc(uint32_t esp);   /* (size) -> void* (32-bit) */
uint32_t aret_calloc(uint32_t esp);   /* (n, size) -> void* */
uint32_t aret_realloc(uint32_t esp);  /* (ptr, size) -> void* */
uint32_t aret_free(uint32_t esp);     /* (ptr) -> void */
uint32_t aret_memcpy(uint32_t esp);   /* (dst, src, n) -> dst */
uint32_t aret_memset(uint32_t esp);   /* (dst, c, n) -> dst */
uint32_t aret_memmove(uint32_t esp);  /* (dst, src, n) -> dst */
uint32_t aret_strlen(uint32_t esp);   /* (s) -> size_t */
uint32_t aret_strcmp(uint32_t esp);   /* (a, b) -> int */
uint32_t aret_strcpy(uint32_t esp);   /* (dst, src) -> dst */

#endif /* ARET_HLE_H */
