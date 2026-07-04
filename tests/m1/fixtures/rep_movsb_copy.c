/* Forces `rep movsb` — the memcpy idiom the lifter turns into a synthetic
   `memcpy(edi, esi, ecx)` call. Copies a fixed 16 bytes between two stack
   buffers so the funcdiff differential can *score* it (in-region pointers,
   constant length) and validate the interpreter's memory-call model against
   Unicorn, which executes the real `rep movsb`. `noinline` keeps it a separate
   recovered function (funcdiff diffs it on its own, from random state). */
#include <stdio.h>

static int __attribute__((noinline)) copy_and_sum(unsigned seed) {
    unsigned char src[16], dst[16];
    for (int i = 0; i < 16; i++) src[i] = (unsigned char)(seed + i);
    __asm__ volatile("cld\n\t rep movsb"
                     : : "S"(src), "D"(dst), "c"(16) : "memory", "cc");
    return dst[0] + dst[7] + dst[15];
}

int main(void) {
    printf("copy=%d\n", copy_and_sum(10));
    return 0;
}
