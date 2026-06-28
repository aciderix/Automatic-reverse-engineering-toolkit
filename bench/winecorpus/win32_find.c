#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int cmpstr(const void*a,const void*b){return strcmp(*(char*const*)a,*(char*const*)b);}
int main(void){
  const char*names[]={"wf_b.txt","wf_a.txt","wf_c.dat","wf_d.txt"};
  for(int i=0;i<4;i++){FILE*f=fopen(names[i],"w");fputs("x",f);fclose(f);}
  WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA("wf_*.txt",&fd);
  char*found[16]; int n=0;
  if(h!=INVALID_HANDLE_VALUE){
    do{ if(n<16) found[n++]=strdup(fd.cFileName); }while(FindNextFileA(h,&fd));
    FindClose(h);
  }
  qsort(found,n,sizeof(char*),cmpstr);
  printf("count=%d:",n); for(int i=0;i<n;i++)printf(" %s",found[i]); printf("\n");
  for(int i=0;i<4;i++)DeleteFileA(names[i]);
  return 0;
}
