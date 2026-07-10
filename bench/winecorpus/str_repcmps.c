/* Exercises the `rep(ne) cmps` string-compare lift (the memcmp/strcmp idiom that
 * old MSVC/Borland inline as `repe cmpsb; je …`). Uses raw inline asm so the
 * instruction is actually emitted (modern libc memcmp would be host-backed and
 * bypass the lifter). Wine runs the real `repe/repne cmps` on the CPU, ARET runs
 * the lifted `__rep_cmps*` + flag derivation — the final ecx/esi/edi and the
 * ZF/CF/SF the compare sets must agree. Deterministic output → checkable bit-for-
 * bit vs Wine. Covers repe (stop on mismatch) and repne (stop on match), byte /
 * word / dword element sizes, equal and differing buffers, and the count. */
#include <stdio.h>
#include <string.h>

/* repe cmpsb over n bytes: returns the sign of the first differing pair
 * (memcmp semantics), plus the remaining count via *rem. */
static int repe_cmpsb(const void *a, const void *b, unsigned n, unsigned *rem) {
    int res; unsigned ecx = n; const unsigned char *s1 = a, *s2 = b;
    __asm__ __volatile__(
        "cld\n\t"
        "repe cmpsb\n\t"
        "seta %%al\n\t"          /* CF/ZF after cmps: above -> 1 */
        "setb %%bl\n\t"          /* below -> 1 */
        "movzbl %%al,%%eax\n\t"
        "movzbl %%bl,%%ebx\n\t"
        "subl %%ebx,%%eax"       /* +1 if a>b, -1 if a<b, 0 if equal */
        : "=a"(res), "+c"(ecx), "+S"(s1), "+D"(s2)
        : : "ebx", "cc", "memory");
    *rem = ecx;
    return res;
}

/* repne cmpsw: scan word pairs until a MATCH or count exhausts; return remaining. */
static unsigned repne_cmpsw(const void *a, const void *b, unsigned nwords) {
    unsigned ecx = nwords; const unsigned short *s1 = a, *s2 = b;
    __asm__ __volatile__("cld\n\trepne cmpsw"
        : "+c"(ecx), "+S"(s1), "+D"(s2) : : "cc", "memory");
    return ecx;
}

/* repe cmpsd over dwords: returns remaining count (0 => all equal). */
static unsigned repe_cmpsd(const void *a, const void *b, unsigned nd) {
    unsigned ecx = nd; const unsigned *s1 = a, *s2 = b;
    __asm__ __volatile__("cld\n\trepe cmpsd"
        : "+c"(ecx), "+S"(s1), "+D"(s2) : : "cc", "memory");
    return ecx;
}

int main(void) {
    unsigned rem;
    const char *x = "hello world";
    const char *y = "hello WORLD";   /* differs at index 6 ('w' vs 'W') */
    const char *z = "hello world";   /* equal to x */

    int r1 = repe_cmpsb(x, z, 11, &rem);
    printf("cmpsb equal: sign=%d rem=%u\n", r1, rem);
    int r2 = repe_cmpsb(x, y, 11, &rem);
    printf("cmpsb diff:  sign=%d rem=%u\n", r2, rem);   /* 'w'(0x77) > 'W'(0x57) -> +1, rem=4 */
    int r3 = repe_cmpsb(y, x, 11, &rem);
    printf("cmpsb rdiff: sign=%d rem=%u\n", r3, rem);   /* -1 */

    /* word compare: two arrays, differ in the 3rd word */
    unsigned short wa[4] = {1, 2, 3, 4}, wb[4] = {1, 2, 9, 4};
    printf("cmpsw repne rem=%u\n", repne_cmpsw(wa, wb, 4));

    /* dword compare: equal then differing */
    unsigned da[3] = {0x11111111u, 0x22222222u, 0x33333333u};
    unsigned db1[3] = {0x11111111u, 0x22222222u, 0x33333333u};
    unsigned db2[3] = {0x11111111u, 0xdeadbeefu, 0x33333333u};
    printf("cmpsd eq rem=%u  ne rem=%u\n", repe_cmpsd(da, db1, 3), repe_cmpsd(da, db2, 3));
    return 0;
}
