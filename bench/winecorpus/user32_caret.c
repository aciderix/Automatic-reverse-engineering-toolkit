#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK wp(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int main(void){
    HINSTANCE hi=GetModuleHandleW(NULL);
    WNDCLASSW wc={0}; wc.lpfnWndProc=wp; wc.hInstance=hi; wc.lpszClassName=L"CaretP";
    RegisterClassW(&wc);
    HWND h=CreateWindowExW(0,L"CaretP",L"c",WS_OVERLAPPEDWINDOW,0,0,200,150,NULL,NULL,hi,NULL);
    printf("blink_default=%u\n", GetCaretBlinkTime());
    /* pos before any caret */
    POINT p0={-1,-1}; GetCaretPos(&p0); printf("pos_before=%ld,%ld\n",(long)p0.x,(long)p0.y);
    /* set pos with no caret */
    BOOL s0=SetCaretPos(11,22); POINT p1={0,0}; GetCaretPos(&p1);
    printf("setpos_nocaret ret=%d pos=%ld,%ld\n", s0, (long)p1.x,(long)p1.y);
    /* create caret -> does it reset pos? */
    BOOL cc=CreateCaret(h,NULL,2,12); POINT p2={0,0}; GetCaretPos(&p2);
    printf("create ret=%d pos_after_create=%ld,%ld\n", cc, (long)p2.x,(long)p2.y);
    BOOL sp=SetCaretPos(30,40); POINT p3={0,0}; GetCaretPos(&p3);
    printf("setpos ret=%d pos=%ld,%ld\n", sp, (long)p3.x,(long)p3.y);
    BOOL sh=ShowCaret(h); BOOL hd=HideCaret(h);
    printf("show=%d hide=%d\n", sh, hd);
    /* blink set/get */
    BOOL sb=SetCaretBlinkTime(250); printf("setblink=%d get=%u\n", sb, GetCaretBlinkTime());
    BOOL dc=DestroyCaret(); POINT p4={0,0}; GetCaretPos(&p4);
    printf("destroy=%d pos_after_destroy=%ld,%ld blink_after=%u\n", dc,(long)p4.x,(long)p4.y, GetCaretBlinkTime());
    /* show/hide with no caret owner */
    BOOL sh2=ShowCaret(h); BOOL hd2=HideCaret(h);
    printf("show_nocaret=%d hide_nocaret=%d\n", sh2, hd2);
    printf("done\n"); return 0;
}
