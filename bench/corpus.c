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

/* Pointer / array / loop functions — exercise memory and bounded loops. */
int arraysum(const int* a, int n)        { int s = 0; for (int i = 0; i < n; i++) s += a[i]; return s; }
int arraymax(const int* a, int n)        { int m = n > 0 ? a[0] : 0; for (int i = 1; i < n; i++) if (a[i] > m) m = a[i]; return m; }
int third(const int* a)                  { return a[2]; }
int counteq(const int* a, int n, int x)  { int c = 0; for (int i = 0; i < n; i++) if (a[i] == x) c++; return c; }
int strlen_c(const char* s)              { int n = 0; while (s[n]) n++; return n; }

/* 1-operand mul/div/idiv (variable divisor -> real div/idiv instruction). */
int udiv(unsigned a, unsigned b) { return b ? (int)(a / b) : 0; }
int umod(unsigned a, unsigned b) { return b ? (int)(a % b) : 0; }
int sdiv(int a, int b)           { return b ? a / b : 0; }
int smod(int a, int b)           { return b ? a % b : 0; }
int widemul(unsigned a, unsigned b) { return (int)((unsigned long long)a * b >> 4); }

/* High-byte registers (ah/dh), sign-extension (cdqe/movsx). */
int hibyte(unsigned x)           { return (int)((x >> 8) & 0xff); }       /* %ah read */
int hibyte3(unsigned x)          { unsigned char h = (x >> 8); return h * 3; }
int sxbyte(int x)                { return (int)(signed char)x; }          /* movsx */
int sxword(int x)                { return (int)(short)x; }                /* movsx */
int idxw(const int* a, int i)    { return a[i] + 1; }                     /* cdqe index */


/* Stack-spill functions: `volatile` locals force constant-offset stack slots
   with no address taken, exercising rsp/rbp stack-slot recovery + promotion.
   volatile changes memory traffic, not the computed value, so the result stays
   deterministic for differential testing. */
int spill2(int a, int b)        { volatile int x = a + b; volatile int y = a - b; return x * y + x - y; }
int spill3(int a, int b, int c) { volatile int p = a * b; volatile int q = b * c; volatile int r = a * c; return p + q - r; }

/* Broader differential coverage: stack arrays, multi-local spills, nested
   control flow, struct-like pointer access — patterns the IR pipeline now
   recovers. All pure and small-input-safe. */
int sortpair(int a, int b)       { int t; if (a > b) { t = a; a = b; b = t; } return a * 10 + b; }
int poly(int x)                  { return ((x*x) + 3*x + 7) & 0xffff; }
int stackarr(int a, int b, int c){ int v[3]; v[0]=a; v[1]=b; v[2]=c; return v[0]*v[1] - v[2]; }
int fieldsum(const int* p)       { return p[0] + p[1]*2 + p[2]*3; }   /* struct-like */
int nestcond(int a, int b)       { if (a > 0) { if (b > 0) return a+b; else return a-b; } return -a; }

/* Non-tail recursion: a self-call to a *defined* symbol, exercising static
   relocation resolution in object files (the call target is a placeholder until
   the .rela.text reloc is applied). */
int sumrec(int n) { return n <= 0 ? 0 : n + sumrec(n - 1); }

/* Subtract-with-borrow idioms: gcc emits `sbb` for these at -O1/-O2. */
int borrow(unsigned a, unsigned b) { return (a < b) ? -1 : 0; }
int cmp3(unsigned a, unsigned b)   { return (a > b) - (a < b); }

/* Scalar floating-point: exercises cvtsi2ss/sd, addss/mulss/.../divsd,
   cvttss2si/cvttsd2si, comiss/comisd, movss/movsd. Returns int so the
   differential harness can compare results (float ops are bit-exact: both the
   original and ARET use native IEEE-754). */
int favg(int a, int b)      { float x = a, y = b; return (int)((x + y) / 2.0f); }
int fpoly(int x)            { double t = x; return (int)(t*t*0.5 + t*3.0 - 1.0); }
int fcmp(int a, int b)      { float x = a * 1.5f, y = b * 1.5f; return x > y ? a : b; }
int fmix(int a, int b, int c) { double r = (double)a / (b ? b : 1) + (double)c * 0.25; return (int)r; }

/* 64-bit division/mul and bit ops: exercise __ix_* helpers (div/mul/idiv 1-op
   64-bit), bswap, and the bit-scan/bit-test paths. */
unsigned long long div64(unsigned long long a, unsigned long long b){ return b ? a / b + a % b : 0; }
long long idiv64(long long a, long long b){ return b ? a / b - a % b : 0; }
int bswapi(unsigned x){ return (int)__builtin_bswap32(x); }
int clz(unsigned x){ return x ? __builtin_clz(x) : 32; }
int ctz(unsigned x){ return x ? __builtin_ctz(x) : 32; }

/* Vectorisable widening/interleaving: exercise punpck* and 16-bit lane ops. */
int widen(const short* a){ int s=0; for(int i=0;i<8;i++) s+=a[i]; return s; }
int interleave(const int* a, const int* b){ int s=0; for(int i=0;i<4;i++) s+=a[i]*b[i]; return s; }

/* Tail calls: gcc emits `jmp func` at -O2, lifted as `return func(args)`. */
int tlen(const char* s){ return (int)strlen(s); }   /* tail call -> jmp strlen */

/* Switch with enough cases that gcc emits a jump table at -O2. */

/* Struct copy by value: gcc may emit `rep movsq`. Returns an element so the
   differential can compare. */

/* Stack array via variable index — previously crashed standalone (deref of
   uninitialised frame register); should now work via the __frame array. */
int stkarr(int a, int b, int c){ int v[6]; for(int i=0;i<6;i++) v[i]=a*i+b; return v[c&5]-c; }

/* Global/table access — only testable with the in-place harness (the decompiled
   code reads these by absolute address). */
static const int LUT[8] = {2,3,5,7,11,13,17,19};
int lut(int i){ return LUT[i & 7]; }
int swv(int x){ switch(x){case 0:return 7;case 1:return 11;case 2:return 13;case 3:return 17;case 4:return 19;case 5:return 23;default:return -1;} }
int swc(int x, int a){switch(x){case 0:return a+7;case 1:return a*11;case 2:return a-13;case 3:return a^17;case 4:return a<<2;case 5:return a*a;case 6:return a-1;case 7:return a+9;default:return -1;}}
