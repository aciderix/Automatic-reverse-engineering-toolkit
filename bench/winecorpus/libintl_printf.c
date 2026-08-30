/* gettext's libintl printf family (the measured post-GLib wall: libintl_vfprintf 30,
 * libintl_snprintf 24, libintl_vasprintf/vsnprintf 22). ARET lifts libintl-8.dll, so the
 * REAL gettext code runs — including its raison d'etre, POSITIONAL args (%n$) that a
 * translated program relies on, which ARET's own printf (aret_vformat) does not model.
 * Calls the libintl_* wrappers directly (what a gettext-redirected program imports).
 * Deterministic output; the Wine oracle loads the same native libintl (.wineoverride). */
#include <stdio.h>
extern int libintl_snprintf(char*, size_t, const char*, ...);
extern int libintl_vfprintf(FILE*, const char*, va_list);
extern int libintl_vsnprintf(char*, size_t, const char*, va_list);
extern int libintl_vasprintf(char**, const char*, va_list);
#include <stdarg.h>
static void via_vsn(char*b,size_t n,const char*f,...){va_list a;va_start(a,f);libintl_vsnprintf(b,n,f,a);va_end(a);}
static void via_vas(const char*f,...){va_list a;va_start(a,f);char*p=0;int r=libintl_vasprintf(&p,f,a);va_end(a);printf("vas r=%d s=[%s]\n",r,p?p:"(nil)");}
static void via_vf(const char*f,...){va_list a;va_start(a,f);libintl_vfprintf(stdout,f,a);va_end(a);}
int main(void){
  char b[128];
  int n=libintl_snprintf(b,sizeof b,"plain %s=%d",  "x", 42);
  printf("snprintf n=%d [%s]\n",n,b);
  /* POSITIONAL: swap order — gettext's whole point */
  int p=libintl_snprintf(b,sizeof b,"pos %2$s before %1$s","AAA","BBB");
  printf("positional n=%d [%s]\n",p,b);
  via_vsn(b,sizeof b,"vsn %s/%d","y",7); printf("vsnprintf [%s]\n",b);
  via_vas("vasprintf %d-%s",9,"z");
  via_vf("vfprintf %d/%2$s/%1$s\n", 3, "R", "L");   /* vfprintf + positional to stdout */
  fflush(stdout);
  printf("done\n");return 0;
}
