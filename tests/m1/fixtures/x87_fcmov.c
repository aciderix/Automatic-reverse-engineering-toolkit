/* Exercises the x87 conditional move family (fcmovcc). At -O2 the compiler turns
   a double ternary `a>b?a:b` into `fucomi` + `fcmov`, and the float `printf`
   conversion (mingw's dtoa) uses `fxam`/`fcmove` too. fcmovcc selects st(0) from
   st(i) based on the EFLAGS a prior `fucomi`/`cmp` set.

   iced's `condition_code()` does NOT cover FCMOVcc (it returns None), so a naive
   lift made the move unconditional -> silently wrong results. The `%d` columns
   confirm the *value* is right; the `%f` columns confirm the float-formatting
   path (which itself runs fcmov inside dtoa) is right too. Expected:
     F mx=7.0 mn=-2.5 | I mx7=1 mn=1 */
#include <stdio.h>

volatile double A = 3.0, B = 7.0, C = -2.5, D = 4.0;

int main(void) {
    double a = A, b = B, c = C, d = D;
    double mx = a > b ? a : b;   /* fucomi + fcmov  -> 7.0  */
    double mn = c < d ? c : d;   /* fucomi + fcmov  -> -2.5 */
    printf("F mx=%.1f mn=%.1f | I mx7=%d mn=%d\n", mx, mn, mx == 7.0, mn == -2.5);
    return 0;
}
