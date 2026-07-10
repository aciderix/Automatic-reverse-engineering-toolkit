/* Exercises the *raw* x87 transcendental instructions (fsin/fcos/fptan/fpatan/
 * fyl2x/f2xm1/fscale/fsincos) via inline asm — NOT the libm sin/cos/exp/log calls
 * (those are host-backed by name and bypass the lifter entirely). ARET's static
 * x87 depth-pass does not model these ops ("unmodelled x87 op" bail), so they are
 * lowered to the runtime FPU-stack net (`__x87rt_*`), which must reproduce the
 * hardware bit-for-bit. Running the PE under Wine executes the real i386 x87
 * transcendentals on the CPU, so this is a direct ground-truth check of the net.
 * Fixed inputs + %.6f keep the output deterministic across both engines. */
#include <stdio.h>

static double do_fsin(double x){ double r;
  __asm__ __volatile__("fsin" : "=t"(r) : "0"(x)); return r; }
static double do_fcos(double x){ double r;
  __asm__ __volatile__("fcos" : "=t"(r) : "0"(x)); return r; }
static double do_f2xm1(double x){ double r; /* 2^x - 1, |x|<=1 */
  __asm__ __volatile__("f2xm1" : "=t"(r) : "0"(x)); return r; }
static double do_fptan(double x){ double r; /* tan; pushes 1.0 we must pop */
  __asm__ __volatile__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
static double do_fpatan(double y, double x){ double r; /* atan2(y,x) */
  __asm__ __volatile__("fpatan" : "=t"(r) : "0"(y), "u"(x) : "st(1)"); return r; }
static double do_fyl2x(double x, double y){ double r; /* y*log2(x) */
  __asm__ __volatile__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static double do_fscale(double x, double n){ double r; /* x * 2^floor(n) */
  __asm__ __volatile__("fscale" : "=t"(r) : "0"(x), "u"(n)); return r; }
static double do_fsincos_sin(double x){ double s;
  __asm__ __volatile__("fsincos\n\tfstp %%st(0)" : "=t"(s) : "0"(x)); return s; }

int main(void){
  printf("fsin=%.6f fcos=%.6f\n", do_fsin(1.0), do_fcos(1.0));
  printf("fptan=%.6f fpatan=%.6f\n", do_fptan(0.5), do_fpatan(1.0, 1.0));
  printf("fyl2x=%.6f f2xm1=%.6f\n", do_fyl2x(8.0, 1.0), do_f2xm1(0.5));
  printf("fscale=%.6f fsincos=%.6f\n", do_fscale(3.0, 2.0), do_fsincos_sin(0.5));
  return 0;
}
