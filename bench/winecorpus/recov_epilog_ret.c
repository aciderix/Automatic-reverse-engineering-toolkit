/* Recovery: a function's own epilogue `ret` stolen by bare-ret-stub recovery.
 *
 * DRIVER: the WinMerge (MFC90) null-deref `0xC0000005 at 0x10` — a lifted MSVC C++
 * constructor returned 0 instead of `this`, so `p->member = new T(...)` stored NULL
 * and a later `[this+0x10]` deref faulted. Root cause (measured to the instruction):
 * the constructor is `… ; mov eax,this ; call _EH_epilog3 ; ret`. Its lone epilogue
 * `ret` had its address taken (an EH/vtable table), so ARET recovered that single
 * `ret` as a standalone bare-`ret` function. That made the `ret`'s address a
 * TRUNCATING BOUNDARY: `collect_function` stopped *before* the constructor's own
 * `ret`, leaving the `call _EH_epilog3` block with a fall-through absent from the
 * function → `build_ir` produced no `Return` terminator → `emit` synthesised a
 * fallback `return 0`, dropping the `this` already in eax.
 *
 * This fixture reproduces it MINIMALLY (mingw cannot emit MSVC `_EH_prolog3`/
 * `_EH_epilog3`, so they are hand-written, as the SEH bricks do): `probe` is a
 * thiscall ctor that saves `this` (ecx), calls a helper, reloads `this` into eax,
 * and returns via the eax-preserving `_EH_epilog3`. `g_taken` takes the address of
 * probe's epilogue `ret`, which is what triggers the bare-ret-stub recovery. The
 * `.def` forces the `_except_handler3` import so SEH-establish is active (matches
 * the driver). Expected under Wine and ARET: probe returns its `this` (0x1234). */
#include <stdio.h>
int __cdecl _except_handler3(void);
unsigned int mycookie = 0xb0b1b2b3;
int intermediate(void) { return 7; }
extern int call_probe(int this_);
extern void probe_ret(void); /* probe's epilogue `ret`; its address is taken below */
void *volatile g_taken = (void *)probe_ret;

__asm__(
".text\n"
".globl _eh_prolog3\n"
"_eh_prolog3:\n"
"  push %eax\n  push %fs:0\n  lea 0xc(%esp), %eax\n  sub 0xc(%esp), %esp\n"
"  push %ebx\n  push %esi\n  push %edi\n  mov %ebp, (%eax)\n  mov %eax, %ebp\n"
"  mov _mycookie, %eax\n  xor %ebp, %eax\n  push %eax\n  push -0x4(%ebp)\n"
"  movl $0xffffffff, -0x4(%ebp)\n  lea -0xc(%ebp), %eax\n  mov %eax, %fs:0\n  ret\n"
".globl _eh_epilog3\n"
"_eh_epilog3:\n"
"  mov -0xc(%ebp), %ecx\n  mov %ecx, %fs:0\n  pop %ecx\n"
"  pop %edi\n  pop %edi\n  pop %esi\n  pop %ebx\n  mov %ebp, %esp\n  pop %ebp\n  push %ecx\n  ret\n"
".globl _probe\n"          /* thiscall ctor: `this` in ecx, returns `this` */
"_probe:\n"
"  push $0x4\n  mov $_intermediate, %eax\n  call _eh_prolog3\n"
"  mov %ecx, -0x10(%ebp)\n  call _intermediate\n  mov -0x10(%ebp), %eax\n  call _eh_epilog3\n"
".globl _probe_ret\n"
"_probe_ret:\n"
"  ret\n"
".globl _call_probe\n"
"_call_probe:\n"
"  mov 4(%esp), %ecx\n  call _probe\n  ret\n"
);

int main(int argc, char **argv) {
    if (argc < 0) _except_handler3();
    (void)g_taken;
    int r = call_probe(0x1234);
    printf("probe=0x%x expect=0x1234 %s\n", r, r == 0x1234 ? "OK" : "BUG");
    return 0;
}
