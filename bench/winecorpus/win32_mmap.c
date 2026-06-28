#include <windows.h>
#include <stdio.h>
int main(void){
  /* write a known file via the CRT, then memory-map it read-only and checksum */
  FILE*f=fopen("wd_map.bin","wb");
  for(int i=0;i<1000;i++) fputc((i*7+3)&0xff,f);
  fclose(f);
  HANDLE h=CreateFileA("wd_map.bin",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  HANDLE m=CreateFileMappingA(h,NULL,PAGE_READONLY,0,0,NULL);
  unsigned char*p=(unsigned char*)MapViewOfFile(m,FILE_MAP_READ,0,0,0);
  long sum=0; int ok=(p!=NULL);
  if(ok) for(int i=0;i<1000;i++) sum+=p[i];
  int b500 = ok ? p[500] : -1;
  UnmapViewOfFile(p); CloseHandle(m); CloseHandle(h);
  printf("ro_mapped=%d checksum=%ld byte500=%d\n", ok, sum, b500);

  /* read-write mapping: create+size a file, map RW, fill, unmap, read back */
  HANDLE h2=CreateFileA("wd_map2.bin",GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  HANDLE m2=CreateFileMappingA(h2,NULL,PAGE_READWRITE,0,64,NULL);
  unsigned char*q=(unsigned char*)MapViewOfFile(m2,FILE_MAP_WRITE,0,0,0);
  int ok2=(q!=NULL);
  if(ok2) for(int i=0;i<64;i++) q[i]=(unsigned char)(255-i);
  FlushViewOfFile(q,64); UnmapViewOfFile(q); CloseHandle(m2); CloseHandle(h2);
  FILE*g=fopen("wd_map2.bin","rb"); int first=fgetc(g),atforty=0;
  fseek(g,40,SEEK_SET); atforty=fgetc(g); fclose(g);
  printf("rw_mapped=%d first=%d at40=%d\n", ok2, first, atforty);
  DeleteFileA("wd_map.bin"); DeleteFileA("wd_map2.bin");
  return 0;
}
