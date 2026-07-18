/* DLL lifting — the lifted comctl32's DllMain runs at startup (doc 80 §1.2), so
   comctl32 registers its control window classes into ARET's HLE exactly as Wine
   does at DLL load. Proof: the classes exist BEFORE InitCommonControlsEx is even
   called (that is the DllMain's doing), and GetClassInfo finds each — bit-
   identical to Wine. (The ATOM value GetClassInfo returns is an opaque handle,
   so we compare only registered-or-not.) */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
int main(void) {
    WNDCLASSA wc;
    printf("preinit_progress=%d\n", GetClassInfoA(NULL, "msctls_progress32", &wc) != 0);
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_PROGRESS_CLASS | ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);
    printf("progress=%d trackbar=%d toolbar=%d status=%d bogus=%d\n",
           GetClassInfoA(NULL, "msctls_progress32", &wc) != 0,
           GetClassInfoA(NULL, "msctls_trackbar32", &wc) != 0,
           GetClassInfoA(NULL, "ToolbarWindow32", &wc) != 0,
           GetClassInfoA(NULL, "msctls_statusbar32", &wc) != 0,
           GetClassInfoA(NULL, "NoSuchClass99", &wc) != 0);
    return 0;
}
