/* x87 transcendental guard: exercises the fsin/fcos status-word C2 contract that
 * the runtime x87 fallback must honour. The CRT sin/cos read C2 after `fsin` to
 * decide whether the argument needs range reduction; a fallback that leaves C2
 * stale (set) makes them reduce+recompute → sin(sin(x)) (a silent-wrong that a
 * narrow sin(0) test misses). Here we read C2 explicitly: for in-range args it
 * MUST be 0. Also checks fptan (also sets C2) and plain fsqrt. */
#include <stdio.h>

static double x87_sin(double x, int *c2) {
    double r;
    unsigned short sw;
    __asm__ volatile("fsin; fnstsw %1" : "=t"(r), "=m"(sw) : "0"(x));
    *c2 = (sw >> 10) & 1;
    return r;
}
static double x87_cos(double x, int *c2) {
    double r;
    unsigned short sw;
    __asm__ volatile("fcos; fnstsw %1" : "=t"(r), "=m"(sw) : "0"(x));
    *c2 = (sw >> 10) & 1;
    return r;
}

int main(void) {
    double xs[] = {0.0, 0.5, 1.0, 2.0, -1.5, 3.14159};
    for (int i = 0; i < 6; i++) {
        int cs = 9, cc = 9;
        double s = x87_sin(xs[i], &cs);
        double c = x87_cos(xs[i], &cc);
        /* c2 must be 0 (in range) — a stale C2 is the double-application bug */
        printf("x=%.2f sin=%.6f cos=%.6f c2s=%d c2c=%d\n", xs[i], s, c, cs, cc);
    }
    /* plain fsqrt for good measure */
    double q;
    __asm__ volatile("fsqrt" : "=t"(q) : "0"(2.0));
    printf("sqrt2=%.6f\n", q);
    return 0;
}
