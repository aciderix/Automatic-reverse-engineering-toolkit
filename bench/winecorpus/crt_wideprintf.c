/* Wide (16-bit) printf: %ls/%S/%lc/%C in the narrow formatter, and the wide
 * formatters wsprintfW / _snwprintf / _vsnwprintf. (swprintf is CRT-signature
 * ambiguous -> sound abort, not modelled.) Oracle: Wine. ASCII. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>
static void via_vsnw(wchar_t *b, size_t n, const wchar_t *fmt, ...) {
    va_list ap; va_start(ap, fmt); _vsnwprintf(b, n, fmt, ap); va_end(ap);
}
int main(void){
    wchar_t w[] = L"Wide99";
    printf("ls=[%ls] S=[%S] w10=[%10ls] p4=[%.4ls]\n", w, w, w, w);
    printf("lc=[%lc] C=[%C]\n", (wint_t)L'Z', (wint_t)L'Q');
    wchar_t b[128];
    wsprintfW(b, L"n=%d hex=%08x s=%s c=%c pct=%%", 42, 255, L"in", L'Z');
    printf("wsprintfW=[%ls] ret=%d\n", b, lstrlenW(b));
    int r = _snwprintf(b, 6, L"%s", L"HELLOWORLD"); b[6]=0;
    printf("snw_trunc ret=%d buf=[%ls]\n", r, b);
    int r2 = _snwprintf(b, 32, L"%05d-%s", 9, L"ok");
    printf("snw_ok ret=%d buf=[%ls]\n", r2, b);
    via_vsnw(b, 32, L"vs=%d:%s", 7, L"end");
    printf("vsnw=[%ls]\n", b);
    printf("done\n");
    return 0;
}
