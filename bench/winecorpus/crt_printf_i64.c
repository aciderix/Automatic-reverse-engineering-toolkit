/* MSVC printf 64-bit format guard. msvcrt uses the `I64` size prefix (`%I64d`,
 * `%I64u`, `%I64x`) for 64-bit integers, which glibc does not understand — the
 * shared reformatter must translate it, else `%I64d` prints literally (first seen
 * in busybox `expr`, which formats its result with `%I64d`). Also checks bare `%I`
 * (pointer-width, 32-bit on Win32) and a mix with ordinary specifiers. Must match
 * Wine bit-for-bit. */
#include <stdio.h>
int main(void) {
    long long v = 42;
    unsigned long long u = 0x1122334455667788ULL;
    printf("d=%I64d u=%I64u x=%I64x X=%I64X\n", v, u, u, u);
    printf("neg=%I64d big=%I64d\n", (long long)-123456789012LL, (long long)1000000000000LL);
    printf("mix %d %I64d %s %c\n", 7, (long long)9, "end", '!');
    printf("width=%08I64x\n", (unsigned long long)0xABCDull);
    return 0;
}
