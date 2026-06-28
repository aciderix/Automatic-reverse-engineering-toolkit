#include <windows.h>
#include <stdio.h>
int main(void){
  HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode=0; BOOL iscon=GetConsoleMode(h,&mode);
  DWORD ft=GetFileType(h);
  printf("isconsole=%d filetype=%lu valid=%d\n",(int)iscon,(unsigned long)ft,
         (h!=INVALID_HANDLE_VALUE && h!=NULL));
  return 0;
}
