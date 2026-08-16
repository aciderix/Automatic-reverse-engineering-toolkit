/* Proves ARET's loader runs a lifted DLL's PE TLS callback at process attach (the
 * wall real libglib hit). The companion DLL (lift_tlscb.dll.c) registers a TLS
 * callback that sets a flag on DLL_PROCESS_ATTACH; here we read it back. Under Wine
 * the loader runs the callback (ran=1, reason=DLL_PROCESS_ATTACH=1); ARET must match. */
#include <stdio.h>

__declspec(dllimport) int tlscb_ran(void);
__declspec(dllimport) int tlscb_reason(void);

int main(void) {
    printf("tls_ran=%d\n", tlscb_ran());        /* 1 iff the callback fired */
    printf("tls_reason=%d\n", tlscb_reason());   /* DLL_PROCESS_ATTACH = 1 */
    printf("done\n");
    return 0;
}
