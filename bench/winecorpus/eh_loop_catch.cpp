/* GNU/Itanium C++ EH — a try/catch INSIDE a loop, where the catch continuation
 * resumes code that reads a local the compiler kept in a CALLEE-SAVED REGISTER
 * (the loop counter in %ebx at -O1). This is the regression guard for the bug where
 * a catch continuation / cleanup landing pad — lifted as a separate function and
 * resumed via aret_call — arrived with ebx/esi/edi = 0 (the EH dispatcher only
 * threaded esp/ebp/obj/sel), so the resumed loop restarted at n=0 and spun forever
 * ("caught 11" without end). The fix records the establisher's live callee-saved
 * regs at each guarded call (aret_gnu_eh_setpc) and hands them to the resume
 * (aret_gnu_eh_run). Memory-resident locals already reach the pad via the shared
 * stack; this closes the register-resident case.
 *
 * worker(n) holds nested RAII Guards and throws an int at depth n, so each scope
 * emits a cleanup landing pad and the unwind runs the right destructors; main loops
 * n=1..6 with the counter in %ebx across the throwing call. __cxa_* / _Unwind_* are
 * ARET HLE shims (libstdc++/libgcc are Wine-only deps, see .winelibs); the same
 * libstdc++ loads beside the exe under Wine (the oracle) => bit-identical. */
#include <cstdio>
struct Guard { int id; Guard(int i):id(i){ printf("G%d\n",id);} ~Guard(){ printf("~G%d\n",id);} };
__attribute__((noinline)) static int worker(int n){
    Guard a(1);
    if(n==1) throw 11;
    Guard b(2);
    if(n==2) throw 22;
    {
        Guard c(3);
        if(n==3) throw 33;
        Guard d(4);
        if(n==4) throw 44;
    }
    Guard e(5);
    if(n==5) throw 55;
    return n*7;
}
int main(){
    setvbuf(stdout,0,_IONBF,0);
    printf("start\n");
    for(int n=1;n<=6;n++){
        try { int r = worker(n); printf("r=%d\n", r); }
        catch(int x){ printf("caught %d\n", x); }
    }
    printf("done\n");
    return 0;
}
