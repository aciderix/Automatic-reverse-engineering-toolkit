#include <stdio.h>
#include <math.h>
int main(void){
  printf("atan2=%.4f asin=%.4f acos=%.4f tan=%.4f\n", atan2(1,1),asin(0.5),acos(0.5),tan(0.5));
  printf("log10=%.4f sinh=%.4f cosh=%.4f tanh=%.4f\n", log10(1000.0),sinh(1.0),cosh(1.0),tanh(1.0));
  printf("round=%.1f trunc=%.1f cbrt=%.4f copysign=%.1f\n", round(2.5),trunc(2.9),cbrt(27.0),copysign(3.0,-1.0));
  int e; double m=frexp(12.0,&e);
  printf("frexp m=%.3f e=%d fmin=%.1f fmax=%.1f\n", m,e,fmin(2.0,5.0),fmax(2.0,5.0));
  return 0;
}
