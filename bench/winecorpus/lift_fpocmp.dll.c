/* Companion DLL exercising recovery of a lone, FPO (frame-pointer-omitted) function
 * reached ONLY through a function pointer stored in .data — the pattern a real lifted
 * libgobject/libffi hit (a GCompareFunc / dispatcher). `cmp` compiles under -O2 to a
 * leaf with no standard prologue (`mov eax,[esp+4]`), GCC aligns it to 16 with `nop`
 * padding after the previous function's `ret`, and its address is taken only by the
 * static initializer of `g_cmp` (a base-relocated .data slot) — never a direct call.
 * So static recovery must find it via the data pointer, and accept it as a function
 * START despite the nop between the padding terminator and the entry. */
#include <stdlib.h>

static int cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}
/* `volatile` stops -O2 from folding the pointer into a `mov [esp+d], offset cmp` code
 * immediate (which the code-immediate recovery path would catch): GCC must load it from
 * the base-relocated .data slot at the call, so `cmp` is reachable ONLY through the data
 * pointer — forcing the data-pointer + boundary-proof recovery this fixture guards. */
static int (*volatile g_cmp)(const void *, const void *) = cmp;

__declspec(dllexport) void dll_sort(int *arr, int n) {
    qsort(arr, n, sizeof(int), g_cmp);   /* indirect call through the recovered cmp */
}
