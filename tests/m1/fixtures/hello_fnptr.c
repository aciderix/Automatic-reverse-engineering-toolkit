/* Indirect-call dispatch fixture: call a function through a pointer (the pointer
   holds the original code VA, dispatched by aret_call to the transpiled sub_). */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

static int __attribute__((noinline)) add_op(int a, int b) { return a + b; }
static int __attribute__((noinline)) mul_op(int a, int b) { return a * b; }

void __stdcall mainCRTStartup(void) {
    int (*ops[2])(int, int) = { add_op, mul_op };
    volatile int sel = 1;                 /* choose mul at runtime */
    int r = ops[sel](6, 7);               /* indirect call */
    printf("INDIRECT: result=%d\n", r);   /* expect 42 */
    ExitProcess(0);
}
