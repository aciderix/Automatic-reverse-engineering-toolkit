/* mlang IMultiLanguage::GetCodePageInfo, filled from the Wine-extracted code-page table
 * (tools/gen_mlang_cp.py). This is the first mlang method WinMerge calls. Verifies the
 * whole MIMECPINFO is bit-identical Wine (flags, family cp, GDI charset, description, MIME
 * charset names, fonts) for cp 1252, and that an unknown cp is a defined S_FALSE. */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mlang.h>
#include <stdio.h>

static const GUID MY_CLSID_CMultiLanguage =
    { 0x275C23E2, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };
static const GUID MY_IID_IMultiLanguage =
    { 0x275C23E1, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };

static void pw(const WCHAR *w) { for (; *w; w++) putchar((char)*w); }

int main(void) {
    CoInitialize(NULL);
    IMultiLanguage *ml = NULL;
    CoCreateInstance(&MY_CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
                     &MY_IID_IMultiLanguage, (void **)&ml);
    if (ml) {
        MIMECPINFO cpi;
        HRESULT hr = IMultiLanguage_GetCodePageInfo(ml, 1252, &cpi);
        printf("hr:%ld cp:%u fam:%u flags:0x%lx gdi:%d\n", (long)hr, cpi.uiCodePage,
               cpi.uiFamilyCodePage, (unsigned long)cpi.dwFlags, cpi.bGDICharset);
        printf("desc:["); pw(cpi.wszDescription); printf("]\n");
        printf("web:[");  pw(cpi.wszWebCharset);  printf("] body:["); pw(cpi.wszBodyCharset); printf("]\n");
        printf("fixed:["); pw(cpi.wszFixedWidthFont); printf("] prop:["); pw(cpi.wszProportionalFont); printf("]\n");

        HRESULT hr2 = IMultiLanguage_GetCodePageInfo(ml, 999999, &cpi);
        printf("unknown hr:%ld\n", (long)hr2);
        IMultiLanguage_Release(ml);
    }
    CoUninitialize();
    return 0;
}
