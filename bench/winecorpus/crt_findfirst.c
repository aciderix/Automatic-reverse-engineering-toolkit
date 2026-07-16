/* _findfirst/_findnext/_findclose — msvcrt CRT directory iteration over
 * `struct _finddata_t`. Oracle: Wine. attrib is CRT-encoded (measured): regular
 * file=_A_ARCH(0x20), directory=_A_SUBDIR(0x10), read-only adds _A_RDONLY(0x01);
 * '.'/'..' enumerated. Times are env-dependent → not printed (deterministic). */
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int main(void) {
    _mkdir("ffcrt");
    struct { const char *n; int len; } items[] = {
        {"ffcrt/a.txt", 3}, {"ffcrt/bb.txt", 7}, {"ffcrt/c.dat", 0}, {"ffcrt/sub", -1}, {"ffcrt/ro.txt", 5},
    };
    for (int i = 0; i < 5; i++) {
        if (items[i].len < 0) { _mkdir(items[i].n); continue; }
        FILE *f = fopen(items[i].n, "wb");
        for (int k = 0; k < items[i].len; k++) fputc('x', f);
        fclose(f);
    }
    _chmod("ffcrt/ro.txt", _S_IREAD); /* read-only -> _A_RDONLY bit */

    struct _finddata_t fd;
    char lines[32][160]; int n = 0;
    intptr_t h = _findfirst("ffcrt/*", &fd);
    printf("first_valid=%d\n", h != -1);
    if (h != -1) {
        do {
            snprintf(lines[n++], 160, "%-8s attrib=%02x size=%lu", fd.name,
                     (unsigned)fd.attrib, (unsigned long)fd.size);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
    char *p[32]; for (int i = 0; i < n; i++) p[i] = lines[i];
    qsort(p, n, sizeof(char *), cmpstr);
    printf("count=%d\n", n);
    for (int i = 0; i < n; i++) printf("  %s\n", p[i]);

    /* case-insensitive wildcard subset */
    int m = 0; intptr_t h2 = _findfirst("ffcrt/*.TXT", &fd);
    if (h2 != -1) { do { m++; } while (_findnext(h2, &fd) == 0); _findclose(h2); }
    printf("txt_count=%d\n", m);

    /* no match -> handle -1, errno ENOENT */
    errno = 0;
    intptr_t h3 = _findfirst("ffcrt/none*", &fd);
    printf("nomatch=%d enoent=%d\n", h3 == -1, errno == ENOENT);

    _chmod("ffcrt/ro.txt", _S_IREAD | _S_IWRITE);
    _unlink("ffcrt/a.txt"); _unlink("ffcrt/bb.txt"); _unlink("ffcrt/c.dat"); _unlink("ffcrt/ro.txt");
    _rmdir("ffcrt/sub"); _rmdir("ffcrt");
    printf("done\n");
    return 0;
}
