/* swscanf — wide (16-bit) scanf. Under mingw the binary drives its own static wide
 * scanf, which classifies via iswctype(c, desc) (implemented here, ASCII-exact). MSVC
 * binaries reach aret_swscanf directly (mirrors the verified sscanf). Oracle: Wine. */
#include <stdio.h>
#include <wchar.h>
int main(void){
    int a,b,r; long long ll; double d; float f; wchar_t ws[32]; char ns[32]; wchar_t wc;
    r=swscanf(L"12 0x2A 3.5", L"%d %x %lf", &a,&b,&d); printf("r=%d %d %d %.1f\n", r,a,b,d);
    r=swscanf(L"  -7  42", L"%d %d", &a,&b); printf("r=%d %d %d\n", r,a,b);
    r=swscanf(L"100200300400", L"%lld", &ll); printf("r=%d %lld\n", r,ll);
    r=swscanf(L"2.5e3", L"%f", &f); printf("r=%d %.1f\n", r,f);
    r=swscanf(L"hello world", L"%ls", ws); printf("r=%d [%ls]\n", r,ws);
    r=swscanf(L"narrow99", L"%hs", ns); printf("r=%d [%s]\n", r,ns);
    r=swscanf(L"Q", L"%lc", &wc); printf("r=%d [%lc]\n", r,wc);
    r=swscanf(L"abcdef", L"%3ls", ws); printf("r=%d [%ls]\n", r,ws);
    r=swscanf(L"", L"%d", &a); printf("r_eof=%d\n", r);
    r=swscanf(L"nope", L"%d", &a); printf("r_fail=%d\n", r);
    printf("done\n");
    return 0;
}
