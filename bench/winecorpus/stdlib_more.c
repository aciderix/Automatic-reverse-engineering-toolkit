#include <stdio.h>
#include <stdlib.h>
static void bye(void){ printf("atexit_ran\n"); }
int main(void){
  atexit(bye);
  printf("abs=%d labs=%ld llabs=%lld\n", abs(-5), labs(-100000L), llabs(-9999999999LL));
  div_t d=div(17,5); ldiv_t l=ldiv(100,7);
  printf("div q=%d r=%d ldiv q=%ld r=%ld\n", d.quot,d.rem,l.quot,l.rem);
  printf("strtoll=%lld strtoull=%llu\n", strtoll("-123456789",0,10), strtoull("ffff",0,16));
  putenv("WD_VAR=hello42"); printf("getenv=[%s]\n", getenv("WD_VAR"));
  return 0;
}
