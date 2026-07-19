/* MapDialogRect — dialog-units -> pixels via the dialog's font base units. A dialog's
 * control positions live in "dialog units" (a grid tied to the font: 1 horizontal DU =
 * 1/4 avg-char-width, 1 vertical DU = 1/8 char-height), so the pixel geometry depends on
 * the resolved dialog font. We reproduce Wine's base-unit formula (GdiGetCharDimensions)
 * exactly and autonomously. Verified bit-exact by naming a font BOTH engines resolve
 * identically ("DejaVu Sans", like gdi_textout) -> same TTF -> same metrics -> same units.
 * MapDialogRect({0,0,4,8}) recovers the base units directly (MulDiv(4,ux,4)=ux etc.);
 * a larger rect exercises the MulDiv scaling+rounding. Modeless (no EndDialog needed),
 * no display needed for the unit math. */
#include <windows.h>
#include <stdio.h>
#pragma pack(push, 1)
typedef struct {
    DLGTEMPLATE dt;      /* style, dwExtendedStyle, cdit, x, y, cx, cy — 18 bytes */
    WORD menu;           /* 0 = no menu   */
    WORD windowClass;    /* 0 = default   */
    WCHAR title[4];      /* "Tst\0"       */
    WORD pointSize;      /* DS_SETFONT: point size */
    WCHAR typeface[11];  /* "DejaVu Sans\0" */
} DLG;
#pragma pack(pop)

static INT_PTR CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)m; (void)w; (void)l;
    return FALSE;   /* modeless: nothing to do at WM_INITDIALOG */
}
int main(void) {
    DLG d; memset(&d, 0, sizeof d);
    d.dt.style = DS_SETFONT | WS_POPUP | WS_CAPTION;   /* not visible: pure unit math */
    d.dt.cdit = 0;
    d.dt.cx = 160; d.dt.cy = 100;
    const WCHAR t[] = L"Tst";     memcpy(d.title, t, sizeof t);
    d.pointSize = 8;
    const WCHAR f[] = L"DejaVu Sans"; memcpy(d.typeface, f, sizeof f);

    HWND h = CreateDialogIndirectParamA(GetModuleHandleA(0), &d.dt, NULL, Proc, 0);
    if (!h) { printf("no dialog\n"); return 1; }

    RECT u = { 0, 0, 4, 8 };      /* -> (base_x, base_y) exactly */
    MapDialogRect(h, &u);
    RECT b = { 2, 3, 160, 100 };  /* -> scaled+rounded pixels */
    MapDialogRect(h, &b);
    printf("base %ld %ld\n", (long)u.right, (long)u.bottom);
    printf("rect %ld %ld %ld %ld\n", (long)b.left, (long)b.top, (long)b.right, (long)b.bottom);
    DestroyWindow(h);
    printf("done\n");
    return 0;
}
