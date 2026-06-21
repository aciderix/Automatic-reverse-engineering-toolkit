#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);
/* A jump-table switch where several cases share a body (cases 1,2,4 -> 2000),
   producing duplicate jump-table entries. The structurer maps case k ->
   successors[k]; if duplicate targets are collapsed, every case after the first
   duplicate is routed to the wrong block (the bug behind Lua's GC dispatching a
   userdata to the upvalue case). */
static int __attribute__((noinline)) sw(int x) {
    int r;
    switch (x) {
        case 0: r = 1000; break;
        case 1: r = 2000; break;
        case 2: r = 2000; break;
        case 3: r = 3000; break;
        case 4: r = 2000; break;
        case 5: r = 5000; break;
        default: r = 9; break;
    }
    return r + x;
}
void __stdcall mainCRTStartup(void) {
    printf("JT: %d %d %d %d %d %d\n", sw(0), sw(1), sw(2), sw(3), sw(4), sw(5));
    ExitProcess(0);  /* expect 1000 2001 2002 3003 2004 5005 */
}
