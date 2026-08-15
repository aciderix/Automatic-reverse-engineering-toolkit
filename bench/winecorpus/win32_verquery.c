/* HLE guard for the ninja-startup OS surface (doc 82): VerSetConditionMask (pure
 * bit-packing), FindFirstFileExA (extended find), GetActiveProcessorCount. Prints only
 * deterministic facts so ARET and Wine match. (VerifyVersionInfo itself is exercised
 * bit-identically by `ninja -n` end-to-end but is NOT winediff-checkable here: Wine
 * compares against its OWN reported version, ARET against its fixed 6.2.) */
#include <windows.h>
#include <stdio.h>

int main(void) {
    /* VerSetConditionMask: 3-bit condition placed at the type's slot (version-independent).
       VER_MAJORVERSION=2 -> slot 1 ; VER_MINORVERSION=1 -> slot 0 ; VER_GREATER_EQUAL=3. */
    ULONGLONG m = VerSetConditionMask(0, VER_MAJORVERSION, VER_GREATER_EQUAL);   /* 3<<3 = 24 */
    m = VerSetConditionMask(m, VER_MINORVERSION, VER_GREATER_EQUAL);             /* |= 3<<0 = 27 */
    printf("mask=%llu\n", (unsigned long long)m);

    /* FindFirstFileExA on a file we create -> same enumeration as FindFirstFileA. */
    FILE *f = fopen("verq.dat", "wb"); fputs("z", f); fclose(f);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileExA("verq.dat", FindExInfoStandard, &fd,
                                FindExSearchNameMatch, NULL, 0);
    printf("findex=%d name=%s\n", h != INVALID_HANDLE_VALUE,
           h != INVALID_HANDLE_VALUE ? fd.cFileName : "?");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    remove("verq.dat");

    /* GetActiveProcessorCount: value is env-dependent (host cores vs ARET's 1), so only
       the invariant is printed. */
    printf("nproc_ok=%d\n", (int)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) >= 1));

    printf("done\n");
    return 0;
}
