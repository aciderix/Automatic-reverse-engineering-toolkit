#include <windows.h>
#include <stdio.h>
int main(void){
  LONG v=10;
  LONG a=InterlockedIncrement(&v);
  LONG b=InterlockedDecrement(&v);
  LONG c=InterlockedExchangeAdd(&v,5);
  LONG d=InterlockedExchange(&v,99);
  LONG e=InterlockedCompareExchange(&v,42,99);
  printf("inc=%ld dec=%ld xadd=%ld xchg=%ld cmpxchg=%ld final=%ld\n",
         (long)a,(long)b,(long)c,(long)d,(long)e,(long)v);
  return 0;
}
