/* Exercises the x87 `fxam` instruction: classify st(0) into the FPU condition
   codes (C3/C2/C0) and read them with `fnstsw ax`. This is the idiom mingw's
   `__sqrt`/`fpclassify` use to special-case NaN/Inf/zero/negative inputs. ARET
   must model `fxam` (not bail the whole function to opaque asm), so the mask
   `sw & 0x4500` yields the IEEE class for each input. */
#include <stdio.h>

volatile double v_zero = 0.0;
volatile double v_big = 1e300;
volatile double v_one = 1.0;

static unsigned cls(double x) {
    unsigned short sw;
    /* load x, classify, store status word to AX, then pop to balance the stack */
    __asm__ __volatile__("fldl %1\n\t"
                         "fxam\n\t"
                         "fnstsw %%ax\n\t"
                         "fstp %%st(0)"
                         : "=a"(sw)
                         : "m"(x)
                         : "st");
    return (unsigned)(sw & 0x4500);
}

int main(void) {
    double nan = v_zero / v_zero;  /* 0/0  -> NaN  : C0      = 0x0100 */
    double inf = v_big * v_big;    /* ovf  -> +Inf : C2|C0   = 0x0500 */
    printf("NAN=%u INF=%u ONE=%u ZERO=%u\n",
           cls(nan), cls(inf), cls(v_one), cls(v_zero));
    /* expect: NAN=256 INF=1280 ONE=1024 ZERO=16384 */
    return 0;
}
