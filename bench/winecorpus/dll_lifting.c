/* DLL lifting (doc 80 §1.2): the app imports from a companion DLL (dll_lifting.dll.c)
   that ARET lifts too (--with-dll). The imported calls must dispatch to the lifted
   DLL code, bit-identical to Wine loading the real DLL — not to an HLE shim. */
#include <stdio.h>
__declspec(dllimport) int lift_add(int, int);
__declspec(dllimport) int lift_mul(int, int);
__declspec(dllimport) int lift_poly(int);
int main(void) {
    printf("add=%d mul=%d poly=%d\n", lift_add(20, 22), lift_mul(6, 7), lift_poly(5));
    return 0;
}
