/* ARET HLE runtime — Win32 → POSIX compatibility shims (UBT Phase 3).
 *
 * These functions replace the Windows API imports of a transpiled PE. They are
 * called by ARET-generated C with ONE argument: the modelled stack pointer
 * `esp` at the call site. The original 32-bit stdcall/cdecl arguments live on
 * that modelled stack, so each shim reads them ABI-accurately at esp+0, esp+4,
 * … (no return-address slot exists in the transpiled model — the call is a
 * plain C call, so argument 0 is at esp+0).
 *
 * This is the embryo of `aret_os_hle` from the design document: a native
 * implementation of the source OS's API, compiled INTO the final binary so the
 * program runs natively on the target — not under an emulator or Wine.
 */
#ifndef ARET_HLE_H
#define ARET_HLE_H

#include <stdint.h>

/* The transpiled code passes the modelled 32-bit stack pointer. Each shim reads
 * its stdcall arguments from there. Return values are the Win32 DWORD/BOOL/HANDLE
 * the caller expects in EAX. */

/* kernel32.dll — console / standard handles */
uint32_t GetStdHandle(uint32_t esp);   /* (DWORD nStdHandle) -> HANDLE (mapped to a POSIX fd) */
uint32_t WriteFile(uint32_t esp);      /* (h, buf, len, *written, overlapped) -> BOOL */
uint32_t ReadFile(uint32_t esp);       /* (h, buf, len, *read, overlapped) -> BOOL */
uint32_t WriteConsoleA(uint32_t esp);  /* (h, buf, nchars, *written, reserved) -> BOOL */

/* kernel32.dll — process / misc */
uint32_t ExitProcess(uint32_t esp);    /* (UINT code) -> noreturn */
uint32_t GetLastError(uint32_t esp);   /* () -> DWORD */
uint32_t SetLastError(uint32_t esp);   /* (DWORD) -> void */
uint32_t Sleep(uint32_t esp);          /* (DWORD ms) -> void */
uint32_t GetCurrentProcessId(uint32_t esp); /* () -> DWORD */

#endif /* ARET_HLE_H */
