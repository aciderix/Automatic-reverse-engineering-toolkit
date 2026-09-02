/* C99 <fenv.h> rounding-mode + exception-env round-trip. mingw compiles
 * fesetround/fegetround to fldcw/fnstcw and fegetenv/fesetenv/feholdexcept/
 * feupdateenv to fnstenv/fldenv; ARET routes all four to its runtime x87 model
 * so the rounding mode (__x87rt_rc) survives a save/restore across the call
 * boundary. Observed via fegetround (a fnstcw read of the modelled control word,
 * not host libm), so this is a clean differential of the fenv round-trip, not of
 * a rounding libm function. Deterministic; differential against the Wine/Windows
 * oracle. */
#include <stdio.h>
#include <fenv.h>
static const char* rname(int r){
    switch(r){case FE_TONEAREST:return "nearest";case FE_UPWARD:return "up";
    case FE_DOWNWARD:return "down";case FE_TOWARDZERO:return "zero";}return "?";
}
int main(void){
    fesetround(FE_UPWARD);
    fenv_t saved; fegetenv(&saved);      /* fnstenv: capture RC=up */
    fesetround(FE_TONEAREST);            /* perturb */
    fesetenv(&saved);                    /* fldenv: restore RC=up */
    printf("after restore: %s\n", rname(fegetround()));

    fesetround(FE_DOWNWARD);
    fenv_t s2; fegetenv(&s2);
    fesetround(FE_TOWARDZERO);
    printf("perturbed: %s\n", rname(fegetround()));
    fesetenv(&s2);
    printf("restored: %s\n", rname(fegetround()));

    fesetround(FE_TONEAREST);
    fenv_t hold; feholdexcept(&hold);    /* fnstenv + fnclex */
    volatile double x = 1.0; double y = (x/3.0)*3.0;
    feupdateenv(&hold);                  /* fldenv + re-raise */
    printf("bracket: %s y=%.5f\n", rname(fegetround()), y);
    printf("done\n");
    return 0;
}
