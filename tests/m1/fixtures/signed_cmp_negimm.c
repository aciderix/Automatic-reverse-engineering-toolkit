#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);
/* Signed compare against a negative 32-bit immediate (cmp r32, imm32; jge).
   The immediate -1000999 (0xfff0b9d9) must be sign-extended, not read as
   +4293913049. This is exactly Lua's index2value pseudo-index test. */
static int __attribute__((noinline)) ge_neg(int x) { return x >= -1000999; }
void __stdcall mainCRTStartup(void) {
    printf("SIGNCMP: %d %d %d\n", ge_neg(-1), ge_neg(-2000000), ge_neg(5));
    ExitProcess(0);  /* expect 1 0 1 */
}
