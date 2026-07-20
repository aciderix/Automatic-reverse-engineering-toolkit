/* comctl32 socle batch 2: in-memory clipboard round-trip (Set/Get/IsFormatAvailable)
 * + caret position state. Both bit-identical to Wine. (IMM / misc no-ops are sound
 * display/no-IME stubs, not separately asserted.) */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK WP(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProc(h,m,w,l);}
int main(void){
 WNDCLASSA wc;memset(&wc,0,sizeof wc);wc.lpfnWndProc=WP;wc.hInstance=GetModuleHandleA(0);wc.lpszClassName="CL";RegisterClassA(&wc);
 HWND w=CreateWindowExA(0,"CL","p",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,GetModuleHandleA(0),NULL);
 // clipboard round-trip
 if(OpenClipboard(w)){
   EmptyClipboard();
   HGLOBAL hm=GlobalAlloc(GMEM_MOVEABLE,6); char*p=(char*)GlobalLock(hm); strcpy(p,"Hello"); GlobalUnlock(hm);
   SetClipboardData(CF_TEXT,hm);
   CloseClipboard();
 }
 int avail=0; char got[16]="";
 if(OpenClipboard(w)){
   avail=IsClipboardFormatAvailable(CF_TEXT);
   HANDLE h=GetClipboardData(CF_TEXT);
   if(h){ char*p=(char*)GlobalLock(h); if(p){strncpy(got,p,15);} GlobalUnlock(h); }
   CloseClipboard();
 }
 printf("clip avail=%d text=[%s]\n",avail,got);
 // caret
 CreateCaret(w,NULL,2,12); SetCaretPos(7,9);
 POINT pt={0,0}; GetCaretPos(&pt);
 printf("caret %ld %ld\n",(long)pt.x,(long)pt.y);
 DestroyCaret();
 DestroyWindow(w);
 printf("done\n");return 0;
}
