/* Deterministic reproduction of the mingw ___chkstk_ms register bug.
 * ___chkstk_ms only probes guard pages; it saves/restores ecx (push ecx/pop ecx)
 * and leaves esp untouched, so a value staged in ecx BEFORE the call is still
 * there AFTER. mingw relies on exactly this: `mov ecx,len; call ___chkstk_ms;
 * sub esp,eax; rep stos` (an alloca immediately zeroed). If ARET models the call
 * as clobbering caller-saved ecx, the recovered value is lost. We stage a known
 * value in ecx, call the probe with a small (sub-page) size so esp never moves,
 * and read ecx back: it must equal the input. */
#include <stdio.h>

extern void __chkstk_ms(void);   /* GCC/mingw guard-page probe helper */

__attribute__((noinline))
static unsigned ecx_survives_chkstk(unsigned x)
{
	unsigned r;
	__asm__ volatile(
		"movl %[x], %%ecx\n\t"   /* stage a known value in caller-saved ecx   */
		"movl $16, %%eax\n\t"    /* size < one page: the probe does not move esp */
		"call ___chkstk_ms\n\t"  /* preserves every GP register (push/pop ecx) */
		"movl %%ecx, %[r]\n\t"   /* recover ecx: == x iff it was preserved      */
		: [r] "=r"(r)
		: [x] "r"(x)
		: "eax", "ecx", "cc", "memory");
	return r;
}

int main(void)
{
	unsigned got = ecx_survives_chkstk(0x1234u);
	printf("ecx=0x%x %s\n", got, got == 0x1234u ? "OK" : "LOST");
	return got == 0x1234u ? 0 : 1;
}
