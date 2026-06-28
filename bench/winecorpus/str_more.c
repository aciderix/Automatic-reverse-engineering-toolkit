#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void){
  char s[64]="a,bb,ccc,dddd"; char*tok=strtok(s,","); int n=0; long tl=0;
  while(tok){ n++; tl+=strlen(tok); tok=strtok(NULL,","); }
  printf("tok n=%d totlen=%ld\n",n,tl);
  printf("spn=%zu cspn=%zu pbrk=%ld\n", strspn("12345abc","0123456789"),
         strcspn("abc123","0123456789"), (long)(strpbrk("hello world","wxyz")-(char*)"hello world"));
  char*m=memchr("abcdef",'d',6); printf("memchr=%ld\n", m?(long)(m-(char*)"abcdef"):-1);
  char nb[16]="AB"; strncat(nb,"CDEFGH",3); printf("strncat=[%s]\n",nb);
  char*d=strdup("duplicated"); printf("strdup=[%s] len=%zu\n",d,strlen(d)); free(d);
  return 0;
}
