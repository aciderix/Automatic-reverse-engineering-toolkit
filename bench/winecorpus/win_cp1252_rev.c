/* CP1252 REVERSE (UTF16->ANSI) with Wine's best-fit, modelled (doc 82). kernel32
 * WideCharToMultiByte(CP_ACP) and ntdll RtlUnicodeToMultiByteN share the measured table
 * -> bit-identical Wine: exact CP1252 slots, best-fit (A-macron->'A', frac-slash->'/'),
 * and the default char '?' for the genuinely unmappable. */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
NTSTATUS WINAPI RtlUnicodeToMultiByteN(PCHAR,ULONG,PULONG,PCWCH,ULONG);
int main(void) {
    unsigned short cps[] = {
        0x0041,0x00E9,0x00A9,0x20AC,0x2019,0x0178,0x0152,0x2122,0x2013,0x2014, /* exact CP1252 */
        0x0100,0x0130,0x2044,0xFF41,0xFF5A,0x00BC,                             /* best-fit */
        0x2153,0x0391,0x4E2D,0x0500                                            /* unmappable -> '?' */
    };
    int k = sizeof cps / sizeof cps[0];
    /* whole string through kernel32 */
    char kb[64]; BOOL used = 0;
    int kn = WideCharToMultiByte(CP_ACP, 0, (WCHAR *)cps, k, kb, sizeof kb, NULL, &used);
    printf("k32 n=%d used=%d:", kn, used);
    for (int i = 0; i < kn; i++) printf(" %02X", (unsigned char)kb[i]);
    printf("\n"); fflush(stdout);
    /* whole string through ntdll */
    char nb[64]; ULONG nn = 0;
    RtlUnicodeToMultiByteN(nb, sizeof nb, &nn, (PCWCH)cps, (ULONG)(k * 2));
    printf("ntdll n=%lu:", (unsigned long)nn);
    for (unsigned i = 0; i < nn; i++) printf(" %02X", (unsigned char)nb[i]);
    printf("\n"); fflush(stdout);
    return 0;
}
