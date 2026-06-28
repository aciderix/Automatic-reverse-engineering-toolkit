#include <windows.h>
#include <stdio.h>
int main(void){
  HANDLE f=CreateFileA("wd_w32.tmp",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  if(f==INVALID_HANDLE_VALUE){printf("CREATE_FAIL\n");return 1;}
  const char*msg="windows file io 123\n"; DWORD wr=0;
  WriteFile(f,msg,(DWORD)lstrlenA(msg),&wr,NULL);
  CloseHandle(f);
  f=CreateFileA("wd_w32.tmp",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  if(f==INVALID_HANDLE_VALUE){printf("OPEN_FAIL\n");return 1;}
  char buf[64]={0}; DWORD rd=0;
  ReadFile(f,buf,sizeof buf-1,&rd,NULL);
  DWORD sz=GetFileSize(f,NULL);
  printf("wrote=%lu read=%lu size=%lu buf=[%.*s]\n",(unsigned long)wr,(unsigned long)rd,(unsigned long)sz,(int)rd-1,buf);
  CloseHandle(f); DeleteFileA("wd_w32.tmp");
  return 0;
}
