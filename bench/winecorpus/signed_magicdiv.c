/* Guard: signed `/` and `%` by a *literal* constant. gcc/mingw -O2 compiles
 * these to a magic-multiply `mov reg,MAGIC; imul r/m` (one-operand signed imul)
 * where MAGIC often has bit 31 set (23 -> 0xb21642c9, 7 -> 0x92492493,
 * 3 -> 0xaaaaaaab). The one-operand imul is lifted as sext(eax)*sext(r/m); after
 * the optimizer copy-propagates the `mov reg,MAGIC` and folds `MAGIC & 0xffffffff`
 * -> `MAGIC`, the constant operand reaches SignExtend as a *bare* const. A backend
 * that then zero-extends it reads a large positive i64, flipping the product's
 * high word -> wrong quotient/remainder. Found via sqlite mingw (`h % 23` gave a
 * negative hash bucket index -> out-of-bounds store -> crash). Bit-for-bit vs Wine.
 */
#include <stdio.h>
int main(void) {
    long acc = 0;
    /* runtime dividend, literal divisors whose magic has bit 31 set */
    for (int n = -100000; n <= 100000; n += 137) {
        acc += (long)(n % 23) + (long)(n / 23) * 7;
        acc += (long)(n % 7)  * 3 + (long)(n / 7);
        acc += (long)(n % 3)  - (long)(n / 3) * 5;
        acc += (long)(n % 365);
    }
    printf("acc=%ld\n", acc);
    return 0;
}
