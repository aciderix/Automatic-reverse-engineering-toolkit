/* General CRT mop-up measured on ninja's real-build import wall (doc 82): _sopen
 * (share-mode open, same file semantics as _open) and isleadbyte (0 in the C locale,
 * like IsDBCSLeadByte/_ismbblead). Prints only deterministic facts so ARET and Wine
 * match bit-for-bit. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include <ctype.h>

int main(void) {
    /* _sopen: create + write, then _sopen for read and read back. */
    int fd = _sopen("aretso.txt", _O_CREAT | _O_TRUNC | _O_WRONLY, _SH_DENYWR, _S_IREAD | _S_IWRITE);
    printf("sopen_w_ok=%d\n", fd >= 0);
    if (fd >= 0) { _write(fd, "hello", 5); _close(fd); }

    int rd = _sopen("aretso.txt", _O_RDONLY, _SH_DENYNO);
    printf("sopen_r_ok=%d\n", rd >= 0);
    char buf[8] = {0};
    int n = rd >= 0 ? _read(rd, buf, 5) : -1;
    if (rd >= 0) _close(rd);
    printf("sopen_read=%d buf=%s\n", n, buf);           /* 5 hello */

    int miss = _sopen("aret_no_such_file.xyz", _O_RDONLY, _SH_DENYNO);
    printf("sopen_miss=%d\n", miss < 0);                 /* 1 */
    remove("aretso.txt");

    /* isleadbyte: single-byte C locale -> no byte is ever a lead byte. */
    int leads = 0;
    for (int c = 0; c < 256; c++) if (isleadbyte(c)) leads++;
    printf("isleadbyte_count=%d\n", leads);              /* 0 */

    printf("done\n");
    return 0;
}
