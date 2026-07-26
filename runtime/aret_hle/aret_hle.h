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
extern uint32_t g_last_error;               /* per-fiber GetLastError() slot */
int aret_fiber_sleep(uint32_t ms);          /* Sleep via fibers; 0 if no threads */
int aret_current_fiber(void);               /* running fiber index (0 = main / none) */
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

/* Filesystem subsystem (UBT Phase 3). Windows paths are translated to native
 * paths under the ARET prefix: `C:\dir\file` -> `$ARET_PREFIX/drive_c/dir/file`
 * (backslashes become slashes; relative paths pass through). $ARET_PREFIX
 * defaults to "aret_prefix" in the current directory. */

/* msvcrt.dll — C stdio */
uint32_t aret_fopen(uint32_t esp);    /* (name, mode) -> FILE* */
uint32_t aret_fclose(uint32_t esp);   /* (FILE*) -> int */
uint32_t aret_fread(uint32_t esp);    /* (ptr, size, nmemb, FILE*) -> nmemb */
uint32_t aret_fwrite(uint32_t esp);   /* (ptr, size, nmemb, FILE*) -> nmemb */
uint32_t aret_fputs(uint32_t esp);    /* (s, FILE*) -> int */
uint32_t aret_fgets(uint32_t esp);    /* (buf, n, FILE*) -> buf|0 */
uint32_t aret_fseek(uint32_t esp);    /* (FILE*, off, whence) -> int */
uint32_t aret_ftell(uint32_t esp);    /* (FILE*) -> long */
uint32_t aret_remove(uint32_t esp);   /* (name) -> int */

/* kernel32.dll — Win32 file API (handles are POSIX fds in this model) */
uint32_t aret_CreateFileA(uint32_t esp);  /* (name, access, share, sec, disp, flags, tmpl) -> HANDLE */
uint32_t aret_CloseHandle(uint32_t esp);  /* (HANDLE) -> BOOL */
uint32_t aret_DeleteFileA(uint32_t esp);  /* (name) -> BOOL */
uint32_t aret_SetFilePointer(uint32_t esp); /* (h, dist, *high, method) -> DWORD */
uint32_t aret_GetFileAttributesA(uint32_t esp); /* (name) -> DWORD attrs / INVALID */

/* Diagnostic for an intercepted import that has no shim yet: the builder emits a
 * weak stub per unresolved import that calls this (warns once, then returns 0),
 * so a large binary still links and reports exactly which APIs it needs. */
void aret_unimpl(const char *name);

/* An instruction the lifter could not model, reached at run time. Continuing
 * would substitute a no-op for an unknown effect — a silent wrong result — so we
 * fail loud (print the instruction and abort) instead. The transpile report's
 * `partial(asm)` count lists how many functions still contain one statically. */
void aret_unmodelled(const char *insn);

/* Runtime delay-load resolver fallbacks (DLL lifting): a delay-loaded import gets
 * a synthetic VA at runtime, so the static dispatch defers a miss to these — one
 * dispatches the call, the other reports its stdcall pop. Both return 0/no-op
 * when no delay-import was resolved (the common case). */
int aret_delay_dispatch(uint32_t va, uint32_t esp, uint64_t *out);
uint32_t aret_delay_pop(uint32_t va);

/* No-op shim (returns 0) for recognized startup-glue functions (mingw/MSVC
 * global ctor/dtor runners, EH-frame registration, pseudo-relocator) we bind
 * away instead of lifting their indirect-call-heavy table walks. */
uint32_t aret_noop(uint32_t esp);

/* Segment bases for the transpiled code: `fs:[ea]` becomes a load/store at
 * `__aret_fs() + ea`, pointing into a synthetic TEB/PEB so the Windows startup's
 * thread/process-block reads work natively. gs is unused in 32-bit user mode. */
uint32_t __aret_fs(void);
uint32_t __aret_gs(void);

/* Indirect-call dispatch: a function pointer holds the original code virtual
 * address, so calls through it are routed here, which maps the VA to the
 * transpiled `sub_<va>` (generated table in aret_dispatch.c). */
uint64_t aret_call(uint32_t va, uint64_t esp, uint64_t a, uint64_t c, uint64_t d, uint64_t b, uint64_t si, uint64_t di, uint64_t bx);
/* Execution trace (--trace, doc 81 §I1): record a lifted function's entry; dumped on crash. */
void aret_trace_push(uint32_t va, uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx,
                     uint32_t ebp, uint32_t esi, uint32_t edi, uint32_t ebx);
void aret_trace_dump(void);

/* Bytes of stack arguments the internal function at code VA `va` pops on return
 * (`ret N`, the __stdcall/FAST_FUNC callee-pops-args ABI); 0 for cdecl/unknown.
 * After an indirect `call`, the caller raises esp by this so the callee's own
 * cleanup is modelled (generated table in aret_dispatch.c). */
uint32_t __aret_callee_pop(uint32_t va);

/* Register a transpiled callback (its code VA) to run at process exit; shared by
 * the atexit and _onexit shims. */
void aret_register_atexit(uint32_t va);

/* Data imports (variables): for a known name returns a pointer to a synthetic
 * object (e.g. the `_iob` stdio array), so the builder can patch the IAT slot to
 * it; 0 for names we do not model. */
uint32_t aret_data_import(const char *name);

#endif /* ARET_HLE_H */
