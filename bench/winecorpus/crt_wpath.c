/* HLE family: msvcrt WIDE-CHAR file/dir CRT (`_w*`). Measured (doc 82, 2026-08-15)
 * as the next wall after the C++ runtime lifts. Each `_w*` is its narrow twin with
 * a UTF-16 -> UTF-8 path conversion; this exercises the family end-to-end on a temp
 * subdir and prints only DETERMINISTIC facts (return codes, a known file size, the
 * enumerated name) so ARET and Wine (same msvcrt beside the exe) match bit-for-bit.
 * No times / absolute paths are printed (env-dependent). */
#include <stdio.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wchar.h>

int main(void) {
    /* clean any leftover from a previous run (ignore errors) */
    _wunlink(L"aretwd\\hello.txt");
    _wrmdir(L"aretwd");

    printf("mkdir=%d\n", _wmkdir(L"aretwd"));

    int fd = _wopen(L"aretwd\\hello.txt", _O_CREAT | _O_WRONLY | _O_TRUNC, _S_IREAD | _S_IWRITE);
    printf("open>=0=%d\n", fd >= 0);
    if (fd >= 0) { _write(fd, "12345", 5); _close(fd); }

    struct _stat64 st;
    int sr = _wstat64(L"aretwd\\hello.txt", &st);
    printf("stat=%d size=%lld\n", sr, (long long)st.st_size);

    printf("access_ok=%d\n", _waccess(L"aretwd\\hello.txt", 0));   /* exists -> 0  */
    printf("access_no=%d\n", _waccess(L"aretwd\\nope.txt", 0));    /* absent -> -1 */

    struct _wfinddata32_t fdat;
    intptr_t h = _wfindfirst32(L"aretwd\\*.txt", &fdat);
    printf("find>=0=%d\n", h >= 0);
    if (h >= 0) {
        printf("found=%ls size=%u\n", fdat.name, (unsigned)fdat.size);
        printf("findnext_end=%d\n", _wfindnext32(h, &fdat)); /* only one file -> -1 */
        _findclose(h);
    }

    printf("chdir=%d\n", _wchdir(L"aretwd"));
    printf("access_rel=%d\n", _waccess(L"hello.txt", 0));          /* relative after chdir -> 0 */
    printf("chdir_back=%d\n", _wchdir(L".."));

    printf("chmod=%d\n", _wchmod(L"aretwd\\hello.txt", _S_IREAD | _S_IWRITE));
    printf("unlink=%d\n", _wunlink(L"aretwd\\hello.txt"));
    printf("rmdir=%d\n", _wrmdir(L"aretwd"));
    printf("done\n");
    return 0;
}
