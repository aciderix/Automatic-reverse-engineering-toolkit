/* Proves --auto-lift auto-detects + lifts a non-system companion DLL from the exe's
 * imports alone (no --with-dll). If auto-lift failed to resolve/lift it, dll_answer
 * would hit an unimplemented import and abort; a correct 42 proves the auto path. */
#include <stdio.h>
__declspec(dllimport) int dll_answer(int x);
int main(void) {
    printf("answer=%d\n", dll_answer(20));   /* 41 */
    printf("done\n");
    return 0;
}
