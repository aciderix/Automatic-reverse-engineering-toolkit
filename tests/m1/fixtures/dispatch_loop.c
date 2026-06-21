#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);
/* A bytecode dispatch loop: each handler re-dispatches via computed goto, so the
   loop header ends in the jump-table switch. The structurer must emit the switch
   inside `while(1)` — otherwise the loop body is empty and it spins forever
   (exactly Lua's luaV_execute). */
static int __attribute__((noinline)) interp(const unsigned char* code) {
    static const void* tab[] = {&&ADD, &&MUL, &&HALT};
    int pc = 0, acc = 1;
    goto *tab[code[pc++]];
    ADD:  acc += 3; goto *tab[code[pc++]];
    MUL:  acc *= 2; goto *tab[code[pc++]];
    HALT: return acc;
}
void __stdcall mainCRTStartup(void) {
    static const unsigned char prog[] = {0, 1, 0, 2}; /* ADD MUL ADD HALT */
    printf("INTERP: %d\n", interp(prog));  /* 1 -> 4 -> 8 -> 11 */
    ExitProcess(0);
}
