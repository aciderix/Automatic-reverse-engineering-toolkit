#include <stdio.h>
int main(void){
  printf("[%d][%5d][%-5d][%05d][%+d][% d]\n", 42, 42, 42, 42, 42, 42);
  printf("[%u][%x][%X][%o][%#x]\n", 4000000000u, 0xdeadbeef, 0xCAFE, 0755, 255);
  printf("[%c][%s][%10s][%-10s][%.3s]\n", 'Q', "hi", "pad", "pad", "truncated");
  printf("[%ld][%lu][%lld][%llu]\n", -123456L, 123456UL, -1234567890123LL, 1234567890123ULL);
  printf("[%.2f][%8.3f][%e][%g]\n", 3.14159, 2.5, 12345.678, 0.0001);
  printf("[%%][%d%%]\n", 50);
  printf("[%i][%hd][%hhu]\n", -7, (short)-3, (unsigned char)200);
  return 0;
}
