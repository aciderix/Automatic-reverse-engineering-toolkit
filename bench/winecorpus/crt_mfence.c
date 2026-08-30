/* Memory fences (MFENCE/LFENCE/SFENCE): no architectural effect on a single stream of
 * execution, so ARET drops them (a no-op, like PAUSE) — this asserts a program that
 * issues them still computes the same result as the real CPU under Wine. Inline asm so
 * the exact instructions are emitted without an intrinsics header. */
#include <stdio.h>
int main(void){
    volatile int a = 3, b = 4, c = 0;
    __asm__ __volatile__("mfence" ::: "memory");
    c = a * b;
    __asm__ __volatile__("lfence" ::: "memory");
    c += a;
    __asm__ __volatile__("sfence" ::: "memory");
    printf("c=%d\n", c);
    for (int i = 0; i < 3; i++) {
        __asm__ __volatile__("mfence\n\tlfence\n\tsfence" ::: "memory");
        c += i;
    }
    printf("after fences c=%d\n", c);
    printf("done\n");
    return 0;
}
