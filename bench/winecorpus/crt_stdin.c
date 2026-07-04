/* Exercises the CRT stdin path over ARET's synthetic _iob stream: getchar() to
 * EOF (which the mingw macro inlines to the _filbuf buffer-refill primitive) and
 * fclose(stdin) on exit. Regression guard for two general bugs first surfaced by
 * running the real BusyBox-w32 under ARET:
 *   - _filbuf was an unimplemented weak stub returning 0 forever, so every stdin
 *     read loop (wc, sort, …) spun endlessly instead of stopping at EOF;
 *   - fclose() of a synthetic std stream dereferenced our 32-byte struct as a
 *     glibc FILE and segfaulted (BusyBox rev/nl crash on exit).
 * Input is fed from winecorpus/crt_stdin.in, identically to Wine and ARET. */
#include <stdio.h>

int main(void) {
    long bytes = 0, lines = 0, words = 0;
    int c, inword = 0;
    while ((c = getchar()) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\t' || c == '\n') {
            inword = 0;
        } else if (!inword) {
            inword = 1;
            words++;
        }
    }
    /* wc-style summary — a byte-exact function of the input on either engine.
     * (feof(stdin) is intentionally not asserted: ARET models a synthetic std
     * stream where end-of-input surfaces as getchar()==EOF, not via the FILE
     * flag — see aret_feof.) */
    printf("lines=%ld words=%ld bytes=%ld\n", lines, words, bytes);
    fclose(stdin);
    return 0;
}
