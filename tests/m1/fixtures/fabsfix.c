#include <stdio.h>
#include <math.h>
volatile double gv = -3.5;
int main(void){
  double x = gv;
  printf("ABS=%.2f\n", fabs(x));   /* expect 3.50 */
  return 0;
}
