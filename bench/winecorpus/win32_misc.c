#include <windows.h>
#include <stdio.h>
int main(void){
  HANDLE h=GetProcessHeap();
  char*p=HeapAlloc(h,HEAP_ZERO_MEMORY,32);
  lstrcpyA(p,"Win"); lstrcatA(p,"32");
  int len=lstrlenA(p); int cmp=lstrcmpA(p,"Win32");
  char buf[64]; wsprintfA(buf,"[%s len=%d cmp=%d num=%d]",p,len,cmp,255);
  printf("%s\n",buf);
  HeapFree(h,0,p);
  void*g=GlobalAlloc(GPTR,16); lstrcpyA(g,"global");
  printf("global=[%s]\n",(char*)g); GlobalFree(g);
  printf("lstrcmpi=%d\n", lstrcmpiA("ABC","abc"));
  return 0;
}
