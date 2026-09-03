/* GetFinalPathNameByHandleA/W: a HANDLE is an fd in ARET, so the final path comes from
 * /proc/self/fd reverse-translated to the guest Windows path. Uses an explicit C:\ path
 * so the guest-visible result is identical under Wine and ARET (both echo \\?\C:\...),
 * independent of where each maps drive_c on the host. Covers the buffer-fits return
 * (length excl. NUL), the too-small buffer return, and the wide variant. */
#include <windows.h>
#include <stdio.h>
int main(void){
    HANDLE h = CreateFileA("C:\\aret_gfpn.txt", GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h==INVALID_HANDLE_VALUE){ printf("open fail %lu\n",(unsigned long)GetLastError()); return 1; }
    char a[512]; DWORD na = GetFinalPathNameByHandleA(h, a, sizeof a, 0);
    printf("A n=%lu path=[%s]\n",(unsigned long)na, a);
    char sb[4]; DWORD ns = GetFinalPathNameByHandleA(h, sb, sizeof sb, 0);
    printf("A small=%lu\n",(unsigned long)ns);
    wchar_t w[512]; DWORD nw = GetFinalPathNameByHandleW(h, w, 512, 0);
    printf("W n=%lu match=%d\n",(unsigned long)nw, (nw==na));
    CloseHandle(h);
    DeleteFileA("C:\\aret_gfpn.txt");
    return 0;
}
