/* Companion DLL for dll_lifting.c — real exported functions (a loop + branch in
   lift_poly exercises actual lifted control flow, not just a leaf add). */
__declspec(dllexport) int lift_add(int a, int b) { return a + b; }
__declspec(dllexport) int lift_mul(int a, int b) { return a * b; }
__declspec(dllexport) int lift_poly(int x) {
    int s = 0;
    for (int i = 0; i <= x; i++) s += i * i;
    return s; /* poly(5) = 0+1+4+9+16+25 = 55 */
}
