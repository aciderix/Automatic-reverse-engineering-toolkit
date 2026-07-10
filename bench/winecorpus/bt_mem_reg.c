/* Exercises `bt/bts/btr/btc [mem], reg` — a memory bit-base with a *register* bit
 * offset (the bit-array idiom `bt [arr], eax`). Unlike the imm8 / register-base
 * forms, the register offset is NOT masked to the operand width: it shifts the
 * effective address (element = base + (idx>>5)*4 for dword, bit = idx&31), so a
 * large index reaches far into the array. Raw inline asm forces the exact forms.
 * Wine runs the real instruction on the CPU; ARET runs the lifted address-adjust
 * + load/modify/store. Covers indices within, at, and well past the first dword,
 * the read-only BT (CF only) and the three read-modify-write variants; the array
 * contents + CF are printed for a bit-for-bit check vs Wine. */
#include <stdio.h>

static int bt_mem(const unsigned *arr, int idx) {
    unsigned char cf;
    __asm__ __volatile__("btl %2, %1\n\tsetc %0"
        : "=q"(cf) : "m"(*arr), "r"(idx) : "cc");
    return cf;
}
static int bts_mem(unsigned *arr, int idx) {
    unsigned char cf;
    __asm__ __volatile__("btsl %2, %1\n\tsetc %0"
        : "=q"(cf), "+m"(*arr) : "r"(idx) : "cc");
    return cf;
}
static int btr_mem(unsigned *arr, int idx) {
    unsigned char cf;
    __asm__ __volatile__("btrl %2, %1\n\tsetc %0"
        : "=q"(cf), "+m"(*arr) : "r"(idx) : "cc");
    return cf;
}
static int btc_mem(unsigned *arr, int idx) {
    unsigned char cf;
    __asm__ __volatile__("btcl %2, %1\n\tsetc %0"
        : "=q"(cf), "+m"(*arr) : "r"(idx) : "cc");
    return cf;
}

int main(void) {
    /* bit array: 4 dwords = 128 bits. Set a known pattern. */
    unsigned a[4] = {0x00000001u, 0x80000000u, 0x0000ffffu, 0xa5a5a5a5u};

    /* BT (read only) at several offsets that cross dword boundaries. */
    int off[] = {0, 31, 32, 63, 64, 79, 96, 127};
    for (int i = 0; i < 8; i++)
        printf("bt[%d]=%d ", off[i], bt_mem(a, off[i]));
    printf("\n");

    /* BTS/BTR/BTC read-modify-write at cross-boundary indices, then dump array. */
    printf("bts96 cf=%d ", bts_mem(a, 96));     /* sets bit 0 of a[3] */
    printf("btr33 cf=%d ", btr_mem(a, 33));     /* clears bit 1 of a[1] */
    printf("btc127 cf=%d\n", btc_mem(a, 127));  /* toggles top bit of a[3] */
    printf("array=%08x %08x %08x %08x\n", a[0], a[1], a[2], a[3]);
    return 0;
}
