/* Small general shims (data-driven from missing imports): MulDiv (measured
 * rounding), GetUserDefaultLangID/GetSystemDefaultLangID (en-US 0x0409), wide
 * numeric parse wcstoul/wcstol. Oracle: Wine. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
int main(void){
    printf("md=%d %d %d %d %d\n", MulDiv(10,3,4), MulDiv(10,3,7), MulDiv(-10,3,4),
           MulDiv(100,100,3), MulDiv(7,3,0));
    printf("lang=%04x %04x\n", GetUserDefaultLangID(), GetSystemDefaultLangID());
    wchar_t *e; unsigned long u = wcstoul(L"  0xFF hi", &e, 16);
    printf("wcstoul=%lu rest=[%ls]\n", u, e);
    long v = wcstol(L"-42abc", &e, 10);
    printf("wcstol=%ld rest=[%ls]\n", v, e);
    printf("wcstoul10=%lu\n", wcstoul(L"4294967295", NULL, 10));
    printf("done\n");
    return 0;
}
