/* Regression for address-taken function discovery (stripped, frame-pointer
   omitted). `pick` is reached ONLY through a function-pointer table in data and
   is compiled without a frame pointer, so neither recursive descent (no direct
   call) nor the `push ebp; mov ebp,esp` prologue scan finds it. Discovery must
   recover it from the code pointer in `.data` so the indirect call resolves.
   Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin -fomit-frame-pointer \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 address_taken_callback.c -o address_taken_callback.exe \
       -lmsvcrt -lkernel32
     i686-w64-mingw32-strip address_taken_callback.exe
   (-fomit-frame-pointer so the callback does NOT start with 55 8B EC; stripped so
   there is no symbol to seed it.) */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __main(void) {}

/* A directly-called helper, so the callbacks are non-leaf (push a callee-saved
   register first) — that is the function start address-taken discovery keys on. */
__attribute__((noinline)) static int side(int x) { return x; }

__attribute__((noinline)) static int add(int a, int b) { return side(a) + side(b); }
__attribute__((noinline)) static int mul(int a, int b) { return side(a) * side(b); }

typedef int (*binop)(int, int);
/* Function-pointer table in .data — the only reference to add/mul. */
static binop ops[2] = { add, mul };

__attribute__((noinline)) int main(int argc, char **argv, char **envp) {
    (void)argv;
    (void)envp;
    volatile int i = 1;            /* opaque: forces the indirect call */
    int r = ops[i](6, 7);          /* through the table → mul(6,7) = 42 */
    printf("RESULT=%d\n", r);
    return 0;
}

void __stdcall mainCRTStartup(void) {
    int rc = main(1, 0, 0);
    ExitProcess(rc);
}
