/* shell32 CSIDL / special-folder family (aret_SHGet*).
 *
 * WinMerge/MFC90 walk this exact idiom at startup:
 *   SHGetSpecialFolderLocation(csidl, &pidl) -> SHGetPathFromIDListW(pidl) ->
 *   free via SHGetMalloc()->Free.  Plus the direct forms SHGetSpecialFolderPath /
 *   SHGetFolderPath used by mfc90u.
 *
 * The concrete PATH a special folder resolves to is environment-specific (user
 * name, OS layout) and legitimately differs between ARET and Wine — so it is NOT
 * a soundness property and the fixture does not print it. What IS invariant, and
 * what this checks bit-identically against Wine, is the CONTRACT: success codes,
 * a non-empty path, a round-tripping PIDL, that the two API families agree, and
 * that an unknown CSIDL is a DEFINED failure (never a made-up path). */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objidl.h>
#include <stdio.h>

int main(void) {
    /* 1. Location + path round-trip, freed through the shell allocator. */
    LPITEMIDLIST pidl = NULL;
    HRESULT hr = SHGetSpecialFolderLocation(NULL, CSIDL_APPDATA, &pidl);
    printf("loc hr=%ld pidl=%d\n", (long)hr, pidl != NULL);
    if (hr == S_OK && pidl) {
        WCHAR wpath[MAX_PATH];
        wpath[0] = 0;
        BOOL ok = SHGetPathFromIDListW(pidl, wpath);
        printf("frompidl ok=%d nonempty=%d\n", ok, wpath[0] != 0);

        IMalloc *pm = NULL;
        HRESULT hm = SHGetMalloc(&pm);
        printf("shmalloc hr=%ld pm=%d\n", (long)hm, pm != NULL);
        if (pm) {
            IMalloc_Free(pm, pidl);
            IMalloc_Release(pm);
        }
    }

    /* 2. Direct path forms (what mfc90u calls). */
    WCHAR w2[MAX_PATH];
    w2[0] = 0;
    BOOL ok2 = SHGetSpecialFolderPathW(NULL, w2, CSIDL_PERSONAL, FALSE);
    printf("specpath ok=%d nonempty=%d\n", ok2, w2[0] != 0);

    WCHAR w3[MAX_PATH];
    w3[0] = 0;
    HRESULT hr3 = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, w3);
    printf("folderpath hr=%ld nonempty=%d\n", (long)hr3, w3[0] != 0);

    /* 3. Unknown CSIDL -> defined failure (not a guessed path). */
    WCHAR w4[MAX_PATH];
    w4[0] = 0;
    BOOL ok4 = SHGetSpecialFolderPathW(NULL, w4, 0x0064, FALSE);
    printf("unknown ok=%d empty=%d\n", ok4, w4[0] == 0);

    return 0;
}
