#ifndef __WINE_COMPAT_DEBUG_H
#define __WINE_COMPAT_DEBUG_H
#define WINE_DEFAULT_DEBUG_CHANNEL(x)
#define WINE_DECLARE_DEBUG_CHANNEL(x)
#define TRACE(...)   do {} while (0)
#define WARN(...)    do {} while (0)
#define FIXME(...)   do {} while (0)
#define ERR(...)     do {} while (0)
#define TRACE_ON(x)  0
#define WARN_ON(x)   0
static inline const char *wine_dbgstr_an(const char *s, int n){(void)n;return s?s:"";}
static inline const char *wine_dbgstr_wn(const void *s, int n){(void)s;(void)n;return "";}
#define debugstr_a(s)   wine_dbgstr_an((s), -1)
#define debugstr_w(s)   wine_dbgstr_wn((s), -1)
#define debugstr_us(s)  ""
#define debugstr_u(s)   ""
#define debugstr_an     wine_dbgstr_an
#define debugstr_wn     wine_dbgstr_wn
#endif
