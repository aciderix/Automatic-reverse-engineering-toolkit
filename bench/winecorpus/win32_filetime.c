/* File-metadata group guard: SetEndOfFile, SetFileTime/GetFileTime, and the
 * Local<->UTC FILETIME conversions. Validated by round-trip, which is
 * deterministic and host-independent: a write time set on a 1-second boundary
 * (so filesystem granularity is irrelevant) reads back exactly; SetEndOfFile
 * truncates to the current position; Local<->File is identity under the harness's
 * UTC timezone. All printed values are booleans/counts — identical under Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    /* create a 100-byte file */
    HANDLE h = CreateFileA("aret_ft_probe.tmp", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("create FAILED\n"); return 1; }
    char zero[100] = {0};
    DWORD wr = 0;
    WriteFile(h, zero, 100, &wr, NULL);

    /* SetEndOfFile at position 40 -> file becomes 40 bytes */
    SetFilePointer(h, 40, NULL, FILE_BEGIN);
    BOOL eof = SetEndOfFile(h);
    DWORD sz = GetFileSize(h, NULL);
    printf("seteof ok=%d size=%lu\n", eof ? 1 : 0, (unsigned long)sz);

    /* SetFileTime to a known instant (2021-06-01 00:00:00 UTC), then read back */
    SYSTEMTIME s = {0};
    s.wYear = 2021; s.wMonth = 6; s.wDay = 1;
    FILETIME ft;
    SystemTimeToFileTime(&s, &ft);
    BOOL st = SetFileTime(h, NULL, NULL, &ft);
    FILETIME rc = {0}, ra = {0}, rw = {0};
    BOOL gt = GetFileTime(h, &rc, &ra, &rw);
    int same = (rw.dwLowDateTime == ft.dwLowDateTime && rw.dwHighDateTime == ft.dwHighDateTime);
    printf("settime ok=%d gettime ok=%d write_roundtrip=%d\n", st ? 1 : 0, gt ? 1 : 0, same);
    CloseHandle(h);

    /* Local<->File FILETIME round-trip (identity under UTC) */
    FILETIME lf = {0}, back = {0};
    FileTimeToLocalFileTime(&ft, &lf);
    LocalFileTimeToFileTime(&lf, &back);
    int rt = (back.dwLowDateTime == ft.dwLowDateTime && back.dwHighDateTime == ft.dwHighDateTime);
    printf("filetime local roundtrip=%d\n", rt);

    DeleteFileA("aret_ft_probe.tmp");
    return 0;
}
