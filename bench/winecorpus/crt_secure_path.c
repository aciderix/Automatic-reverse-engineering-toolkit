/* MSVC secure-CRT + shlwapi path + clipboard-format shims (data-driven: the imports
 * WinMerge/MFC90 hits at startup). Oracle: Wine. All-ASCII.
 *  - memcpy_s (C11 Annex K / MSVC errno_t): 0 ok; ERANGE=34 on overflow (dest zeroed);
 *    EINVAL=22 on null src (dest zeroed); 0 on count==0.
 *  - PathFindExtensionA/W (Wine shlwapi verbatim): last '.', reset by any '\\' or ' ';
 *    the terminating NUL (empty) when there is no extension.
 *  - RegisterClipboardFormatA/W: a global-atom id — stable per name, unique per name,
 *    in [0xC000,0xFFFF]. The value itself is process-local, so assert the CONTRACT
 *    (same/diff/range), not the number. */
#include <windows.h>
#include <shlwapi.h>
#include <string.h>
#include <stdio.h>
static void dumpw(const char *tag, const wchar_t *w){ printf("%s=", tag); for(;*w;w++) putchar((char)*w); putchar('\n'); }
int main(void){
    char b[8], c[3]; int e;
    e = memcpy_s(b, sizeof b, "hi", 3);        printf("mcs_ok e=%d s=%s\n", e, b);
    memset(c, 0x55, 3);
    e = memcpy_s(c, sizeof c, "toolong", 8);   printf("mcs_range e=%d z=%d\n", e, c[0]==0);
    e = memcpy_s(b, sizeof b, NULL, 4);         printf("mcs_null e=%d\n", e);
    e = memcpy_s(b, sizeof b, "x", 0);          printf("mcs_zero e=%d\n", e);
    printf("extA1=%s\n",   PathFindExtensionA("a.b.c"));
    printf("extA2=[%s]\n", PathFindExtensionA("noext"));
    printf("extA3=%s\n",   PathFindExtensionA("dir.x\\file.txt"));
    printf("extA4=%s\n",   PathFindExtensionA("a b.c"));
    dumpw("extW", PathFindExtensionW(L"foo.tar.gz"));
    UINT f1 = RegisterClipboardFormatA("MyFmt");
    UINT f2 = RegisterClipboardFormatA("MyFmt");
    UINT f3 = RegisterClipboardFormatA("Other");
    printf("cf same=%d diff=%d range=%d\n", f1==f2, f1!=f3, f1>=0xC000u && f1<=0xFFFFu);
    printf("done\n");
    return 0;
}
