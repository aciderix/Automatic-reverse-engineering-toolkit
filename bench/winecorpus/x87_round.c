/* x87 rounding-mode + transcendental guard. The C `ceil`/`floor`/`trunc` a real
 * MSVC CRT ships install a rounding mode into the x87 control word before
 * `frndint`: `mov reg,IMM; or reg,[oldcw]; and reg,MASK; mov [new]; fldcw [new];
 * frndint`. ARET must PROVE that idiom's RC bits (10-11) from the or/and
 * immediates — not default to round-to-nearest, which ships a silent-wrong
 * ceil(3.2)=3 (a bug a narrow ceil(3.0) test misses, since nearest and up agree
 * on integers). Here we reproduce the idiom in inline asm so the lift must decode
 * it, and also exercise `fpatan` (atan2) and the `fprem` completion loop (fmod)
 * that the tiny leaf CRT helpers use. All output must match Wine bit-for-bit. */
#include <stdio.h>

/* frndint under an explicit RC field, installed via the MSVC control-word idiom:
 * or-in the RC one-bit, and-out the other — forcing bits 10-11 regardless of the
 * saved word. rc: 0 nearest, 1 down(floor), 2 up(ceil), 3 truncate. */
static double rnd_mode(double x, int rc) {
    unsigned short saved, cw;
    unsigned or_bits = (unsigned)rc << 10;      /* set the wanted RC bits */
    unsigned and_mask = ~((~(unsigned)rc & 3u) << 10); /* clear the other RC bit */
    double r;
    __asm__ volatile(
        "fnstcw %1\n\t"           /* save current control word            */
        "movzwl %1, %%eax\n\t"    /* eax = old cw                         */
        "orl %3, %%eax\n\t"       /* force the RC one-bits                */
        "andl %4, %%eax\n\t"      /* clear the RC zero-bit                */
        "movw %%ax, %2\n\t"
        "fldcw %2\n\t"            /* install the mode                    */
        "frndint\n\t"            /* round st0 under it                  */
        "fldcw %1\n\t"           /* restore                             */
        : "=t"(r), "=m"(saved), "=m"(cw)
        : "g"(or_bits), "g"(and_mask), "0"(x)
        : "eax");
    return r;
}
static double x87_atan2(double y, double x) {
    double r;
    __asm__ volatile("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}
static double x87_fmod(double a, double b) {
    double r;
    __asm__ volatile(
        "1:\n\t fprem\n\t fnstsw %%ax\n\t sahf\n\t jp 1b\n\t"
        : "=t"(r) : "0"(a), "u"(b) : "ax", "cc");
    return r;
}

int main(void) {
    double xs[] = {3.2, 3.8, -3.2, -3.8, 2.5, -2.5, 7.0, 0.0};
    for (int i = 0; i < 8; i++) {
        double x = xs[i];
        printf("x=%.2f ceil=%.1f floor=%.1f trunc=%.1f near=%.1f\n",
               x, rnd_mode(x, 2), rnd_mode(x, 1), rnd_mode(x, 3), rnd_mode(x, 0));
    }
    printf("atan2(1,2)=%.15f atan2(-1,2)=%.15f\n", x87_atan2(1.0, 2.0), x87_atan2(-1.0, 2.0));
    printf("fmod(7,3)=%.6f fmod(7.5,2)=%.6f fmod(-7,3)=%.6f\n",
           x87_fmod(7.0, 3.0), x87_fmod(7.5, 2.0), x87_fmod(-7.0, 3.0));
    return 0;
}
