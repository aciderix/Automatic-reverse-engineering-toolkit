/* Inlined getc/putc/ungetc over an fopen'd file. On a statically-linked msvcrt
 * CRT (mingw -O2) getc/putc expand to `--f->_cnt >= 0 ? *f->_ptr++ : _filbuf(f)`
 * (and _flsbuf for putc), reading the FILE's own msvcrt-layout fields directly.
 * A host glibc FILE (whose offset 0 is the 0xfbad… _IO_MAGIC, not _ptr) would be
 * dereferenced as a pointer and crash — the exact BusyBox `wc`/`sort` fault. This
 * guards that HLE FILEs are msvcrt-layout and that _filbuf/_flsbuf/ungetc work. */
#include <stdio.h>

int main(void) {
    FILE *w = fopen("wd_getc.txt", "wb");
    if (!w) { printf("OPENW_FAIL\n"); return 1; }
    for (int i = 0; i < 26; i++) putc('a' + i, w);   /* inlined putc -> _flsbuf */
    putc('\n', w);
    fclose(w);

    FILE *r = fopen("wd_getc.txt", "rb");
    if (!r) { printf("OPENR_FAIL\n"); return 1; }
    int c, n = 0; unsigned sum = 0;
    while ((c = getc(r)) != EOF) {                    /* inlined getc -> _filbuf */
        n++;
        sum = sum * 31u + (unsigned)c;
    }
    printf("read=%d sum=%u eof=%d\n", n, sum, feof(r) ? 1 : 0);

    /* ungetc composes with getc after a rewind. */
    rewind(r);
    c = getc(r);
    ungetc(c, r);
    int c2 = getc(r);
    printf("rewind_first=%c ungetc_ok=%d\n", c, c == c2);

    fclose(r);
    remove("wd_getc.txt");
    return 0;
}
