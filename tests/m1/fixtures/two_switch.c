#include <stdio.h>
volatile int sel = 0;
/* Two dense switches → two jump tables gcc lays out adjacently in .rdata.
   Without a size bound, the first table over-reads into the second, merging
   the functions. Each must stay independent and exact. */
__attribute__((noinline)) int opA(int op, int x, int y){
  switch(op){
    case 0: return x+y;  case 1: return x-y;  case 2: return x*y;
    case 3: return x|y;  case 4: return x&y;  case 5: return x^y;
    case 6: return x<<1; case 7: return y<<1; case 8: return x+1;
    case 9: return y+1;  case 10: return x-1; case 11: return y-1;
    default: return -1;
  }
}
__attribute__((noinline)) int opB(int op, int x, int y){
  switch(op){
    case 0: return x*100+y; case 1: return x*100-y; case 2: return x+100*y;
    case 3: return x-100*y; case 4: return 100; case 5: return 200;
    case 6: return 300; case 7: return 400; case 8: return 500;
    case 9: return 600; case 10: return 700;
    default: return -2;
  }
}
int main(void){
  int s=sel; int t=0;
  for(int i=0;i<12;i++) t += opA(i, 7+s, 3+s);
  for(int i=0;i<11;i++) t += opB(i, 2+s, 5+s);
  printf("t=%d\n", t);
  return 0;
}
