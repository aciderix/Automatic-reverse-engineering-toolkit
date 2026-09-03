#include <stdio.h>
volatile int sel = 0;
/* Like two_switch.c, but the switch selector lives in MEMORY (a struct field),
   so the compiler bounds it with `cmp [reg+off], N` and only *reloads* it into a
   register for the `jmp [reg*4+table]`. The register never carries the `cmp`, so a
   bound-finder that only matches `cmp <register>, N` misses it — and the first
   table over-reads into the second (both adjacent in .rdata, all targets
   executable), merging opA and opB into one giant CFG. This is exactly what made
   libLerc's sub_456340 absorb ~400 spurious targets (878k phi nodes, 283 MB C).
   The bound must be recovered from the memory-operand compare. */
struct Ctx { int pad[16]; int op; };

__attribute__((noinline)) int opA(struct Ctx *c, int x, int y){
  switch(c->op){
    case 0: return x+y;  case 1: return x-y;  case 2: return x*y;
    case 3: return x|y;  case 4: return x&y;  case 5: return x^y;
    case 6: return x<<1; case 7: return y<<1;
    default: return -1;
  }
}
__attribute__((noinline)) int opB(struct Ctx *c, int x, int y){
  switch(c->op){
    case 0: return x*100+y; case 1: return x*100-y; case 2: return x+100*y;
    case 3: return x-100*y; case 4: return 100; case 5: return 200;
    case 6: return 300;     case 7: return 400;    case 8: return 500;
    default: return -2;
  }
}
int main(void){
  int s = sel; int t = 0;
  struct Ctx c;
  for(int i=0;i<8;i++){ c.op = i; t += opA(&c, 7+s, 3+s); }
  for(int i=0;i<9;i++){ c.op = i; t += opB(&c, 2+s, 5+s); }
  printf("t=%d\n", t);
  return 0;
}
