/* HLE family: Win32 wide-char FS volumes/paths (`*W`), the 2nd-tier OS wall measured
 * (doc 90, 2026-08-16) once the C++ runtime auto-lifts. Exercises GetLongPathNameW,
 * GetVolumePathNameW, GetDiskFreeSpaceExA, SearchPathW, SetFileInformationByHandle and
 * FindFirst/Next/CloseVolume. Prints only DETERMINISTIC facts (path echoes, the EOF-set
 * file size, success bools + invariants, volume-GUID SHAPE) so ARET and Wine match
 * bit-for-bit; fully-qualified paths, raw byte counts and the volume GUID itself are
 * env-dependent and never printed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int main(void) {
    /* GetLongPathNameW: identity for an already-long name that exists. */
    DeleteFileW(L"aretlp.txt");
    HANDLE hl = CreateFileW(L"aretlp.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr = 0; WriteFile(hl, "0123456789", 10, &wr, NULL); CloseHandle(hl);
    WCHAR lbuf[MAX_PATH]; DWORD lr = GetLongPathNameW(L"aretlp.txt", lbuf, MAX_PATH);
    printf("longpath=%d echo=%d\n", lr == 10, wcscmp(lbuf, L"aretlp.txt") == 0);

    /* GetVolumePathNameW: drive root of the path (single-volume model). */
    WCHAR vp[MAX_PATH];
    BOOL vok = GetVolumePathNameW(L"C:\\Windows\\System32\\x", vp, MAX_PATH);
    printf("volpath=%d root=%d\n", vok != 0, wcscmp(vp, L"C:\\") == 0);
    /* A relative path resolves against the current drive, which is env-dependent (Wine
     * maps the Unix cwd to Z:, our model to C:) -> assert only the drive-root SHAPE. */
    BOOL vok2 = GetVolumePathNameW(L"relative\\name", vp, MAX_PATH);
    printf("volpath_rel=%d shape=%d\n", vok2 != 0, vp[1] == L':' && vp[2] == L'\\' && vp[3] == 0);

    /* GetDiskFreeSpaceExA: env-dependent bytes -> only success + the avail<=total invariant. */
    ULARGE_INTEGER avail, total, freeb;
    BOOL dr = GetDiskFreeSpaceExA(".", &avail, &total, &freeb);
    printf("diskA=%d inv=%d\n", dr != 0, (int)(avail.QuadPart <= total.QuadPart));

    /* SearchPathW: find a file that exists in the current directory. The full result is
     * env-dependent (fully-qualified) -> assert only found + that filePart is the name. */
    LPWSTR fp = NULL; WCHAR sbuf[MAX_PATH];
    DWORD sr = SearchPathW(NULL, L"aretlp.txt", NULL, MAX_PATH, sbuf, &fp);
    printf("search=%d fp=%d\n", sr != 0, fp && wcscmp(fp, L"aretlp.txt") == 0);
    DWORD sr2 = SearchPathW(L".", L"aretlp", L".txt", MAX_PATH, sbuf, &fp);  /* ext appended */
    printf("search_ext=%d\n", sr2 != 0);
    DWORD sr3 = SearchPathW(L".", L"no_such_aret_file.zzz", NULL, MAX_PATH, sbuf, &fp);
    printf("search_miss=%d\n", sr3 == 0);

    /* SetFileInformationByHandle FileEndOfFileInfo: truncate to 4 bytes, verify the size. */
    HANDLE he = CreateFileW(L"aretlp.txt", GENERIC_WRITE | GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    FILE_END_OF_FILE_INFO eofi; eofi.EndOfFile.QuadPart = 4;
    BOOL se = SetFileInformationByHandle(he, FileEndOfFileInfo, &eofi, sizeof eofi);
    LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(he, &sz); CloseHandle(he);
    printf("seteof=%d size=%lld\n", se != 0, (long long)sz.QuadPart);

    /* FindFirstVolumeW: assert the GUID-path SHAPE and that FindVolumeClose succeeds. The
     * GUID itself and the volume count are env-dependent -> not printed. */
    WCHAR voln[MAX_PATH]; HANDLE hv = FindFirstVolumeW(voln, MAX_PATH);
    int vfound = (hv != INVALID_HANDLE_VALUE);
    size_t vl = vfound ? wcslen(voln) : 0;
    int vshape = vfound && vl >= 12
                 && wcsncmp(voln, L"\\\\?\\Volume{", 11) == 0
                 && voln[vl - 2] == L'}' && voln[vl - 1] == L'\\';
    int vclose = vfound ? (FindVolumeClose(hv) != 0) : 0;
    printf("vol_found=%d vol_shape=%d vol_close=%d\n", vfound, vshape, vclose);

    DeleteFileW(L"aretlp.txt");
    printf("done\n");
    return 0;
}
