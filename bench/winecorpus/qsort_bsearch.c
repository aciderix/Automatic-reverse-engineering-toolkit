#include <stdio.h>
#include <stdlib.h>
static int cmp(const void*a,const void*b){return *(const int*)a-*(const int*)b;}
int main(void){
  int v[]={5,2,9,1,7,3,8,4,6,0};
  qsort(v,10,sizeof(int),cmp);
  for(int i=0;i<10;i++)printf("%d",v[i]); printf("\n");
  int key=7; int*f=bsearch(&key,v,10,sizeof(int),cmp);
  printf("found=%ld\n", f?(long)(f-v):-1L);
  key=11; f=bsearch(&key,v,10,sizeof(int),cmp);
  printf("notfound=%ld\n", f?(long)(f-v):-1L);
  return 0;
}
