#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
  int *a=malloc(10*sizeof(int));
  for(int i=0;i<10;i++)a[i]=i*i;
  a=realloc(a,20*sizeof(int));
  long s=0; for(int i=0;i<10;i++)s+=a[i];
  char *c=calloc(8,1); int z=0; for(int i=0;i<8;i++)z+=c[i];
  printf("sum=%ld calloc_zero=%d\n", s, z);
  free(a); free(c);
  // linked list
  struct N{int v;struct N*next;}*h=NULL;
  for(int i=1;i<=5;i++){struct N*n=malloc(sizeof*n);n->v=i*10;n->next=h;h=n;}
  long t=0; for(struct N*n=h;n;n=n->next)t+=n->v;
  printf("list=%ld\n", t);
  return 0;
}
