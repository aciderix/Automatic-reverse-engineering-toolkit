/* First native control that actually PAINTS: a BUTTON renders its push-button frame +
 * centered caption. Triggered via WM_PRINTCLIENT into a memory DIB (the testable path;
 * no SDL needed). The frame is verified STRUCTURALLY (index-based, theme-independent);
 * the caption's exact pixels depend on the resolved font (gdi_uifont env caveat), so we
 * only assert that caption pixels were drawn (textpix>0). Needs a display (control
 * creation), like the other GUI fixtures. */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }

int main(void) {
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "BPar";
    RegisterClassA(&wc);
    HWND par = CreateWindowExA(0, "BPar", "p", WS_OVERLAPPEDWINDOW, 0, 0, 200, 200, NULL, NULL, wc.hInstance, NULL);
    const int BW = 50, BH = 22;
    HWND btn = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               0, 0, BW, BH, par, (HMENU)1, wc.hInstance, NULL);
    HFONT f = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "DejaVu Sans");
    SendMessageA(btn, WM_SETFONT, (WPARAM)f, 0);

    HDC md = CreateCompatibleDC(NULL);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = BW; bi.bmiHeader.biHeight = -BH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *b = NULL; HBITMAP hb = CreateDIBSection(md, &bi, DIB_RGB_COLORS, &b, NULL, 0); SelectObject(md, hb);
    unsigned *px = (unsigned *)b; for (int i = 0; i < BW * BH; i++) px[i] = 0x00FF00FF;   /* magenta */
    SendMessageA(btn, WM_PRINTCLIENT, (WPARAM)md, PRF_CLIENT);
#define AT(x, y) (px[(y) * BW + (x)] & 0xFFFFFF)
#define SC(i)    (GetSysColor(i) & 0xFFFFFF)
    int textpix = 0;
    for (int y = 5; y < BH - 5; y++) for (int x = 12; x < BW - 12; x++) {
        unsigned c = AT(x, y); if (c != SC(COLOR_3DFACE) && c != 0xFF00FF) textpix++;
    }
    printf("frame oTL=%d oBR=%d iTL=%d face=%d | caption_drawn=%d\n",
           AT(0, 0) == SC(COLOR_BTNHIGHLIGHT), AT(BW - 1, BH - 1) == SC(COLOR_3DDKSHADOW),
           AT(1, 1) == SC(COLOR_3DLIGHT), AT(25, 11) == SC(COLOR_3DFACE) || textpix > 0, textpix > 0);
    printf("done\n");
    return 0;
}
