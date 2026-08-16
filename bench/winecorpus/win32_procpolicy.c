/* Process-policy / error-mode startup shims exercised by real runtimes (glib's
 * gspawn startup hit HeapSetInformation and SetProcessDEPPolicy). Deterministic:
 *   - SetErrorMode round-trips (returns the previous mode; GetErrorMode reads it back);
 *   - HeapSetInformation (advisory heap tuning) and SetProcessDEPPolicy (DEP hardening)
 *     report whatever Wine reports — the harness fixes the expected value. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    UINT prev = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    printf("seterr_prev=%u\n", prev);                 /* previous mode (0 at start) */
    printf("geterr=%u\n", GetErrorMode());            /* the mode we just set */
    UINT prev2 = SetErrorMode(0);
    printf("seterr_prev2=%u\n", prev2);               /* the previous set value round-trips */

    BOOL hi = HeapSetInformation(GetProcessHeap(), HeapEnableTerminationOnCorruption, NULL, 0);
    printf("heapinfo=%d\n", hi != 0);

    BOOL dep = SetProcessDEPPolicy(PROCESS_DEP_ENABLE);
    printf("dep=%d le=%lu\n", dep != 0, dep ? 0UL : (unsigned long)GetLastError());

    printf("done\n");
    return 0;
}
