/* Regression for Phase 3 — stripped-binary recovery + FLIRT of statically-linked
   CRT/startup-glue. Built STRIPPED (`-s`, no symbols), it exercises:

     * a CRT function used at runtime (qsort with an indirect callback, sprintf),
     * the mingw startup glue registered by `__main` and run at exit —
       `atexit(___do_global_dtors)`, an indirect call to a glue routine whose
       address is taken by immediate (`mov [esp],0x<dtors>; call atexit`).

   With no symbols, ARET must (a) wildcard the base-relocated operands in its
   FLIRT signatures so `__pei386_runtime_relocator` & co. match this binary, and
   (b) let a FLIRT hit seed address-taken recovery so `___do_global_dtors` is
   recovered and host-backed. Otherwise the glue is lifted (hitting unmodelled
   x87 / an indirect call to an unrecovered address) and the program aborts at
   exit. Expected output (matches Wine): "SORTED: 11 22 33 48 59 70 85 96".

   Build (stripped):
     i686-w64-mingw32-gcc -O1 -s stripped_crt.c -o stripped_crt.exe */
#include <stdio.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b; /* qsort callback (indirect call) */
}

int main(void) {
    int v[8];
    for (int i = 0; i < 8; i++) v[i] = (i * 37 + 11) % 100;
    qsort(v, 8, sizeof(int), cmp);
    char buf[128];
    int n = 0;
    for (int i = 0; i < 8; i++) n += sprintf(buf + n, "%d ", v[i]);
    printf("SORTED: %s\n", buf);
    return 0;
}
