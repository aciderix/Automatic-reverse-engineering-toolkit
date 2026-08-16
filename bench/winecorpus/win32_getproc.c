/* GetProcAddress resolving a named API to a CALLABLE pointer (previously a flat NULL).
 * The wall a real lifted glib program hit: glib GetProcAddress's ntdll RtlGetVersion at
 * startup and asserts it non-NULL, then calls it. Deterministic and bit-comparable:
 *   - RtlGetVersion (ntdll) resolves, is callable, reports the modelled version 6.2.9200
 *     (same as GetVersionEx — the two must agree, and do on Windows/Wine);
 *   - a kernel32 API we model (GetVersion) resolves and is callable through the pointer;
 *   - a bogus name resolves to NULL (GetProcAddress's own "not found"). */
#include <windows.h>
#include <stdio.h>

typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);
typedef DWORD (WINAPI *GetVersion_t)(void);

int main(void) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HMODULE k32   = GetModuleHandleA("kernel32.dll");

    /* The exact build number is environment-dependent — Wine's RtlGetVersion reports its
     * emulated build (uncapped), ARET reports its modelled Windows (6.2.9200) consistently
     * with GetVersionEx. Compare the modelable invariants: callable, STATUS_SUCCESS, an NT
     * platform, and a plausible (>= 6) major — not the env-specific build (cf. _getdrive). */
    RtlGetVersion_t rtl = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
    printf("rtl_nonnull=%d\n", rtl != NULL);
    if (rtl) {
        OSVERSIONINFOW o; memset(&o, 0, sizeof o); o.dwOSVersionInfoSize = sizeof o;
        LONG st = rtl(&o);
        printf("rtl st=%ld plat=%lu major_ge6=%d\n",
               st, o.dwPlatformId, o.dwMajorVersion >= 6);
    }

    GetVersion_t gv = (GetVersion_t)GetProcAddress(k32, "GetVersion");
    printf("gv_nonnull=%d\n", gv != NULL);
    if (gv) printf("gv_nt=%d\n", (gv() & 0x80000000u) == 0);   /* bit 31 clear = NT */

    FARPROC bogus = GetProcAddress(k32, "ThisApiDoesNotExist_ARET");
    printf("bogus_null=%d\n", bogus == NULL);

    printf("done\n");
    return 0;
}
