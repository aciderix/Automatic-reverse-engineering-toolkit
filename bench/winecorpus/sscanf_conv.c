#include <stdio.h>
#include <stdlib.h>
int main(void){
  int a,b; double d; char s[32];
  sscanf("42 -7 3.5 word", "%d %d %lf %31s", &a,&b,&d,s);
  printf("a=%d b=%d d=%.2f s=%s\n", a,b,d,s);
  printf("atoi=%d atof=%.2f atol=%ld\n", atoi("123x"), atof("4.5e1"), atol("-99"));
  printf("strtol=%ld strtoul=%lu\n", strtol("0x1F",0,16), strtoul("777",0,8));
  printf("strtod=%.3f\n", strtod("2.718",0));
  return 0;
}
