int _pad(int x) { return x ^ 0x5a5a; }  /* keeps real functions off offset 0 */
/* Differential-test corpus: pure, leaf, integer functions with small-input-safe
   behaviour. Compiled at -O1; ARET decompiles each, and difftest.sh checks the
   recompiled version against the original on random inputs. */
int add1(int n)            { return n + 1; }
int addsub(int a, int b)   { return a - b + 7; }
int mulshift(int a)        { return (a << 3) + a; }      /* a*9 */
int maxi(int a, int b)     { return a > b ? a : b; }
int mini(int a, int b)     { return a < b ? a : b; }
int absdiff(int a, int b)  { return a > b ? a - b : b - a; }
int sign(int x)            { if (x < 0) return -1; if (x > 0) return 1; return 0; }
int clampu(unsigned x)     { return x > 100u ? 100 : (int)x; }
int mix(unsigned a, unsigned b) { return (a ^ b) + (a & b) * 2; }
int sumto(int n)           { int s = 0; for (int i = 0; i <= n; i++) s += i; return s; }
int countbits(unsigned x)  { int c = 0; while (x) { c += x & 1; x >>= 1; } return c; }
