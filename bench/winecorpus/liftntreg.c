/* Levier 1 x Nt* floor: the app imports dll_ntreg_roundtrip from a companion DLL that ARET LIFTS
 * (--with-dll, auto-detected from liftntreg.dll.c). The lifted DLL internally calls the ntdll Nt*
 * registry syscalls, which the multi-module loader routes to ARET's aret_Nt* shims -> g_reg.
 * Bit-identical to Wine loading the real DLL -> real ntdll: a lifted binary DLL reaches the Nt*
 * registry floor end-to-end. */
#include <stdio.h>
__declspec(dllimport) unsigned dll_ntreg_roundtrip(unsigned);
int main(void){
    printf("lifted-dll ntreg roundtrip(1234)=%u\n", dll_ntreg_roundtrip(1234));
    printf("lifted-dll ntreg roundtrip(42)=%u\n", dll_ntreg_roundtrip(42));
    return 0;
}
