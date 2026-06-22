#include <stdio.h>
volatile int z=0;
static int fib(int n){ return n<2?n:fib(n-1)+fib(n-2); }
static int fact(int n){ return n<=1?1:n*fact(n-1); }
int main(void){
  printf("fib5=%d fib10=%d fact5=%d\n", fib(5)+z, fib(10)+z, fact(5)+z);
  return 0; /* expect fib5=5 fib10=55 fact5=120 */
}
