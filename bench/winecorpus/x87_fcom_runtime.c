/* Guard for the runtime-x87 `fucom st(i)` operand. When a function's static x87
 * depth analysis bails, its x87 ops lower onto the runtime x87 stack
 * (lift_x87_runtime). This exercises the classic float compare-to-zero idiom
 * `fldz; fxch st(1); fucom st(1); fnstsw ax; sahf; sete` through that path — a
 * preceding unmodelled `fldl2e` forces the runtime fallback. iced models
 * `fucom st(1)` as two operands (implicit ST0 + ST1); reading op0 gave ST0, so
 * the lifter compared st(0) with itself (always equal) and every `x == 0` test
 * succeeded — BusyBox `awk`'s `a/b` then always raised "Division by zero". The
 * lifter must read the last operand (ST1), matching the static path. */
#include <stdio.h>

static int is_zero(double x) {
    unsigned char eq;
    __asm__ volatile(
        "fldl2e\n\t fstp %%st(0)\n\t"   /* unmodelled op -> runtime x87 fallback */
        "fldz\n\t"                       /* st0=0, st1=x */
        "fxch %%st(1)\n\t"               /* st0=x, st1=0 */
        "fucom %%st(1)\n\t"              /* compare x vs 0 (the operand under test) */
        "fnstsw %%ax\n\t"
        "fstp %%st(1)\n\t"
        "sahf\n\t"
        "sete %0\n\t"
        : "=q"(eq) : "t"(x) : "ax", "cc");
    return eq;
}

int main(int argc, char **argv) {
    (void)argv;
    double v = 7.0 + (argc - 1); /* 7.0, not const-folded away */
    printf("is_zero(%.0f)=%d is_zero(0)=%d\n", v, is_zero(v), is_zero(0.0));
    return 0;
}
