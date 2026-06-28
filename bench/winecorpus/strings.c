#include <stdio.h>
#include <string.h>
int main(void){
  char b[64]; strcpy(b,"Hello"); strcat(b,", World");
  printf("len=%zu cmp=%d ncmp=%d\n", strlen(b), strcmp(b,"Hello, World"), strncmp(b,"Hello",5));
  char *p=strchr(b,'W'); char *q=strrchr(b,'l'); char *r=strstr(b,"World");
  printf("chr=%ld rchr=%ld str=%ld\n", (long)(p-b),(long)(q-b),(long)(r-b));
  char d[16]; memset(d,'*',8); d[8]=0; memcpy(d+2,"XY",2);
  printf("mem=[%s] memcmp=%d\n", d, memcmp("abc","abd",3));
  char s[64]; sprintf(s,"%d-%s-%x",7,"z",0xaa); printf("sprintf=[%s]\n",s);
  int n=snprintf(s,5,"%d",123456); printf("snprintf n=%d s=[%s]\n",n,s);
  char mv[16]="0123456789"; memmove(mv+2,mv,5); printf("memmove=[%s]\n",mv);
  return 0;
}
