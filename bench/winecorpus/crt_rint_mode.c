/* Cross-function rounding mode: fesetround() (a call) sets the x87 rounding mode,
 * then an INLINED rint() (compiled to a bare `frndint`, no local fldcw) must honour
 * it. ARET routes a bare frndint to its runtime x87 model, which reads the live
 * __x87rt_rc a prior fesetround installed — so the mode set in one function is seen
 * by frndint in another. Volatile inputs defeat compile-time folding. Deterministic;
 * differential against the Wine/Windows oracle. */
#include <stdio.h>
#include <fenv.h>
#include <math.h>
int main(void){
    volatile double a = 2.5, c = 2.9, e = -2.5;
    fesetround(FE_UPWARD);
    printf("up:   rint(2.5)=%+.1f rint(2.9)=%+.1f rint(-2.5)=%+.1f\n", rint(a), rint(c), rint(e));
    fesetround(FE_DOWNWARD);
    printf("down: rint(2.5)=%+.1f rint(2.9)=%+.1f rint(-2.5)=%+.1f\n", rint(a), rint(c), rint(e));
    fesetround(FE_TOWARDZERO);
    printf("zero: rint(2.5)=%+.1f rint(2.9)=%+.1f rint(-2.5)=%+.1f\n", rint(a), rint(c), rint(e));
    fesetround(FE_TONEAREST);
    printf("near: rint(2.5)=%+.1f rint(2.9)=%+.1f rint(-2.5)=%+.1f\n", rint(a), rint(c), rint(e));
    printf("done\n");
    return 0;
}
