/* fread + feof idiom — regression guard for the CRT bug where aret_fread hit EOF during
 * a bulk read but did NOT set the stream's _IOEOF flag. A program that reads a block
 * larger than what remains and then calls feof() to tell a short-read-at-EOF from a real
 * error saw a short read with feof()==0 and treated it as failure — e.g. zstd:
 * "Unexpected short read". Deterministic, self-contained (a temp file in cwd), no DLLs. */
#include <stdio.h>

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    const char *path = "crt_fread_eof.tmp";
    FILE *w = fopen(path, "wb");
    if (!w) { printf("open-write-fail\n"); return 1; }
    for (int i = 0; i < 100; i++) fwrite("0123456789ABCDEF", 1, 16, w);  /* 1600 bytes */
    fclose(w);

    FILE *r = fopen(path, "rb");
    if (!r) { printf("open-read-fail\n"); return 1; }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, r);        /* ask 64K, file is 1600 -> short read + EOF */
    /* Normalise feof/ferror to a boolean: C only guarantees "nonzero" at EOF, and the exact
     * value is implementation-defined (msvcrt 0x10 vs Wine 1). The CONTRACT under test is
     * "EOF detected after a short bulk read", which is what a real program (zstd) checks. */
    printf("read=%u eof=%d err=%d\n", (unsigned)n, feof(r) ? 1 : 0, ferror(r) ? 1 : 0);
    size_t n2 = fread(buf, 1, 16, r);               /* now firmly at EOF */
    printf("read2=%u eof2=%d\n", (unsigned)n2, feof(r) ? 1 : 0);
    fclose(r);
    remove(path);
    printf("done\n");
    return 0;
}
