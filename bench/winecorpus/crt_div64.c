/* Proves the libgcc 64-bit soft-arith shims (aret_divdi3 & co): built with
 * -shared-libgcc so the helpers are DLL imports (from libgcc_s_dw2-1.dll) that ARET
 * routes to its shims, while the Wine oracle loads the genuine libgcc_s. The div/mod
 * of a/b emit __divdi3/__moddi3/__udivdi3/__umoddi3; __divmoddi4/__udivmoddi4 (which
 * gcc rarely emits on its own) are called directly to cover the remainder-pointer ABI. */
#include <stdio.h>
extern long long __divmoddi4(long long, long long, long long *);
extern unsigned long long __udivmoddi4(unsigned long long, unsigned long long, unsigned long long *);
static volatile long long S[][2] = {
  { 1000000000007LL, 123457LL }, { -1000000000007LL, 123457LL },
  { 1000000000007LL, -123457LL }, { -1000000000007LL, -123457LL },
  { 9223372036854775807LL, 3LL }, { -42LL, 5LL },
};
static volatile unsigned long long U[][2] = {
  { 18446744073709551615ULL, 1000000007ULL }, { 0xFFFFFFFFFFFFULL, 7ULL },
};
int main(void){
  for (int i=0;i<6;i++){ long long a=S[i][0], b=S[i][1];
    printf("s %lld / %lld = %lld  %% = %lld\n", a, b, a/b, a%b); }
  for (int i=0;i<2;i++){ unsigned long long a=U[i][0], b=U[i][1];
    printf("u %llu / %llu = %llu  %% = %llu\n", a, b, a/b, a%b); }
  long long r; long long q = __divmoddi4(-1000000000007LL, 123457LL, &r);
  printf("dm4 q=%lld r=%lld\n", q, r);
  unsigned long long ur; unsigned long long uq = __udivmoddi4(18446744073709551615ULL, 1000000007ULL, &ur);
  printf("udm4 q=%llu r=%llu\n", uq, ur);
  printf("done\n");
  return 0;
}
