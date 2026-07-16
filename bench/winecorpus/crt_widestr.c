/* Wide-string CRT (16-bit code units): ordinal ops + case-insensitive (_wcsicmp/
 * _wcsnicmp = ASCII fold, exact for the C locale) + kernel32 lstrlenW/cpyW/catW.
 * Oracle: Wine. All-ASCII. (lstrcmpW/iW are linguistic in Windows -> sound abort,
 * not modelled; %ls printf is a separate gap, avoided here via a byte dump.) */
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
static int sgn(int x){ return x<0?-1:x>0?1:0; }
static void dump(const char *tag, const wchar_t *w){ printf("%s=", tag); for(;*w;w++) putchar((char)*w); putchar('\n'); }
int main(void){
    wchar_t c[64], d[64];
    printf("wcslen=%d\n", (int)wcslen(L"Hello"));
    printf("wcscmp=%d %d\n", sgn(wcscmp(L"Hello",L"hello")), sgn(wcscmp(L"abc",L"abc")));
    printf("wcsncmp=%d %d\n", sgn(wcsncmp(L"abcXY",L"abcQR",3)), sgn(wcsncmp(L"abX",L"abY",3)));
    printf("wcsicmp=%d %d %d\n", sgn(_wcsicmp(L"Hello",L"hello")), sgn(_wcsicmp(L"abc",L"abd")), sgn(_wcsicmp(L"ZED",L"abc")));
    printf("wcsnicmp=%d %d\n", sgn(_wcsnicmp(L"FOObar",L"fooBAZ",3)), sgn(_wcsnicmp(L"FOOb",L"fooc",4)));
    wchar_t *p = wcschr(L"Hello",L'l'); printf("wcschr=%d\n", p?(int)(p-L"Hello"):-1);
    wchar_t *q = wcsrchr(L"Hello",L'l'); printf("wcsrchr=%d\n", q?(int)(q-L"Hello"):-1);
    wchar_t *r = wcsstr(L"abcdefg", L"cde"); printf("wcsstr=%d\n", r?1:0);
    printf("towlower=%d towupper=%d\n", (int)towlower(L'Q'), (int)towupper(L'q'));
    wcsncpy(c, L"WORLD", 3); c[3]=0; dump("wcsncpy", c);
    printf("lstrlenW=%d\n", lstrlenW(L"Hello"));
    lstrcpyW(d,L"foo"); lstrcatW(d,L"bar"); dump("lstrcat", d);
    printf("done\n");
    return 0;
}
