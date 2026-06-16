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

/* x87 80-bit extended precision (`long double` on x86-64 always lowers to the
   x87 FPU at every -O level). Exercises fild (int load), faddp/fsubp/fmulp/fdivp
   arithmetic, fcomi compares + branches, and the float->int truncation idiom
   (fnstcw/or/fldcw/fistp). Int-compatible signatures so the differential harness
   can compare results (x87 long double is bit-exact: both original and ARET use
   native `long double`). */
unsigned x87arith(int a, int b, int c){
    long double x = (long double)a, y = (long double)b, z = (long double)c;
    long double r = (x*y - z) * (x + y);
    return (unsigned)r;
}
int x87cmp(int a, int b){
    long double x = (long double)a, y = (long double)b;
    long double p = x*x - y, q = y*y - x;
    return p < q ? 1 : (p == q ? 2 : 3);
}
long long x87div(int a, int b){
    long double x = (long double)a, y = (long double)b;
    long double d = x*x + y*y;
    if (d == 0) return 0;
    long double r = (x*x*x - y*y*y) / d;
    return (long long)r;
}

/* Rotates, 1-operand signed multiply (imul r/m -> rdx:rax), and register xchg.
   Int-compatible signatures for the differential harness. */
unsigned rotl(unsigned x, int n){ n &= 31; return (x << n) | (x >> ((32 - n) & 31)); }
unsigned rotr(unsigned x, int n){ n &= 31; return (x >> n) | (x << ((32 - n) & 31)); }
long long imulh(long a, long b){ return (long long)(((__int128)a * b) >> 64); }
int xchg3(int a, int b){ int t = a; a = b; b = t; return a*7 - b*3; }

/* Packed double (128-bit) lane moves: exercises unpcklpd/unpckhpd (and the
   movhps/movhlps/shufpd half-move machinery). Built from int args (finite,
   deterministic) so the differential harness can compare. */
typedef double __v2df __attribute__((vector_size(16)));
int vshuf(int x, int y){
    __v2df a = {(double)x, (double)y};
    __v2df b = {(double)y, (double)x};
    __v2df c = a*b + a;
    return (int)(c[0] - c[1]*2);
}

/* rep stos (gcc emits `rep stosq` to zero a struct/array). */
int repset(int a, int b){ long buf[16]; for(int i=0;i<16;i++) buf[i]=0; buf[a&15]=b; return (int)buf[b&15]; }

/* Packed single-precision SSE (4 floats / 128-bit): addps/subps/mulps/divps,
   minps/maxps/sqrtps, cmpps+movmskps, shufps, unpcklps/unpckhps, cvtdq2ps.
   Built from int args (finite, deterministic) so the harness can compare. */
typedef float __v4sf __attribute__((vector_size(16)));
typedef int   __v4si __attribute__((vector_size(16)));
int psA(int x, int y){
    __v4sf a={(float)x,(float)y,(float)(x+2),(float)(y+3)};
    __v4sf b={(float)(y+1),(float)(x-1),(float)x,(float)y};
    __v4sf c=a*b + a - b;
    __v4sf d=b/(a+ (__v4sf){1,1,1,1});
    return (int)c[0]+(int)c[1]*3-(int)c[2]+(int)d[3];
}
int psB(int x, int y){
    __v4sf a={(float)x,(float)y,(float)(x*2),(float)(y*2)};
    __v4sf b={(float)y,(float)x,(float)(y*3),(float)(x*3)};
    __v4sf lo=__builtin_ia32_minps(a,b), hi=__builtin_ia32_maxps(a,b);
    __v4sf s=__builtin_ia32_sqrtps(hi*hi);
    return (int)lo[0]-(int)hi[1]+(int)s[2]+(int)s[3];
}
int psC(int x, int y){
    __v4sf a={(float)x,(float)y,(float)(x+1),(float)(y+1)};
    __v4sf b={(float)y,(float)x,(float)(x-1),(float)(y-1)};
    __v4sf m=(__v4sf)__builtin_ia32_cmpltps(a,b);
    return __builtin_ia32_movmskps(m) + (int)((__v4sf)__builtin_ia32_cmpleps(a,b))[0];
}
int psD(int x, int y){
    __v4si a={x,y,x+y,x-y};
    __v4sf b=__builtin_ia32_cvtdq2ps(a);
    __v4sf c=b*b - b;
    return (int)c[0]+(int)c[1]-(int)c[2]+(int)c[3];
}
