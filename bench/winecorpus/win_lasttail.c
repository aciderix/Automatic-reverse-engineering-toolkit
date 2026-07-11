/* Final long-tail: MoveFileA (rename), CreateBitmap+GetObject, ShowCursor counter,
 * GetLastActivePopup, RegDeleteValueA (empty hive -> not found). Measured vs Wine. */
#include <windows.h>
#include <stdio.h>

int main(void) {
    /* MoveFileA round-trip */
    HFILE f = _lcreat("mvsrc.dat", 0); _lwrite(f, "data", 4); _lclose(f);
    int mv = MoveFileA("mvsrc.dat", "mvdst.dat");
    HFILE g = _lopen("mvdst.dat", OF_READ);
    char b[8]; int rd = (g != HFILE_ERROR) ? _lread(g, b, 4) : -1; if (g != HFILE_ERROR) _lclose(g);
    b[rd > 0 ? rd : 0] = 0;
    int srcgone = (_lopen("mvsrc.dat", OF_READ) == HFILE_ERROR);
    printf("move=%d read=%d [%s] srcgone=%d\n", mv, rd, b, srcgone);

    /* CreateBitmap + GetObject */
    HBITMAP bm = CreateBitmap(6, 4, 1, 32, NULL);
    BITMAP bi; int r = GetObjectA(bm, sizeof bi, &bi);
    printf("cbmp r=%d w=%ld h=%ld bpp=%d\n", r, bi.bmWidth, bi.bmHeight, bi.bmBitsPixel);
    DeleteObject(bm);

    /* ShowCursor counter (sequence to avoid printf arg-order ambiguity) */
    int c1 = ShowCursor(TRUE);
    int c2 = ShowCursor(FALSE);
    printf("cursor show=%d hide=%d\n", c1, c2);

    printf("popup_self=%d regdel=%d\n", GetLastActivePopup((HWND)0x1) == (HWND)0x1,
           RegDeleteValueA(HKEY_CURRENT_USER, "nope"));
    printf("done\n");
    return 0;
}
