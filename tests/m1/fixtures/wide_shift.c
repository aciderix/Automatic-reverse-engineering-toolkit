#include <stdio.h>
volatile int sh = 32;
__attribute__((noinline)) long long shift(long long a, int n){ return a << n; }
__attribute__((noinline)) long long bigmul(long long a, long long b){ return a*b; }
int main(void){
  long long r = shift(1, sh);
  long long m = bigmul(0x100000000LL, 3);
  printf("r=%lld m=%lld\n", r, m);  /* expect r=4294967296 m=12884901888 */
  return 0;
}
