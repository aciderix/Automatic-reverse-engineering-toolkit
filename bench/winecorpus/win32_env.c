#include <windows.h>
#include <stdio.h>
int main(void){
  SetEnvironmentVariableA("WD_W32","value99");
  char buf[64]; DWORD n=GetEnvironmentVariableA("WD_W32",buf,sizeof buf);
  char exp[128]; DWORD m=ExpandEnvironmentStringsA("[%WD_W32%]",exp,sizeof exp);
  SetLastError(123); DWORD le=GetLastError();
  printf("get n=%lu buf=[%s] expand m=%lu exp=[%s] lasterr=%lu\n",
         (unsigned long)n,buf,(unsigned long)m,exp,(unsigned long)le);
  return 0;
}
