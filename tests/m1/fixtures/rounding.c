#include <stdio.h>
#include <math.h>
volatile double a = 2.75, b = -2.5, c = 5.5;
int main(void){
  printf("floor=%.1f ceil=%.1f fdiv=%.1f nfloor=%.1f\n",
         floor(a), ceil(a), floor(c/2.0), floor(b));
  return 0;  /* expect floor=2.0 ceil=3.0 fdiv=2.0 nfloor=-3.0 */
}
