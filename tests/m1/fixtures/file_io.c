/* Round-trips a file through the CRT stdio layer: fopen("w") + fputs + fclose,
   then fopen("r") + fread + fclose + remove. mingw's stdio bottoms out in the
   low-level msvcrt I/O imports (_open/_read/_write/_close), so this exercises the
   GetFileAttributesA / _open / _lseek HLE shims. Single-line output (the runner
   prefixes each line) — expected: FILEIO n=12 a=line1 b=line2 */
#include <stdio.h>
#include <string.h>

int main(void) {
    FILE *f = fopen("aret_fileio_tmp.txt", "w");
    if (!f) { printf("ERR open-w\n"); return 1; }
    fputs("line1\nline2\n", f);
    fclose(f);

    FILE *g = fopen("aret_fileio_tmp.txt", "r");
    if (!g) { printf("ERR open-r\n"); return 1; }
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1, g);
    buf[n] = 0;
    fclose(g);
    remove("aret_fileio_tmp.txt");

    /* "line1\nline2\n": first token at buf[0..5], second at buf[6..11] */
    printf("FILEIO n=%zu a=%.5s b=%.5s\n", n, buf, buf + 6);
    return 0;
}
