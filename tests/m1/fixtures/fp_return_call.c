/* A function returning a double (in st(0) under the 32-bit x87 ABI), whose result
   the caller compares with fucomi. This exercises the x87 fp-return channel: the
   depth analysis must count the st(0) the call pushes (else the x87 stack
   underflows and the whole comparison degrades to opaque asm), AND the returned
   *value* must be threaded from callee to caller (else the comparison runs on an
   undefined st(0)). This is the exact shape of Lua's luaL_checkversion_, whose
   `if (v != ver)` otherwise always took its error path. Expected: match. Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 fp_return_call.c -o fp_return_call.exe \
       -lmsvcrt -lkernel32  */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

static double __attribute__((noinline)) version(void) { return 504.0; }

void __stdcall mainCRTStartup(void) {
    double v = version();
    printf("FPRET: %s\n", (v == 504.0) ? "match" : "mismatch");
    ExitProcess(0);
}
