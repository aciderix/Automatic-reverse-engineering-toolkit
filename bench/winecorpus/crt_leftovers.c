/* HLE mop-up of the remaining measured OS-wall leftovers (doc 82): _ultoa/_ltoa/_itoa,
 * MB_CUR_MAX (_p___mb_cur_max), _aligned_malloc/_aligned_free, GetTickCount64,
 * SetSystemTime, _wutime. Prints only deterministic facts so ARET and Wine match. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <wchar.h>

int main(void) {
    char b[64];
    printf("ultoa=%s\n", _ultoa(4294967295UL, b, 16));   /* ffffffff */
    printf("ltoa=%s\n",  _ltoa(-42, b, 10));              /* -42      */
    printf("itoa=%s\n",  _itoa(255, b, 2));               /* 11111111 */
    printf("mbcurmax=%d\n", (int)MB_CUR_MAX);             /* 1        */

    void *p = _aligned_malloc(100, 64);
    printf("aligned=%d\n", (int)(p != NULL && ((uintptr_t)p % 64) == 0));
    if (p) { memset(p, 0xAB, 100); printf("algbyte=%d\n", ((unsigned char *)p)[99]); _aligned_free(p); }

    printf("tick64nz=%d\n", (int)(GetTickCount64() > 0));

    SYSTEMTIME st; GetSystemTime(&st);
    printf("setsystime=%d\n", SetSystemTime(&st) != 0);   /* 1 (sandboxed, like Wine) */

    /* _wutime: set a file's mtime to a fixed epoch, read it back via _wstat64 */
    FILE *f = fopen("aretut.txt", "wb"); fputs("x", f); fclose(f);
    struct _utimbuf tb; tb.actime = 1000000000; tb.modtime = 1000000000;
    int ur = _wutime(L"aretut.txt", &tb);
    struct _stat64 s; int sr = _wstat64(L"aretut.txt", &s);
    printf("wutime=%d stat=%d mtime=%lld\n", ur, sr, (long long)s.st_mtime);
    _wunlink(L"aretut.txt");

    printf("done\n");
    return 0;
}
