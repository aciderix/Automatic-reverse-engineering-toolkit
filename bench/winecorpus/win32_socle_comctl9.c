/* comctl32 socle batch 9 (final): the last socle imports. Only GetDateFormatW (with an
 * explicit NUMERIC picture) and LocalSize have a deterministic standalone oracle, so this
 * fixture exercises those. The rest of batch 9 has no standalone cross-engine oracle and
 * is verified only in situ (through a lifted comctl32) or qualitatively:
 *   - Uniscribe Script* -> sound FAILURE so a lifted comctl32 uses its GDI text path;
 *   - floor / __stdio_common_vsprintf -> CRT, exercised inside lifted comctl32;
 *   - Polygon / PolyPolyline -> on-screen paint (no headless capture);
 *   - GetLocaleInfoW -> en-US best-effort (locale-resolution caveat, like gdi_uifont).
 * GetDateFormatW numeric fields (yyyy/yy/MM/M/dd/d + literals) are bit-identical to Wine. */
#include <windows.h>
#include <stdio.h>
int main(void){
    SYSTEMTIME st; memset(&st,0,sizeof st);
    st.wYear=2024; st.wMonth=3; st.wDay=7; st.wDayOfWeek=4;
    WCHAR buf[128];
    int n=GetDateFormatW(LOCALE_INVARIANT,0,&st,L"yyyy-MM-dd",buf,128);
    printf("fmt1 n=%d [", n); for(int i=0;buf[i];i++) putchar((char)buf[i]); printf("]\n");
    n=GetDateFormatW(LOCALE_INVARIANT,0,&st,L"yy/M/d",buf,128);
    printf("fmt2 n=%d [", n); for(int i=0;buf[i];i++) putchar((char)buf[i]); printf("]\n");
    n=GetDateFormatW(LOCALE_INVARIANT,0,&st,L"d.M.yyyy",buf,128);
    printf("fmt3 n=%d [", n); for(int i=0;buf[i];i++) putchar((char)buf[i]); printf("]\n");
    HLOCAL h=LocalAlloc(LPTR,100); SIZE_T sz=LocalSize(h);
    printf("localsize_ge100=%d\n", sz>=100);
    printf("done\n");return 0;
}
