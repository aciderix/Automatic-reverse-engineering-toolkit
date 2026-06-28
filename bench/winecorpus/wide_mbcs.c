#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
int main(void){
  const char*a="Hello"; WCHAR w[32];
  int n=MultiByteToWideChar(CP_ACP,0,a,-1,w,32);
  char back[32]; int m=WideCharToMultiByte(CP_ACP,0,w,-1,back,32,NULL,NULL);
  printf("mb2wc=%d wclen=%zu wc2mb=%d back=[%s]\n",n,wcslen(w),m,back);
  WCHAR wc[16]; wcscpy(wc,L"abc"); wcscat(wc,L"XYZ");
  printf("wcs len=%zu cmp=%d\n", wcslen(wc), wcscmp(wc,L"abcXYZ"));
  return 0;
}
