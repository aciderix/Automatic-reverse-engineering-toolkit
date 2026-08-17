/* HLE family: shell PIDL from a path (`ILCreateFromPath*`/`ILFree`), part of the
 * 2nd-tier OS wall measured (doc 90, 2026-08-16) once the C++ runtime auto-lifts.
 * Builds a PIDL from an existing path, round-trips it back through SHGetPathFromIDList
 * (already proven), and frees it. Prints only DETERMINISTIC facts (create succeeded,
 * round-trip yields a non-empty path, NULL->NULL) so ARET and Wine match bit-for-bit;
 * the PIDL bytes and the canonicalised path are env-dependent and never printed. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>

int main(void) {
    /* Wide: C:\Windows exists in the prefix (wineboot creates it). */
    LPITEMIDLIST p = ILCreateFromPathW(L"C:\\Windows");
    int created = (p != NULL);
    WCHAR back[MAX_PATH]; back[0] = 0;
    int rt = created && SHGetPathFromIDListW(p, back) && back[0] != 0;
    ILFree(p);
    printf("ilw_create=%d ilw_rt=%d\n", created, rt);

    /* ANSI variant. */
    LPITEMIDLIST pa = ILCreateFromPathA("C:\\Windows");
    int createda = (pa != NULL);
    char backa[MAX_PATH]; backa[0] = 0;
    int rta = createda && SHGetPathFromIDListA(pa, backa) && backa[0] != 0;
    ILFree(pa);
    printf("ila_create=%d ila_rt=%d\n", createda, rta);

    /* NULL path -> NULL PIDL; ILFree(NULL) is a no-op. */
    LPITEMIDLIST pn = ILCreateFromPathW(NULL);
    printf("ilw_null=%d\n", pn == NULL);
    ILFree(NULL);

    printf("done\n");
    return 0;
}
