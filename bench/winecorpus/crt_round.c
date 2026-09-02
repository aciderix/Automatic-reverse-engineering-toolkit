/* libm rounding functions that mingw compiles to *lifted* x87 leaf routines
 * returning st(0) via fnstcw/frndint (nearbyint) — these bail to ARET's runtime
 * x87 model, so their result must be published to the fp-return channel from the
 * runtime stack, not a never-written static slot (the bug this fixture pins: a
 * runtime-mode fp-returning callee used to return 0.0 to a static caller).
 * Default rounding mode throughout; volatile inputs defeat compile-time folding.
 * Deterministic; differential against the Wine/Windows oracle. */
#include <stdio.h>
#include <math.h>
int main(void){
    volatile double v[] = { 2.5, 2.1, 2.9, -2.5, -2.1, 0.5, 3.5 };
    for (int i = 0; i < 7; i++) {
        double x = v[i];
        printf("x=%+.1f nearbyint=%+.1f rint=%+.1f floor=%+.1f ceil=%+.1f trunc=%+.1f round=%+.1f lrint=%+ld\n",
               x, nearbyint(x), rint(x), floor(x), ceil(x), trunc(x), round(x), lrint(x));
    }
    printf("done\n");
    return 0;
}
