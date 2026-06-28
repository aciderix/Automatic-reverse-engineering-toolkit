#include <windows.h>
#include <stdio.h>
int main(void){
  UINT icp=GetConsoleCP(), ocp=GetConsoleOutputCP();
  HANDLE h=GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD written=0;
  BOOL wc=WriteConsoleW(h,L"WCW\n",4,&written,NULL);
  printf("incp=%u outcp=%u wcok=%d wcn=%lu\n",icp,ocp,(int)wc,(unsigned long)written);
  return 0;
}
