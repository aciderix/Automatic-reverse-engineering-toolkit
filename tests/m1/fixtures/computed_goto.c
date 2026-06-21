#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);
/* GCC computed goto (&&label): compiles to `mov reg,[tab+idx*4]; jmp reg` — an
   absolute-address dispatch table, like Lua's luaV_execute bytecode loop. */
static int __attribute__((noinline)) disp(int op, int x) {
    static const void* tab[] = {&&A, &&B, &&C, &&D};
    goto *tab[op & 3];
    A: return x + 1;
    B: return x + 10;
    C: return x + 100;
    D: return x + 1000;
}
void __stdcall mainCRTStartup(void) {
    printf("CGOTO: %d %d %d %d\n", disp(0,5), disp(1,5), disp(2,5), disp(3,5));
    ExitProcess(0);  /* expect 6 15 105 1005 */
}
