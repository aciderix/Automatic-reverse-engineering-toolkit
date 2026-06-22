#include <stdio.h>
volatile double seed = 2.9;
int main(void){
  double x = seed;
  long sum = 0;
  for (int i = 0; i < 5; i++) {
    sum += (long)(x * (i + 1));   /* (long) cast = truncate fist, CW often hoisted */
  }
  printf("sum=%ld\n", sum);       /* 2+5+8+11+14 = 40 */
  return 0;
}
