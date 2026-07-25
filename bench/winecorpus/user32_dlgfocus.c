/* Dialog manager initial keyboard focus. After WM_INITDIALOG the dialog manager gives
   focus to the first tab-stop control (GetNextDlgTabItem(hDlg, NULL, FALSE)) when the
   DLGPROC returns TRUE; when it returns FALSE the DLGPROC has set focus itself and the
   manager leaves it. We report which control ID has focus (GetDlgCtrlID(GetFocus())),
   which is deterministic (HWNDs are not) — measured against Wine so our
   u32_dialog_default_focus matches. Runs under the harness Xvfb. */
#include <windows.h>
#include <stdio.h>

static BYTE tpl[512];
static int off;
static void al4(void)    { while (off & 3) tpl[off++] = 0; }
static void w16(WORD v)  { tpl[off++] = (BYTE)v; tpl[off++] = (BYTE)(v >> 8); }
static void w32(DWORD v) { w16((WORD)v); w16((WORD)(v >> 16)); }
static void wstr(const char *s) { while (*s) w16((WORD)(BYTE)*s++); w16(0); }

/* Build a 3-control dialog: a non-tab-stop STATIC (label, id 99), then a tab-stop
   BUTTON (id 100), then a tab-stop EDIT (id 101). The first tab-stop is the button. */
static void build(void) {
    off = 0;
    w32(DS_SETFONT | WS_POPUP | WS_CAPTION); w32(0);
    w16(3);                                  /* cdit */
    w16(0); w16(0); w16(200); w16(120);
    w16(0); w16(0);                          /* menu, class */
    wstr("Dlg");
    w16(8); wstr("DejaVu Sans");
    al4();                                   /* STATIC label, NOT a tab-stop */
    w32(WS_CHILD | WS_VISIBLE | SS_LEFT); w32(0);
    w16(10); w16(6); w16(80); w16(10); w16(99);
    w16(0xFFFF); w16(0x0082);                /* class atom = Static */
    wstr("Name:"); w16(0);
    al4();                                   /* BUTTON, tab-stop -> first tab item */
    w32(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON); w32(0);
    w16(10); w16(20); w16(50); w16(14); w16(100);
    w16(0xFFFF); w16(0x0080);                /* class atom = Button */
    wstr("OK"); w16(0);
    al4();                                   /* EDIT, tab-stop */
    w32(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER); w32(0);
    w16(10); w16(40); w16(60); w16(12); w16(101);
    w16(0xFFFF); w16(0x0081);                /* class atom = Edit */
    wstr(""); w16(0);
}

static INT_PTR CALLBACK ProcTrue(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)w; (void)l;
    return m == WM_INITDIALOG ? TRUE : FALSE;   /* TRUE -> manager sets default focus */
}
static INT_PTR CALLBACK ProcFalse(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_INITDIALOG) { SetFocus(GetDlgItem(h, 101)); return FALSE; }  /* app keeps focus */
    return FALSE;
}

static int focus_id(void) { HWND f = GetFocus(); return f ? GetDlgCtrlID(f) : 0; }

int main(void) {
    HINSTANCE hi = GetModuleHandleA(0);
    build();
    HWND a = CreateDialogIndirectParamA(hi, (LPCDLGTEMPLATE)tpl, NULL, ProcTrue, 0);
    printf("initret=TRUE  focus_id=%d\n", a ? focus_id() : -1);   /* expect first tab-stop = 100 */
    if (a) DestroyWindow(a);

    build();
    HWND b = CreateDialogIndirectParamA(hi, (LPCDLGTEMPLATE)tpl, NULL, ProcFalse, 0);
    printf("initret=FALSE focus_id=%d\n", b ? focus_id() : -1);   /* expect app's choice = 101 */
    if (b) DestroyWindow(b);
    printf("done\n");
    return 0;
}
