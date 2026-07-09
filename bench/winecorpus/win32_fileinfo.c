#include <windows.h>
#include <stdio.h>
int main(void){
  HANDLE f=CreateFileA("wd_fi.tmp",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  if(f==INVALID_HANDLE_VALUE){printf("CREATE_FAIL\n");return 1;}
  DWORD wr=0; WriteFile(f,"0123456789ABCDEF",16,&wr,NULL);
  CloseHandle(f);
  f=CreateFileA("wd_fi.tmp",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  if(f==INVALID_HANDLE_VALUE){printf("OPEN_FAIL\n");return 1;}
  BY_HANDLE_FILE_INFORMATION bi;
  BOOL ok=GetFileInformationByHandle(f,&bi);
  CloseHandle(f); DeleteFileA("wd_fi.tmp");
  if(!ok){printf("QUERY_FAIL\n");return 1;}
  unsigned long long size=((unsigned long long)bi.nFileSizeHigh<<32)|bi.nFileSizeLow;
  int is_dir=(bi.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?1:0;
  printf("size=%llu dir=%d links=%lu\n",size,is_dir,(unsigned long)bi.nNumberOfLinks);
  return 0;
}
