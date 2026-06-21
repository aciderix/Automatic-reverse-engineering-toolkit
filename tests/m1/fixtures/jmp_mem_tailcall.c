/* Regression for lifting `jmp [mem]` (indirect tail call through a function
   pointer in memory, e.g. mingw's ____lc_codepage_func). A no-argument thunk
   forwarding to a pointer in .data tail-calls as `jmp dword ptr [g_hook]`; that
   must lift to a real indirect tail call (read the pointer, dispatch), not an
   opaque asm no-op. `g_hook` is written in a separate function so the compiler
   cannot devirtualize. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O2 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 jmp_mem_tailcall.c -o jmp_mem_tailcall.exe \
       -lmsvcrt -lkernel32 */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __main(void) {}

static int g_result;
__attribute__((noinline)) static void hook_a(void) { g_result = 1; }
__attribute__((noinline)) static void hook_b(void) { g_result = 42; }

static void (*g_hook)(void) = hook_a;
__attribute__((noinline)) static void set_hook(void (*f)(void)) { g_hook = f; }

/* No args → pure tail call: `jmp dword ptr [g_hook]`. */
__attribute__((noinline)) static void run_hook(void) { g_hook(); }

int main(int argc, char **argv, char **envp) {
    (void)argv;
    (void)envp;
    set_hook(hook_b);
    run_hook();
    printf("JT=%d\n", g_result); /* 42 */
    return 0;
}

void __stdcall mainCRTStartup(void) { ExitProcess(main(1, 0, 0)); }
