/* Multiline EDIT (ES_MULTILINE) text model vs Wine: line count/index/length count the CRLF
   in offsets, EM_GETLINE excludes it, EM_GETSEL/SETSEL/LINEFROMCHAR/REPLACESEL splice the
   selection. Also exercises the SetWindowTextW/GetWindowTextW wide path (a W string must not
   be read as ANSI). Display-free: pure text-model state, byte-identical to Wine. */
#include <windows.h>
#include <stdio.h>
static LRESULT CALLBACK wp(HWND h,UINT m,WPARAM w,LPARAM l){return DefWindowProcW(h,m,w,l);}
int main(void){
    HINSTANCE hi=GetModuleHandleW(NULL);
    WNDCLASSW wc={0}; wc.lpfnWndProc=wp; wc.hInstance=hi; wc.lpszClassName=L"MlE";
    RegisterClassW(&wc);
    HWND h=CreateWindowExW(0,L"MlE",L"m",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,hi,NULL);
    HWND e=CreateWindowExW(0,L"EDIT",NULL,WS_CHILD|ES_MULTILINE|ES_AUTOVSCROLL,
                           0,0,380,260,h,(HMENU)1,hi,NULL);
    printf("edit=%d\n", e!=NULL);
    SetWindowTextW(e, L"abc\r\ndef\r\nghijk");
    int lc=(int)SendMessageW(e,EM_GETLINECOUNT,0,0);
    printf("linecount=%d\n", lc);
    for(int i=0;i<lc;i++){
        int idx=(int)SendMessageW(e,EM_LINEINDEX,i,0);
        int len=(int)SendMessageW(e,EM_LINELENGTH,idx,0);
        WCHAR lb[64]; lb[0]=(WCHAR)62; /* set buffer size word (EM_GETLINE convention) */
        int got=(int)SendMessageW(e,EM_GETLINE,i,(LPARAM)lb); lb[got]=0;
        printf("line%d idx=%d len=%d got=%d s=%ls\n", i, idx, len, got, lb);
    }
    int tl=(int)SendMessageW(e,WM_GETTEXTLENGTH,0,0);
    printf("textlen=%d\n", tl);
    /* selection */
    SendMessageW(e,EM_SETSEL,5,9);
    DWORD s=0,en=0; SendMessageW(e,EM_GETSEL,(WPARAM)&s,(LPARAM)&en);
    printf("sel=%lu,%lu\n",(unsigned long)s,(unsigned long)en);
    int lfc=(int)SendMessageW(e,EM_LINEFROMCHAR,7,0);
    printf("linefromchar7=%d\n", lfc);
    SendMessageW(e,EM_REPLACESEL,0,(LPARAM)L"XY");
    WCHAR all[128]={0}; GetWindowTextW(e,all,128);
    printf("aftersel_len=%d\n",(int)wcslen(all));
    printf("done\n"); return 0;
}
