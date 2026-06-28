#include <windows.h>
#include <stdio.h>
#include <string.h>
int main(void){
  HANDLE h=CreateFileA("wf.bin",GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  char data[1000]; for(int i=0;i<1000;i++)data[i]=(char)(i&0xff);
  DWORD wr; WriteFile(h,data,1000,&wr,NULL);
  DWORD pos=SetFilePointer(h,500,NULL,FILE_BEGIN);
  char b; DWORD rd; ReadFile(h,&b,1,&rd,NULL);
  DWORD endpos=SetFilePointer(h,0,NULL,FILE_END);
  LARGE_INTEGER sz; int gs=GetFileSizeEx(h,&sz);
  LARGE_INTEGER d; d.QuadPart=100; LARGE_INTEGER np; int sx=SetFilePointerEx(h,d,&np,FILE_BEGIN);
  int fl=FlushFileBuffers(h);
  CloseHandle(h); DeleteFileA("wf.bin");
  printf("pos=%lu byte500=%d endpos=%lu gs=%d size=%lld sx=%d np=%lld flush=%d\n",
    (unsigned long)pos,(int)(unsigned char)b,(unsigned long)endpos,gs,(long long)sz.QuadPart,sx,(long long)np.QuadPart,fl);
  char full[512],*fp=NULL; DWORD n=GetFullPathNameA("sub\\foo.txt",sizeof full,full,&fp);
  printf("fullpath_ok=%d filepart=[%s]\n", (int)(n>=7 && strcmp(full+n-7,"foo.txt")==0), fp?fp:"(null)");
  return 0;
}
