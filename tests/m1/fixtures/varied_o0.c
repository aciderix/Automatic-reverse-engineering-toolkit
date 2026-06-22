#include <stdio.h>
#include <string.h>
#include <stdint.h>
volatile int z = 0;
/* recursion */
static int fib(int n){ return n<2?n:fib(n-1)+fib(n-2); }
/* bit tricks */
static unsigned popcnt(uint32_t x){ unsigned c=0; while(x){c+=x&1;x>>=1;} return c; }
/* 64-bit */
static uint64_t mix64(uint64_t a, uint64_t b){ uint64_t h=a*0x9E3779B97F4A7C15ULL; h^=b>>29; h*=0xBF58476D1CE4E5B9ULL; return h>>31; }
/* string */
static int vowels(const char*s){ int n=0; for(;*s;s++) if(strchr("aeiou",*s)) n++; return n; }
/* switch dispatch */
static long op(int k, long a, long b){
  switch(k){case 0:return a+b;case 1:return a-b;case 2:return a*b;case 3:return b?a/b:0;
            case 4:return a%(b?b:1);case 5:return a<<(b&31);case 6:return a>>(b&31);
            case 7:return a&b;case 8:return a|b;case 9:return a^b;default:return -1;}
}
/* float */
static double poly(double x){ return ((3.0*x-2.0)*x+1.5)*x-0.25; }
int main(void){
  printf("fib=%d pop=%u\n", fib(20)+z, popcnt(0xDEADBEEF));
  printf("mix=%llu vowels=%d\n", (unsigned long long)mix64(123,456), vowels("hello beautiful world"));
  long t=0; for(int k=0;k<11;k++) t+=op(k, 100, 7);
  printf("ops=%ld\n", t);
  printf("poly=%.4f\n", poly(2.5));
  return 0;
}
