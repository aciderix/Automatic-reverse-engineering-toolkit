/* CLI-corpus tier-2 cluster (wallsweep, after tier-1): BCryptGenRandom, MoveFileEx,
 * ReadConsoleW. (TerminateProcess/DebugBreak are trivial and terminate the process, so
 * they are sound by construction rather than stdout-diffed here.)
 *
 * Only environment-invariant contract is printed, so it is bit-identical Wine:
 * BCryptGenRandom succeeds and two draws differ (real entropy, not a guessed constant);
 * MoveFileEx renames; ReadConsoleW on a REDIRECTED stdin returns FALSE (not a console),
 * exactly as Windows/Wine and our isatty() gate agree under winediff. */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    unsigned char a[16] = {0}, b[16] = {0};
    NTSTATUS s1 = BCryptGenRandom(NULL, a, sizeof a, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    NTSTATUS s2 = BCryptGenRandom(NULL, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    int nonzero = 0;
    for (int i = 0; i < 16; i++) if (a[i]) nonzero = 1;
    printf("bcrypt s1:%d s2:%d nonzero:%d differ:%d\n",
           (int)s1, (int)s2, nonzero, memcmp(a, b, 16) != 0);

    FILE *f = fopen("aret_mv_src.tmp", "w");
    if (f) { fputs("x", f); fclose(f); }
    BOOL mv = MoveFileExA("aret_mv_src.tmp", "aret_mv_dst.tmp", MOVEFILE_REPLACE_EXISTING);
    FILE *g = fopen("aret_mv_dst.tmp", "r");
    int moved = (g != NULL);
    if (g) fclose(g);
    remove("aret_mv_dst.tmp");
    remove("aret_mv_src.tmp");
    printf("movefileex ok:%d moved:%d\n", mv, moved);

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t wbuf[8];
    DWORD nr = 999;
    BOOL rc = ReadConsoleW(hIn, wbuf, 4, &nr, NULL);
    printf("readconsole ok:%d\n", rc);
    return 0;
}
