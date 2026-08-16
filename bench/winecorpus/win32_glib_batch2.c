/* libglib residual B2: GetFileInformationByHandleEx (FileStandardInfo from fstat) and
 * SHGetKnownFolderPath (GUID -> modelled path). The folder path is environment-dependent
 * so only the contract is checked (S_OK + a C: path; unknown GUID -> failure). */
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>

int main(void) {
    HANDLE h = CreateFileW(L"aretfi.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr; WriteFile(h, "hello", 5, &wr, NULL);
    CloseHandle(h);
    h = CreateFileW(L"aretfi.dat", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    FILE_STANDARD_INFO fsi;
    BOOL ok = GetFileInformationByHandleEx(h, FileStandardInfo, &fsi, sizeof fsi);
    printf("fileinfo_ok=%d eof=%lld dir=%d\n", ok, (long long)fsi.EndOfFile.QuadPart, fsi.Directory);
    CloseHandle(h);
    DeleteFileW(L"aretfi.dat");

    PWSTR path = NULL;
    HRESULT hr = SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &path);
    printf("kf_ok=%d drive=%c\n", (hr == S_OK && path != NULL), path ? (char)path[0] : '?');
    if (path) CoTaskMemFree(path);

    GUID bogus = {0};
    path = NULL;
    hr = SHGetKnownFolderPath(&bogus, 0, NULL, &path);
    printf("kf_bogus_failed=%d\n", hr != S_OK);
    if (path) CoTaskMemFree(path);

    printf("done\n");
    return 0;
}
