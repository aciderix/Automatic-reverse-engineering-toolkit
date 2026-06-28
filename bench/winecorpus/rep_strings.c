#include <stdio.h>
#include <string.h>
int main(void){
  char a[256],b[256];
  for(int i=0;i<256;i++){a[i]=(char)(i&0x7f); b[i]=(char)(i&0x7f);}
  b[200]=0x55;
  int e=memcmp(a,a,256); int d=memcmp(a,b,256);
  printf("eq=%d ne=%d\n", e, d<0?-1:(d>0?1:0));
  void*p=memchr(a,0x42,256); printf("memchr=%ld\n", p?(long)((char*)p-a):-1);
  char s[32]="abcXabcXabc"; printf("strrchr=%ld memrchr_sim=%d\n",(long)(strrchr(s,'X')-s),
         (int)(strlen(s)));
  printf("strncmp=%d strcmp=%d\n", strncmp("hello","help",3), strcmp("abc","abd")<0?-1:1);
  return 0;
}
