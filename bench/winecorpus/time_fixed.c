#include <stdio.h>
#include <time.h>
int main(void){
  time_t t=1000000000; /* 2001-09-09 01:46:40 UTC */
  struct tm*g=gmtime(&t);
  char buf[64]; strftime(buf,sizeof buf,"%Y-%m-%d %H:%M:%S",g);
  printf("gmtime=[%s] wday=%d yday=%d\n", buf, g->tm_wday, g->tm_yday);
  struct tm gc=*g; time_t back=mktime(&gc); /* TZ=UTC so mktime inverts gmtime */
  printf("asc=[%.24s] difftime=%.0f\n", asctime(g), difftime(2000,500));
  return 0;
}
