/* GetComputerNameA/W (kernel32): the name comes from the host (as Wine's does), so ARET
 * and Wine agree and this compares directly. Exercises the MEASURED size contract:
 * success *nSize = length WITHOUT nul; too-small -> BOOL 0, ERROR_BUFFER_OVERFLOW (111),
 * *nSize = required INCLUDING nul, buffer left untouched; exact length+1 succeeds. */
#include <windows.h>
#include <stdio.h>
int main(void){
    char a[64]; DWORD na = sizeof a;
    BOOL ra = GetComputerNameA(a, &na);
    printf("A ok=%d n=%lu strlen=%d\n", ra, (unsigned long)na, ra ? (int)strlen(a) : -1);

    /* wide: print the code units so W is verified without %ls */
    WCHAR w[64]; DWORD nw = 64;
    BOOL rw = GetComputerNameW(w, &nw);
    printf("W ok=%d n=%lu [", rw, (unsigned long)nw);
    if (rw) for (DWORD i = 0; i < nw; i++) printf("%04x ", (unsigned)w[i]);
    printf("]\n");

    /* too-small buffer: must not be written, err 111, n = needed incl nul */
    char s[1]; DWORD ns = 1; s[0] = 0x7e;
    BOOL rs = GetComputerNameA(s, &ns);
    printf("small ok=%d n=%lu err=%lu untouched=%d\n",
           rs, (unsigned long)ns, (unsigned long)GetLastError(), s[0] == 0x7e);

    /* exact length+1 request succeeds (na was strlen, so na+1 fits) */
    char e[64]; DWORD ne = na + 1;
    BOOL re = GetComputerNameA(e, &ne);
    printf("exact ok=%d n=%lu\n", re, (unsigned long)ne);
    printf("done\n");
    return 0;
}
