/* Minimal reproduction of a mislift when the FUNCTION ENTRY BLOCK is itself a
   loop header reached by a CONDITIONAL back-edge, and the induction variable is
   a register parameter (regparm). SSA construction placed a phi at the header
   for that register but had no predecessor block for the entry edge, so the
   phi's initial (parameter) value was lost — the induction variable never
   advanced -> infinite loop / wrong result. Fix: split the entry block (insert
   an empty pre-header owning the entry VA) so the header gets a clean entry edge.

   This is the minimized sqlite3-mingw `sqlite3ExprAffinity` crash (CREATE TABLE
   segfaulted / hung before the fix). `walk` is hand-written asm so the shape is
   exact and stable across compilers: entry (`movzbl (%eax)`) IS the back-edge
   target of `jne _walk`, with eax (the param) both read there and advanced.

   Build: i686-w64-mingw32-gcc -O2 -o loop_header_entry.exe loop_header_entry.c
   Expected output (native and ARET): "7 7 7 0". */
#include <stdio.h>

struct N { unsigned char op; unsigned char pad; struct N *left; };

/* eax = e (regparm). Entry == loop header, conditional back-edge to entry. */
__asm__(
    ".text\n"
    ".globl _walk\n"
"_walk:\n"
    "    movzbl (%eax), %edx\n"   /* ENTRY == loop header: edx = e->op   */
    "    cmp $1, %dl\n"
    "    jne .Lwexit\n"            /* op != 1 -> done                     */
    "    movl 4(%eax), %eax\n"    /* e = e->left                         */
    "    testl %eax, %eax\n"
    "    jne _walk\n"             /* CONDITIONAL back-edge to the header  */
    "    xorl %edx, %edx\n"       /* e == NULL -> op = 0                  */
".Lwexit:\n"
    "    movzbl %dl, %eax\n"
    "    ret\n");

extern int __attribute__((regparm(3))) walk(struct N *e);

/* opaque: keep the compiler from constant-folding the calls away */
static struct N *pick(struct N *x) { return x; }

int main(void) {
    struct N c = { 7, 0, 0 };
    struct N b = { 1, 0, &c };
    struct N a = { 1, 0, &b };
    struct N z = { 1, 0, 0 };
    printf("%d %d %d %d\n", walk(pick(&a)), walk(pick(&b)), walk(pick(&c)), walk(pick(&z)));
    return 0;
}
