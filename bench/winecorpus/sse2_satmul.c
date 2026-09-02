/* SSE2 packed unsigned-saturating add, high-unsigned multiply and word multiply-add
 * (paddusb/paddusw/pmulhuw/pmaddwd) — the ops pixman's default (SSE2) blitters use.
 * Written in inline asm so the winediff harness builds it with no -msse2, and to pin
 * the exact instructions. Runtime inputs prevent constant folding. Guards the lift
 * arms + __pi_addus8/addus16/mulhuw/maddwd helpers: a scalar-lowered lift diverges
 * from Wine here (saturation clamps, high-word product, word-pair madd). */
#include <stdio.h>
#include <stdint.h>
static volatile uint16_t W1[8] = {100, 40000, 65000, 1, 32768, 500, 12345, 9},
                         W2[8] = {50, 30000, 2000, 65535, 32768, 400, 11111, 7};
static volatile uint8_t  B1[16] = {200,10,255,1,128,64,32,16,250,5,100,150,7,8,9,10},
                         B2[16] = {100,20,5,1,128,200,32,240,10,250,100,150,1,2,3,4};
static void op(const char *n, const void *a, const void *b, const char *mn) {
    uint16_t o[8];
    /* movdqu a->xmm0, b->xmm1, <mn> xmm0,xmm1, store xmm0. mn chosen by caller. */
    if (mn[3]=='w' && mn[0]=='a') /* addusw */
        __asm__ volatile("movdqu (%1),%%xmm0\n\tmovdqu (%2),%%xmm1\n\tpaddusw %%xmm1,%%xmm0\n\tmovdqu %%xmm0,(%0)"::"r"(o),"r"(a),"r"(b):"memory");
    else if (mn[0]=='a') /* addusb */
        __asm__ volatile("movdqu (%1),%%xmm0\n\tmovdqu (%2),%%xmm1\n\tpaddusb %%xmm1,%%xmm0\n\tmovdqu %%xmm0,(%0)"::"r"(o),"r"(a),"r"(b):"memory");
    else if (mn[0]=='h') /* mulhuw */
        __asm__ volatile("movdqu (%1),%%xmm0\n\tmovdqu (%2),%%xmm1\n\tpmulhuw %%xmm1,%%xmm0\n\tmovdqu %%xmm0,(%0)"::"r"(o),"r"(a),"r"(b):"memory");
    else /* maddwd */
        __asm__ volatile("movdqu (%1),%%xmm0\n\tmovdqu (%2),%%xmm1\n\tpmaddwd %%xmm1,%%xmm0\n\tmovdqu %%xmm0,(%0)"::"r"(o),"r"(a),"r"(b):"memory");
    printf("%s %04x%04x%04x%04x%04x%04x%04x%04x\n", n, o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7]);
}
int main(void) {
    op("addusw", (const void*)W1, (const void*)W2, "addusw");
    op("addusb", (const void*)B1, (const void*)B2, "addusb");
    op("mulhuw", (const void*)W1, (const void*)W2, "hmuluw");
    op("maddwd", (const void*)W1, (const void*)W2, "maddwd");
    return 0;
}
