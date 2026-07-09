#include <signal.h>
#include <stdio.h>
static void h1(int s){(void)s;}
static void h2(int s){(void)s;}
int main(void){
  /* signal() returns the PREVIOUS disposition: SIG_DFL first, then the handler
   * installed by the prior call. mingw/gnulib signal-blocking relies on this. */
  void (*p1)(int)=signal(SIGINT,h1);
  void (*p2)(int)=signal(SIGINT,h2);
  void (*p3)(int)=signal(SIGTERM,h1);      /* independent slot: still SIG_DFL */
  printf("p1_dfl=%d p2_h1=%d p3_dfl=%d\n",
         (p1==SIG_DFL), (p2==h1), (p3==SIG_DFL));
  return 0;
}
