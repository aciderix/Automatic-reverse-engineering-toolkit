/* HLE family: Win32 process/thread introspection (measured OS wall, doc 82).
 * GetSystemTimeAdjustment returns the fixed clock defaults (bit-identical to Wine);
 * the affinity mask / CPU times are env-dependent (modelled soundly, not oracle-exact),
 * so only the booleans and invariants are printed -- deterministic across ARET/Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    DWORD adj = 0, inc = 0; BOOL dis = 0;
    BOOL sar = GetSystemTimeAdjustment(&adj, &inc, &dis);
    printf("adjret=%d adj=%lu inc=%lu dis=%d\n", sar != 0, (unsigned long)adj, (unsigned long)inc, dis != 0);

    DWORD_PTR pm = 0, sm = 0;
    BOOL ar = GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    printf("affret=%d subset=%d nz=%d\n", ar != 0, (int)((pm & ~sm) == 0), (int)(pm != 0));
    printf("setaff=%d\n", SetProcessAffinityMask(GetCurrentProcess(), pm) != 0);

    FILETIME c, e, k, u;
    BOOL pt = GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    printf("ptimes=%d exit0=%d\n", pt != 0, (int)(e.dwLowDateTime == 0 && e.dwHighDateTime == 0));

    FILETIME c2, e2, k2, u2;
    BOOL tt = GetThreadTimes(GetCurrentThread(), &c2, &e2, &k2, &u2);
    printf("ttimes=%d\n", tt != 0);

    printf("done\n");
    return 0;
}
