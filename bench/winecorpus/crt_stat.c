/* msvcrt stat-family ABI guard. `_stat`/`_fstat`/`_stati64` must marshal a Linux
 * stat into msvcrt's FIXED struct layout — which differs from a natural i386
 * struct (MSVC 8-byte-aligns `__int64 st_size`, i386 SysV 4-byte-aligns it), so a
 * wrong layout silently misreads the size. We check the fields programs actually
 * rely on and that are deterministic across hosts: st_size (exact) and the
 * file-type classification (regular vs directory). dev/ino/uid/gid/timestamps are
 * host-specific and intentionally not compared. Ground truth = the same PE under
 * Wine; every printed line must match bit-for-bit. */
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <io.h>

int main(void) {
    const char *fn = "aret_stat_probe.tmp";
    FILE *f = fopen(fn, "wb");
    if (!f) { printf("open-for-write FAILED\n"); return 1; }
    fwrite("hello, stat world!", 1, 18, f); /* exactly 18 bytes */
    fclose(f);

    struct _stat s;
    if (_stat(fn, &s) == 0)
        printf("_stat    size=%ld reg=%d dir=%d\n", (long)s.st_size,
               (s.st_mode & _S_IFREG) != 0, (s.st_mode & _S_IFDIR) != 0);
    else printf("_stat FAILED\n");

    struct _stati64 s64;
    if (_stati64(fn, &s64) == 0)
        printf("_stati64 size=%lld reg=%d dir=%d\n", (long long)s64.st_size,
               (s64.st_mode & _S_IFREG) != 0, (s64.st_mode & _S_IFDIR) != 0);
    else printf("_stati64 FAILED\n");

    int fd = _open(fn, _O_RDONLY);
    struct _stat fs;
    if (fd >= 0 && _fstat(fd, &fs) == 0)
        printf("_fstat   size=%ld reg=%d dir=%d\n", (long)fs.st_size,
               (fs.st_mode & _S_IFREG) != 0, (fs.st_mode & _S_IFDIR) != 0);
    else printf("_fstat FAILED\n");
    if (fd >= 0) _close(fd);

    /* directory must classify as a directory, not a regular file */
    struct _stat ds;
    if (_stat(".", &ds) == 0)
        printf("_stat(.) reg=%d dir=%d\n",
               (ds.st_mode & _S_IFREG) != 0, (ds.st_mode & _S_IFDIR) != 0);
    else printf("_stat(.) FAILED\n");

    remove(fn);
    return 0;
}
