/* CP-1252 ANSI->UTF16 modelled (doc 82, plan A): kernel32 MultiByteToWideChar(CP_ACP) and
 * ntdll RtlMultiByteToUnicodeString share ARET's CP1252 table -> bit-identical Wine on the
 * bytes that differ from Latin-1 (0x80-0x9F: euro, curly quotes, dashes...). */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
int main(void) {
    /* every byte 0x80..0xFF through kernel32 CP_ACP */
    char mb[0x81]; for (int i = 0; i < 0x80; i++) mb[i] = (char)(0x80 + i); mb[0x80] = 0;
    WCHAR wc[0x81];
    int n = MultiByteToWideChar(CP_ACP, 0, mb, 0x80, wc, 0x80);
    printf("k32 n=%d:", n);
    for (int i = 0; i < n; i++) printf(" %04X", wc[i]);
    printf("\n");
    /* the same through ntdll's Rtl (served by compiled Wine + the shared table) */
    ANSI_STRING a; a.Buffer = mb; a.Length = 0x80; a.MaximumLength = 0x81;
    UNICODE_STRING u;
    NTSTATUS s = RtlAnsiStringToUnicodeString(&u, &a, TRUE);
    printf("ntdll hr=%ld len=%u:", (long)s, u.Length);
    for (int i = 0; i < u.Length / 2; i++) printf(" %04X", u.Buffer[i]);
    printf("\n");
    RtlFreeUnicodeString(&u);
    return 0;
}
