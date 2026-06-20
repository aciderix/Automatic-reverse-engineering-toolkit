/* x87 floating-point fixture: same source as hello_float.c but built for the
   x87 FPU (the i686 default, no -mfpmath=sse), so float ops lift to the __x87_*
   long-double helpers. Exercises the LLVM backend's x86_fp80 typing.
   Build: i686-w64-mingw32-gcc -nostdlib -O1 -e _mainCRTStartup@0 \
          -o hello_float_x87.exe hello_float_x87.c -lmsvcrt -lkernel32 */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

void __stdcall mainCRTStartup(void) {
    volatile double a = 3.5, b = 2.0;
    double c = a * b + 1.5;            /* 8.5 */
    printf("FLOAT: c=%d c10=%d\n", (int)c, (int)(c * 10.0));  /* 8 85 */
    ExitProcess(0);
}
