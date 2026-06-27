/* 64-bit add/adc carry on a 32-bit target. The array-bound idiom from Lua's
 * luaH_getint: is (key-1) < alimit, computed as a 64-bit value via
 * `add $-1; adc $-1; cmp; sbb`. The 32-bit add's carry was lifted from a
 * sign-extended immediate (0xffffffff -> 0xffff_ffff_ffff_ffff), so the
 * carry-out came out 0 instead of 1 and (2-1) became 0xffffffff00000001.
 * That made registry[2] (the globals table) miss the array part -> _G nil. */
#include <stdint.h>
#include <stdio.h>
__attribute__((noinline)) int in_array(int64_t key, uint32_t alimit) {
    return (uint64_t)(key - 1) < (uint64_t)alimit;
}
int main(void) {
    printf("CARRY: %d %d %d %d\n",
           in_array(2, 2),            /* (2-1)=1 < 2 -> 1  (the _G case) */
           in_array(1, 2),            /* (1-1)=0 < 2 -> 1               */
           in_array(3, 2),            /* (3-1)=2 < 2 -> 0               */
           in_array(0x100000002LL, 2) /* high word set     -> 0         */);
    return 0;
}
