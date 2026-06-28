#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(void){
  printf("proc_is_m1=%d thread_is_m2=%d\n",
    GetCurrentProcess()==(HANDLE)(LONG_PTR)-1, GetCurrentThread()==(HANDLE)(LONG_PTR)-2);
  printf("mod_self=%d\n", GetModuleHandleA(NULL)!=NULL);
  STARTUPINFOW si; memset(&si,0,sizeof si);
  GetStartupInfoW(&si);
  printf("startup_flags=%lu\n", (unsigned long)si.dwFlags);
  char a[512]; DWORD na=GetModuleFileNameA(NULL,a,sizeof a);
  WCHAR w[512]; DWORD nw=GetModuleFileNameW(NULL,w,512);
  printf("modfileA=%d modfileW=%d\n", (int)(na>0&&a[0]), (int)(nw>0&&w[0]));
  HMODULE hm=NULL; BOOL ge=GetModuleHandleExW(0,NULL,&hm);
  printf("getmodex=%d hm_nonnull=%d\n",(int)ge, hm!=NULL);
  return 0;
}
