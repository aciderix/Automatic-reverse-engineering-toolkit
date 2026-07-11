/* Exercises the Win16-era file API (_lopen/_lcreat/_lclose/_lread/_lwrite/_llseek)
 * + lstrcpynA, all POSIX-mapped in the HLE. A file round-trip (create, write,
 * reopen, seek, read) is deterministic and checkable bit-for-bit vs Wine. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    HFILE f = _lcreat("w16.dat", 0);
    printf("creat ok=%d\n", f != HFILE_ERROR);
    const char *msg = "Hello Win16 file API!";
    LONG w = _lwrite(f, msg, (UINT)strlen(msg));
    printf("write=%ld\n", w);
    _lclose(f);

    f = _lopen("w16.dat", OF_READ);
    printf("open ok=%d\n", f != HFILE_ERROR);
    LONG pos = _llseek(f, 6, 0);                 /* SEEK_SET -> "Win16..." */
    printf("seek=%ld\n", pos);
    char buf[32]; memset(buf, 0, sizeof buf);
    LONG r = _lread(f, buf, 5);
    printf("read=%ld [%s]\n", r, buf);
    LONG end = _llseek(f, 0, 2);                 /* SEEK_END */
    printf("end=%ld\n", end);
    _lclose(f);

    char dst[8]; lstrcpynA(dst, "abcdefghij", 5);
    printf("cpyn=[%s] len=%d\n", dst, (int)strlen(dst));
    printf("done\n");
    return 0;
}
