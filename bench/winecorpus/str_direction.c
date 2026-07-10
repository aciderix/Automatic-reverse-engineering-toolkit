/* Exercises the direction flag (DF) across the whole string family — the
 * `std`/`cld` + backward `rep movs`/`stos`/`scas`/`cmps` and non-rep idioms that
 * old code uses (memmove's overlap copy, strrchr's reverse scan). Raw inline asm
 * forces each exact form in both directions. Wine runs the real instructions on
 * the CPU (with real DF); ARET runs the lifted DF-aware helpers. All output is
 * deterministic -> checkable bit-for-bit vs Wine. A wrong direction here is silent
 * memory corruption, so this is the load-bearing oracle for the DF model. */
#include <stdio.h>
#include <string.h>

/* Backward rep movsb: overlap-safe move of n bytes, copying from the top down
 * (esi/edi at the last byte, std). This is memmove's dst>src path. */
static void back_movsb(char *dst, const char *src, unsigned n) {
    const char *s = src + n - 1;
    char *d = dst + n - 1;
    __asm__ __volatile__("std\n\trep movsb\n\tcld"
        : "+S"(s), "+D"(d), "+c"(n) : : "memory");
}
/* Forward rep movsb for comparison. */
static void fwd_movsb(char *dst, const char *src, unsigned n) {
    __asm__ __volatile__("cld\n\trep movsb"
        : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
}
/* Backward rep stosb: fill n bytes going down starting at `end`. */
static void back_stosb(char *end, char val, unsigned n) {
    char *p = end;
    __asm__ __volatile__("std\n\trep stosb\n\tcld"
        : "+D"(p), "+c"(n) : "a"(val) : "memory");
}
/* Backward repne scasb: reverse scan for `c` from the end (strrchr idiom).
 * Returns the number of bytes scanned until the match (or count exhausts). */
static unsigned back_scasb(const char *end, char c, unsigned n) {
    const char *p = end;
    unsigned cnt = n;
    __asm__ __volatile__("std\n\trepne scasb\n\tcld"
        : "+D"(p), "+c"(cnt) : "a"(c) : "cc", "memory");
    return n - cnt;
}
/* Backward repe cmpsb: compare two buffers from the end while equal. */
static unsigned back_cmpsb(const char *ae, const char *be, unsigned n) {
    unsigned cnt = n;
    __asm__ __volatile__("std\n\trepe cmpsb\n\tcld"
        : "+S"(ae), "+D"(be), "+c"(cnt) : : "cc", "memory");
    return cnt;
}
/* Non-rep with DF=1: a single lodsb/stosb pair going backward. */
static void back_lods_stos(char *dst, const char *src, unsigned n) {
    /* copy reversed: read src forward with lodsb, write dst backward with stosb */
    const char *s = src;
    char *d = dst + n - 1;
    for (unsigned i = 0; i < n; i++) {
        char al;
        __asm__ __volatile__("cld\n\tlodsb" : "=a"(al), "+S"(s) : : "memory");
        __asm__ __volatile__("std\n\tstosb\n\tcld" : "+D"(d) : "a"(al) : "memory");
    }
}

int main(void) {
    char buf[32];

    /* Backward overlapping move: shift "0123456789" right by 3 in place. */
    strcpy(buf, "0123456789");
    back_movsb(buf + 3, buf, 10);
    buf[13] = 0;
    printf("back_movsb=%s\n", buf);

    /* Forward move (non-overlap) for contrast. */
    char a[16] = "SOURCE", b[16] = "xxxxxxxx";
    fwd_movsb(b, a, 7);
    printf("fwd_movsb=%s\n", b);

    /* Backward fill: last 5 bytes of a cleared buffer set to '*'. */
    memset(buf, '.', 12); buf[12] = 0;
    back_stosb(buf + 11, '*', 5);
    printf("back_stosb=%s\n", buf);

    /* Reverse scan: last 'l' in "hello world hello" (strrchr). */
    const char *hay = "hello world hello";
    unsigned len = (unsigned)strlen(hay);
    unsigned k = back_scasb(hay + len - 1, 'l', len);
    printf("back_scasb steps=%u (char at %u)\n", k, len - k);

    /* Reverse compare: two strings equal in the tail, differ earlier. */
    const char *s1 = "abcXYZ", *s2 = "defXYZ";
    unsigned rem = back_cmpsb(s1 + 5, s2 + 5, 6);
    printf("back_cmpsb rem=%u\n", rem);

    /* Non-rep backward stos + forward lods reversing a string. */
    back_lods_stos(buf, "REVERSE!", 8);
    buf[8] = 0;
    printf("reversed=%s\n", buf);
    return 0;
}
