/* mlang IMultiLanguage::GetFamilyCodePage (ported from Wine's search-loop LOGIC) and a
 * GetCodePageInfo probe for the Unicode/UTF-8 code pages the extractor now includes.
 * Verifies bit-identical Wine. (GetNumberOfCodePageInfo is intentionally not tested: its
 * runtime count did not reconcile with a static source parse, so it stays unmodelled.) */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mlang.h>
#include <stdio.h>

static const GUID MY_CLSID_CMultiLanguage =
    { 0x275C23E2, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };
static const GUID MY_IID_IMultiLanguage =
    { 0x275C23E1, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };

static void fam(IMultiLanguage *ml, UINT cp) {
    UINT f = 0;
    HRESULT h = IMultiLanguage_GetFamilyCodePage(ml, cp, &f);
    printf("family(%u) h:%ld fam:%u\n", cp, (long)h, f);
}

int main(void) {
    CoInitialize(NULL);
    IMultiLanguage *ml = NULL;
    CoCreateInstance(&MY_CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
                     &MY_IID_IMultiLanguage, (void **)&ml);
    if (ml) {
        fam(ml, 28591);    /* ISO-8859-1  -> Western European (1252) */
        fam(ml, 932);      /* Shift-JIS   -> Japanese (932)          */
        fam(ml, 65001);    /* UTF-8       -> Unicode (1200)          */
        fam(ml, 999999);   /* unknown     -> S_FALSE                 */

        MIMECPINFO cpi;
        HRESULT h = IMultiLanguage_GetCodePageInfo(ml, 65001, &cpi);
        printf("utf8 h:%ld cp:%u fam:%u\n", (long)h, cpi.uiCodePage, cpi.uiFamilyCodePage);
        IMultiLanguage_Release(ml);
    }
    CoUninitialize();
    return 0;
}
