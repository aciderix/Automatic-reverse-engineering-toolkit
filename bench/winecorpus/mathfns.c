#include <stdio.h>
#include <math.h>
int main(void){
  printf("sqrt=%.4f pow=%.4f\n", sqrt(2.0), pow(2.0,10.0));
  printf("floor=%.1f ceil=%.1f fabs=%.1f\n", floor(2.7), ceil(2.1), fabs(-3.5));
  printf("sin=%.4f cos=%.4f\n", sin(1.0), cos(1.0));
  printf("exp=%.4f log=%.4f\n", exp(1.0), log(10.0));
  printf("fmod=%.2f hypot=%.4f\n", fmod(10.0,3.0), hypot(3.0,4.0));
  double ip; double fp=modf(3.75,&ip);
  printf("modf ip=%.1f fp=%.2f ldexp=%.1f\n", ip, fp, ldexp(1.5,3));
  return 0;
}
