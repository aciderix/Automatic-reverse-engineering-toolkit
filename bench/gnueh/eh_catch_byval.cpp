#include <cstdio>
struct E { int x; E(int v):x(v){printf("E ctor %d\n",x);} E(const E&o):x(o.x){printf("E copy %d\n",x);} ~E(){printf("E dtor %d\n",x);} };
int main(){ printf("start\n");
  try { throw E(9); } catch(E e) { printf("caught %d\n", e.x); }
  printf("done\n"); return 0; }
