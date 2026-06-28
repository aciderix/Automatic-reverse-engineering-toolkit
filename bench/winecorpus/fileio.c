#include <stdio.h>
#include <string.h>
int main(void){
  FILE*f=fopen("wd_tmp.txt","w");
  if(!f){printf("OPENW_FAIL\n");return 1;}
  fprintf(f,"line one %d\n", 11);
  fputs("line two\n", f);
  fwrite("abc\n",1,4,f);
  fclose(f);
  f=fopen("wd_tmp.txt","r");
  if(!f){printf("OPENR_FAIL\n");return 1;}
  char buf[64]; long total=0; int lines=0;
  while(fgets(buf,sizeof buf,f)){ lines++; total+=strlen(buf); }
  printf("lines=%d bytes=%ld\n", lines, total);
  fseek(f,0,SEEK_SET); int c=fgetc(f); printf("first=%c\n",c);
  fclose(f); remove("wd_tmp.txt");
  return 0;
}
