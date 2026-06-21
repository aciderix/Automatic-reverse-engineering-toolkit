#include <stdio.h>
#include <stdlib.h>
static int cmp(const void*a,const void*b){ return (*(const int*)a)-(*(const int*)b); }
int main(){ int v[]={5,2,9,1,7,3}; int n=6; qsort(v,n,sizeof(int),cmp);
  for(int i=0;i<n;i++) printf("%d%s", v[i], i+1<n?",":"\n"); return 0; }
