#include <stdio.h>
#include <stdlib.h>
int main(void){
  srand(12345);
  for(int i=0;i<6;i++) printf("%d ", rand());
  printf("\n");
  srand(1); printf("first=%d\n", rand());
  return 0;
}
