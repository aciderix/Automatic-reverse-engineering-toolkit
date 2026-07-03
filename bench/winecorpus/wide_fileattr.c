/* Wide-char file layer + the GetLastError existence-probe contract.
 * Guards: GetVersionExA/AreFileApisANSI (faithful values), GetFullPathNameW,
 * GetFileAttributesExW, and — the key one — GetFileAttributesW on a missing
 * file must set GetLastError()==ERROR_FILE_NOT_FOUND so callers (sqlite's
 * winAccess) tell "absent" from a real access error. Only host-stable facts are
 * printed (platform id, sizes, the basename component, the last-error code). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    OSVERSIONINFOA ov;
    ov.dwOSVersionInfoSize = sizeof ov;
    int gv = GetVersionExA(&ov);
    printf("getver=%d platform=%lu ansi=%d\n",
           gv, (unsigned long)ov.dwPlatformId, (int)AreFileApisANSI());

    HANDLE f = CreateFileW(L"wfa.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, "hello", 5, &wr, NULL); CloseHandle(f); }

    WIN32_FILE_ATTRIBUTE_DATA ad;
    ZeroMemory(&ad, sizeof ad);
    int okex = GetFileAttributesExW(L"wfa.txt", GetFileExInfoStandard, &ad);
    printf("ex=%d exlow=%lu\n", okex, (unsigned long)ad.nFileSizeLow);

    WCHAR full[MAX_PATH];
    WCHAR *part = NULL;
    DWORD n = GetFullPathNameW(L"wfa.txt", MAX_PATH, full, &part);
    int partok = (part != NULL && wcscmp(part, L"wfa.txt") == 0);
    printf("fullok=%d partok=%d\n", (n > 0), partok);

    /* The contract: a missing file -> INVALID + ERROR_FILE_NOT_FOUND (2). */
    SetLastError(0);
    DWORD miss = GetFileAttributesW(L"no_such_wfa.txt");
    printf("missing=%d lasterr=%lu\n",
           (miss == INVALID_FILE_ATTRIBUTES), (unsigned long)GetLastError());

    DeleteFileW(L"wfa.txt");
    return 0;
}
