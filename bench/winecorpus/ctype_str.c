#include <stdio.h>
#include <ctype.h>
#include <string.h>
int main(void){
  const char*s="Hello, World! 42";
  int up=0,lo=0,dg=0,sp=0,pu=0;
  for(const char*p=s;*p;p++){ up+=!!isupper((unsigned char)*p); lo+=!!islower((unsigned char)*p);
    dg+=!!isdigit((unsigned char)*p); sp+=!!isspace((unsigned char)*p); pu+=!!ispunct((unsigned char)*p); }
  printf("up=%d lo=%d dg=%d sp=%d pu=%d\n",up,lo,dg,sp,pu);
  char b[32]; strcpy(b,s);
  for(char*p=b;*p;p++)*p=(char)toupper((unsigned char)*p);
  printf("upper=[%s]\n",b);
  for(char*p=b;*p;p++)*p=(char)tolower((unsigned char)*p);
  printf("lower=[%s]\n",b);
  printf("alnum=%d\n", isalnum('Z')&&isalnum('7')&&!isalnum('!'));
  return 0;
}
