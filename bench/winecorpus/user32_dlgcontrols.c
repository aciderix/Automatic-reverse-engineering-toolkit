/* Dialog control geometry: a DS_SETFONT dialog (DejaVu Sans, resolved identically by
 * both engines) with two controls at known dialog-unit positions. We read each control
 * back relative to the dialog client (GetWindowRect then MapWindowPoints to the dialog,
 * so the dialog's screen placement cancels — bit-exact independent of the window
 * manager) plus its class name, and the dialog's own client size. All are driven by the
 * dialog units -> pixels conversion (base units from the font), so identical to Wine.
 * The template is built byte-by-byte with correct DWORD alignment of each item. */
#include <windows.h>
#include <stdio.h>

static BYTE tpl[512];
static int off;
static void al4(void)        { while (off & 3) tpl[off++] = 0; }
static void w16(WORD v)      { tpl[off++] = (BYTE)v; tpl[off++] = (BYTE)(v >> 8); }
static void w32(DWORD v)     { w16((WORD)v); w16((WORD)(v >> 16)); }
static void wstr(const char *s) { while (*s) w16((WORD)(BYTE)*s++); w16(0); }

static INT_PTR CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)m; (void)w; (void)l; return FALSE;
}
int main(void) {
    /* DLGTEMPLATE */
    w32(DS_SETFONT | WS_POPUP | WS_CAPTION);   /* style   */
    w32(0);                                    /* exStyle */
    w16(2);                                    /* cdit    */
    w16(0); w16(0); w16(200); w16(120);        /* x,y,cx,cy (dialog units) */
    w16(0);                                    /* menu    */
    w16(0);                                    /* class   */
    wstr("Dlg");                               /* caption */
    w16(8); wstr("DejaVu Sans");               /* DS_SETFONT: point size + typeface */
    /* item 1 — BUTTON "OK" */
    al4();
    w32(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON); w32(0);
    w16(10); w16(20); w16(50); w16(14);        /* x,y,cx,cy */
    w16(100);                                  /* id */
    w16(0xFFFF); w16(0x0080);                  /* class atom = Button */
    wstr("OK"); w16(0);                        /* caption + 0 creation bytes */
    /* item 2 — EDIT */
    al4();
    w32(WS_CHILD | WS_VISIBLE | WS_BORDER); w32(0);
    w16(10); w16(40); w16(60); w16(12);
    w16(101);
    w16(0xFFFF); w16(0x0081);                  /* class atom = Edit */
    wstr(""); w16(0);

    HWND h = CreateDialogIndirectParamA(GetModuleHandleA(0), (LPCDLGTEMPLATE)tpl, NULL, Proc, 0);
    if (!h) { printf("no dialog\n"); return 1; }

    RECT dc; GetClientRect(h, &dc);
    printf("dlg client w=%ld h=%ld\n", (long)dc.right, (long)dc.bottom);
    int ids[2] = { 100, 101 };
    for (int i = 0; i < 2; i++) {
        HWND c = GetDlgItem(h, ids[i]);
        if (!c) { printf("ctl %d: missing\n", ids[i]); continue; }
        RECT rc; GetWindowRect(c, &rc);
        MapWindowPoints(NULL, h, (POINT *)&rc, 2);   /* -> dialog-client coords */
        char cls[32] = ""; GetClassNameA(c, cls, sizeof cls);
        printf("ctl %d: x=%ld y=%ld w=%ld h=%ld cls=%s\n", ids[i],
               (long)rc.left, (long)rc.top, (long)(rc.right - rc.left),
               (long)(rc.bottom - rc.top), cls);
    }
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
