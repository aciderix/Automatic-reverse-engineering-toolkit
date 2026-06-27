/* Reproduces a 64-bit signed-compare flag bug via Lua's exact `>>` lowering.
   Lua 5.4 evaluates `x >> n` as luaV_shiftl(x, -n), which first tests
   `n <= -64`. The compiler lowers that 64-bit signed compare to
   `cmp lo, -63 ; sbb hi, -1 ; jl`. The `cmp r/m32, imm8` sign-extends the
   immediate to the operand width (0xffffffc1), but ARET masks values into a
   64-bit C int, so CF was computed as `(u64)lo < (u64)0xffff...ffc1` instead of
   the 32-bit `(u32)lo < (u32)0xffffffc1` — wrong CF, then the high-word sbb
   mis-set SF/OF, so `256 >> 2` returned 0. sub_flags now masks the operands to
   the op width before the CF/ZF compare. Expected: r1=64 r2=1024 r3=0 r4=128 */
#include <stdio.h>
typedef long long i64;
typedef unsigned long long u64;
volatile i64 X = 256;

__attribute__((noinline)) i64 shiftl(i64 x, i64 n) { /* == Lua's luaV_shiftl */
    if (n < 0) {
        if (n <= -64) return 0;
        else return (i64)((u64)x >> (u64)(-n));
    } else {
        if (n >= 64) return 0;
        else return (i64)((u64)x << (u64)n);
    }
}

int main(void) {
    i64 x = X;
    printf("r1=%lld r2=%lld r3=%lld r4=%lld\n",
           shiftl(x, -2), shiftl(x, 2), shiftl(x, -70), shiftl(x, -1));
    return 0;
}
