/* comctl32 socle batch 5: DC clip regions + window regions + FillRgn/FrameRgn.
 * The DC user-clip is a rectangular bbox + complex flag; the region-type returns
 * (NULL=1/SIMPLE=2/COMPLEX=3) are measured bit-exact vs Wine — IntersectClipRect
 * (rect ∩ rect = simple), ExcludeClipRect (interior cut = complex), SelectClipRgn(NULL)
 * = simple + clears the clip, GetClipRgn (0 = none / 1 = set with the box), RectVisible.
 * FillRgn/FrameRgn paint the rectangular region exactly (a complex region aborts loud
 * rather than paint its bbox). SetWindowRgn/GetWindowRgn round-trip the window region.
 * Windowed (GetDC(NULL)/CreateWindow need the Xvfb display). Bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WndP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
int main(void){
    HDC sdc=GetDC(NULL); HDC dc=CreateCompatibleDC(sdc);
    HBITMAP bm=CreateCompatibleBitmap(sdc,64,64); SelectObject(dc,bm);
    HRGN r=CreateRectRgn(0,0,0,0);
    printf("getclip_initial=%d\n",GetClipRgn(dc,r));
    printf("isect1=%d\n",IntersectClipRect(dc,10,10,50,50));
    RECT b; GetClipRgn(dc,r); GetRgnBox(r,&b);
    printf("getclip1 box=%ld,%ld,%ld,%ld\n",(long)b.left,(long)b.top,(long)b.right,(long)b.bottom);
    printf("isect2=%d\n",IntersectClipRect(dc,20,20,60,60));
    GetClipRgn(dc,r); GetRgnBox(r,&b);
    printf("getclip2 box=%ld,%ld,%ld,%ld\n",(long)b.left,(long)b.top,(long)b.right,(long)b.bottom);
    printf("exclude=%d\n",ExcludeClipRect(dc,30,30,40,40));
    RECT rv={22,22,25,25},rv2={0,0,5,5};
    printf("visible_in=%d visible_out=%d\n",RectVisible(dc,&rv),RectVisible(dc,&rv2));
    printf("selectnull=%d\n",SelectClipRgn(dc,NULL));
    printf("getclip_after_null=%d\n",GetClipRgn(dc,r));

    HDC fdc=CreateCompatibleDC(sdc); HBITMAP fbm=CreateCompatibleBitmap(sdc,64,64); SelectObject(fdc,fbm);
    HRGN fr=CreateRectRgn(10,10,30,30); HBRUSH red=CreateSolidBrush(RGB(255,0,0));
    FillRgn(fdc,fr,red);
    printf("fill_in=%06lX fill_out=%06lX\n",(unsigned long)GetPixel(fdc,20,20),(unsigned long)GetPixel(fdc,5,5));
    HRGN fr2=CreateRectRgn(0,0,40,40); HBRUSH blue=CreateSolidBrush(RGB(0,0,255));
    FrameRgn(fdc,fr2,blue,2,2);
    printf("frame_edge=%06lX frame_mid=%06lX\n",(unsigned long)GetPixel(fdc,0,0),(unsigned long)GetPixel(fdc,20,3));

    WNDCLASSA wc;memset(&wc,0,sizeof wc);wc.lpfnWndProc=WndP;wc.hInstance=GetModuleHandleA(0);wc.lpszClassName="RW5";RegisterClassA(&wc);
    HWND h=CreateWindowExA(0,"RW5","w",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,GetModuleHandleA(0),NULL);
    int g0=GetWindowRgn(h,r);
    HRGN wr=CreateRectRgn(0,0,100,80); SetWindowRgn(h,wr,FALSE);
    int g1=GetWindowRgn(h,r); GetRgnBox(r,&b);
    printf("wrgn_none=%d wrgn_type=%d box=%ld,%ld,%ld,%ld\n",g0,g1,(long)b.left,(long)b.top,(long)b.right,(long)b.bottom);
    DestroyWindow(h);
    printf("done\n");return 0;
}
