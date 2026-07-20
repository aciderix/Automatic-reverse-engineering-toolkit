/* comctl32 socle shims (Levier-1 batch 1) — the simple, high-breadth, Wine-verifiable
 * subset the lifted comctl32 needs: CompareFileTime, IsChild, GetObjectType,
 * CharUpper/LowerBuffW, GetBkMode round-trip, GetDoubleClickTime. Each bit-identical to
 * Wine. (StrCmpIW/StrCmpNIW, Monitor*, GetDpiForWindow are also implemented but not
 * link-testable via mingw / are env invariants.) */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
int main(void){
 // CompareFileTime
 FILETIME a={100,0},b={200,0},c={100,0};
 printf("cft %d %d %d\n",CompareFileTime(&a,&b),CompareFileTime(&b,&a),CompareFileTime(&a,&c));
 // IsChild
 WNDCLASSA wc;memset(&wc,0,sizeof wc);wc.lpfnWndProc=WP;wc.hInstance=GetModuleHandleA(0);wc.lpszClassName="SW";RegisterClassA(&wc);
 HWND par=CreateWindowExA(0,"SW","p",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,GetModuleHandleA(0),NULL);
 HWND ch=CreateWindowExA(0,"BUTTON","b",WS_CHILD,0,0,50,20,par,0,GetModuleHandleA(0),NULL);
 printf("ischild %d %d\n",IsChild(par,ch),IsChild(ch,par));
 // GetObjectType
 HBRUSH br=CreateSolidBrush(RGB(1,2,3)); HPEN pn=CreatePen(PS_SOLID,1,0);
 printf("objtype brush=%d pen=%d\n",(int)GetObjectType(br),(int)GetObjectType(pn));
 // StrCmpIW / StrCmpNIW
 // CharUpperBuffW / CharLowerBuffW
 WCHAR buf[]=L"aBc"; CharUpperBuffW(buf,3); printf("upper=%d%d%d\n",buf[0],buf[1],buf[2]);
 WCHAR bl[]=L"aBc"; CharLowerBuffW(bl,3); printf("lower=%d%d%d\n",bl[0],bl[1],bl[2]);
 // GetBkMode round-trip
 HDC dc=GetDC(par); SetBkMode(dc,TRANSPARENT);
 printf("bkmode %d\n",GetBkMode(dc)); ReleaseDC(par,dc);
 printf("dclick %d\n", GetDoubleClickTime());
 DestroyWindow(par);
 printf("done\n");return 0;
}
