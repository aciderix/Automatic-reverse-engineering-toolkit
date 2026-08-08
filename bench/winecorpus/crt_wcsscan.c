/* Wide scanning trio wcspbrk/wcsspn/wcscspn (16-bit code units, msvcrt). Fills a
 * measured gap in the wide-string family (driver: WinMerge/MFC calls wcspbrk).
 * Standard C semantics; oracle = Wine. All-ASCII. Grid covers hit/miss, the
 * empty-set edge, and set membership at the boundary. */
#include <wchar.h>
#include <stdio.h>
static int idx(const wchar_t *s, const wchar_t *p) { return p ? (int)(p - s) : -1; }
int main(void) {
    const wchar_t *s = L"hello world";
    printf("pbrk_ol=%d\n",   idx(s, wcspbrk(s, L"ol")));   /* first of {o,l} in "hello world" */
    printf("pbrk_wz=%d\n",   idx(s, wcspbrk(s, L"wz")));   /* 'w' */
    printf("pbrk_none=%d\n", idx(s, wcspbrk(s, L"XYZ")));  /* none -> NULL -> -1 */
    printf("pbrk_empty=%d\n",idx(s, wcspbrk(s, L"")));     /* empty accept -> NULL -> -1 */
    printf("spn_abc=%d\n",   (int)wcsspn(L"aabbcXYZ", L"abc"));  /* 5 */
    printf("spn_all=%d\n",   (int)wcsspn(L"abc", L"abcdef"));    /* 3 (whole string) */
    printf("spn_none=%d\n",  (int)wcsspn(L"xabc", L"abc"));      /* 0 (first not in set) */
    printf("spn_empty=%d\n", (int)wcsspn(L"abc", L""));          /* 0 */
    printf("cspn_XYZ=%d\n",  (int)wcscspn(L"abcXdef", L"XYZ"));  /* 3 */
    printf("cspn_none=%d\n", (int)wcscspn(L"abcdef", L"XYZ"));   /* 6 (no reject char) */
    printf("cspn_first=%d\n",(int)wcscspn(L"Xabc", L"XYZ"));     /* 0 */
    printf("cspn_empty=%d\n",(int)wcscspn(L"abc", L""));         /* 3 */
    return 0;
}
