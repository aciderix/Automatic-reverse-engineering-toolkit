#include <windows.h>
#include <stdio.h>
int main(void){
  HANDLE f=CreateFileW(L"wd_w.txt",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  int created=(f!=INVALID_HANDLE_VALUE); DWORD wr=0; const char*msg="wide file io\n";
  if(created){ WriteFile(f,msg,13,&wr,NULL); CloseHandle(f); }
  DWORD attr=GetFileAttributesW(L"wd_w.txt"); int exists=(attr!=INVALID_FILE_ATTRIBUTES);
  HANDLE f2=CreateFileW(L"wd_w.txt",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  char buf[32]={0}; DWORD rd=0; ReadFile(f2,buf,sizeof buf-1,&rd,NULL); CloseHandle(f2);
  printf("created=%d wrote=%lu exists=%d read=%lu buf=[%.*s]\n",created,(unsigned long)wr,exists,(unsigned long)rd,(int)rd,buf);
  int del=DeleteFileW(L"wd_w.txt");
  printf("deleted=%d gone=%d\n",del,GetFileAttributesW(L"wd_w.txt")==INVALID_FILE_ATTRIBUTES);
  return 0;
}
