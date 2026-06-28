#include <stdio.h>
int main(void){
  FILE*f=fopen("wd_bin.tmp","wb");
  int arr[5]={10,20,30,40,50}; fwrite(arr,sizeof(int),5,f);
  fputc('X',f); fclose(f);
  f=fopen("wd_bin.tmp","rb");
  int got[5]; size_t r=fread(got,sizeof(int),5,f);
  long sum=0; for(int i=0;i<5;i++)sum+=got[i];
  long pos=ftell(f); fseek(f,0,SEEK_SET); rewind(f);
  int c=fgetc(f); ungetc(c,f); int c2=fgetc(f);
  fseek(f,20,SEEK_SET); int last=fgetc(f);
  printf("read=%zu sum=%ld pos=%ld c=%d c2=%d last=%c\n",r,sum,pos,c,c2,last);
  fclose(f); remove("wd_bin.tmp");
  puts("puts_line");
  return 0;
}
