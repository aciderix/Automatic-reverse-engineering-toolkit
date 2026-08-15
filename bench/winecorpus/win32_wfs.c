/* HLE family: Win32 wide-char filesystem/volume surface (`*W`). Measured (doc 82,
 * 2026-08-15) as part of the OS wall after the C++ runtime lifts. Exercises
 * RemoveDirectoryW, CreateHardLinkW, GetVolumeInformationW, GetDiskFreeSpaceExW and
 * prints only DETERMINISTIC facts (return codes, the linked file's content, a
 * success bool + the avail<=total invariant) so ARET and Wine match bit-for-bit;
 * volume names / raw byte counts are env-dependent and never printed. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    DeleteFileW(L"aretA.txt"); DeleteFileW(L"aretB.txt"); RemoveDirectoryW(L"aretdir");

    CreateDirectoryW(L"aretdir", NULL);
    printf("rmdir=%d\n", RemoveDirectoryW(L"aretdir") != 0);

    HANDLE h = CreateFileW(L"aretA.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr = 0; WriteFile(h, "12345", 5, &wr, NULL); CloseHandle(h);
    printf("hardlink=%d\n", CreateHardLinkW(L"aretB.txt", L"aretA.txt", NULL) != 0);

    HANDLE hb = CreateFileW(L"aretB.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    char buf[8] = {0}; DWORD rd = 0; ReadFile(hb, buf, 5, &rd, NULL); CloseHandle(hb);
    printf("linkread=%.5s rd=%lu\n", buf, (unsigned long)rd);

    printf("volinfo=%d\n", GetVolumeInformationW(NULL, NULL, 0, NULL, NULL, NULL, NULL, 0) != 0);

    ULARGE_INTEGER avail, total, freeb;
    BOOL dr = GetDiskFreeSpaceExW(L".", &avail, &total, &freeb);
    printf("disk=%d inv=%d\n", dr != 0, (int)(avail.QuadPart <= total.QuadPart));

    DeleteFileW(L"aretA.txt"); DeleteFileW(L"aretB.txt");
    printf("done\n");
    return 0;
}
