#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
static int cmpw(const void*a,const void*b){return wcscmp(*(wchar_t*const*)a,*(wchar_t*const*)b);}
int main(void){
  const wchar_t*names[]={L"ww_b.dat",L"ww_a.dat",L"ww_c.txt",L"ww_d.dat"};
  for(int i=0;i<4;i++){HANDLE h=CreateFileW(names[i],GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);CloseHandle(h);}
  WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW(L"ww_*.dat",&fd);
  wchar_t*found[8]; int n=0;
  if(h!=INVALID_HANDLE_VALUE){ do{ if(n<8) found[n++]=wcsdup(fd.cFileName); }while(FindNextFileW(h,&fd)); FindClose(h); }
  qsort(found,n,sizeof(wchar_t*),cmpw);
  printf("count=%d:",n);
  for(int i=0;i<n;i++){ char nb[64]; int j=0; for(;found[i][j]&&j<63;j++)nb[j]=(char)found[i][j]; nb[j]=0; printf(" %s",nb); }
  printf("\n");
  for(int i=0;i<4;i++)DeleteFileW(names[i]);
  return 0;
}
