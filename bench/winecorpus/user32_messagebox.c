/* Exercises MessageBoxA in the display-free tier (G5, doc 72). A modal message
 * box needs a display + a user; with none, the ground truth (Wine with DISPLAY
 * unset) returns -1 immediately without blocking. ARET returns the same -1 — the
 * honest "no display available" answer, not a guessed button. The NAME.nodisplay
 * marker makes winediff run BOTH engines with DISPLAY unset, so the comparison is
 * apples-to-apples and deterministic (no hang on a real window). A real dialog
 * arrives with SDL (G2b). */
#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("ok   =%d\n", MessageBoxA(NULL, "info text", "caption", MB_OK));
    printf("okc  =%d\n", MessageBoxA(NULL, "proceed?", "caption", MB_OKCANCEL));
    printf("yn   =%d\n", MessageBoxA(NULL, "yes or no", "caption", MB_YESNO | MB_ICONQUESTION));
    printf("done\n");
    return 0;
}
