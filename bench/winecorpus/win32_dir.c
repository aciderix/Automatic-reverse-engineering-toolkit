#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(void){
  BOOL c=CreateDirectoryA("wd_subdir",NULL);
  DWORD attr=GetFileAttributesA("wd_subdir");
  int isdir=(attr!=INVALID_FILE_ATTRIBUTES)&&(attr&FILE_ATTRIBUTE_DIRECTORY)?1:0;
  char cwd[512]; GetCurrentDirectoryA(sizeof cwd,cwd);
  int hadcwd=(strlen(cwd)>0);
  BOOL r=RemoveDirectoryA("wd_subdir");
  DWORD attr2=GetFileAttributesA("wd_subdir");
  printf("create=%d isdir=%d hadcwd=%d remove=%d gone=%d\n",
         (int)c,isdir,hadcwd,(int)r,attr2==INVALID_FILE_ATTRIBUTES);
  return 0;
}
