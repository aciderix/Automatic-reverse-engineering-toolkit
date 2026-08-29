/* IMultiLanguage — the mlang values ARET is about to model, measured on real Windows.
 *
 * winecorpus/ole_mlang.c is the winediff fixture; ARET currently ABORTS on it because
 * two IMultiLanguage methods were left instrument-first (aret_win32.c):
 *   - GetNumberOfCodePageInfo: a COUNT. Wine returns 73; ARET's embedded table (extracted
 *     from Wine) has 73 rows. Before ARET returns 73, this probe records what the REAL
 *     mlang.dll counts — because ARET's table is Wine-derived and Windows may count
 *     differently, and §0 forbids shipping a number we have not proven.
 *   - ConvertStringToUnicode(1252, "Az"): a deterministic conversion (expected 65,122).
 * GetCodePageInfo(1252) is already modelled from the Wine table; printed here so the
 * real-Windows description/family can be compared to what ARET emits.
 *
 * On windows-latest, CoCreateInstance(CLSID_CMultiLanguage) loads the genuine system
 * mlang.dll, so every line below is the Win32 the original binaries were built against.
 * NOT a gate (bench/winoracle/README.md): a measurement a session reads and encodes.
 *
 * Self-contained: the IID is spelled out (mingw's libuuid lacks IID_IMultiLanguage);
 * MSVC's uuid.lib has CLSID_CMultiLanguage and the SDK ships <mlang.h>. */
#include <windows.h>
#include <objbase.h>
#include <mlang.h>
#include <stdio.h>

static const GUID iid_imultilanguage =
    { 0x275c23e1, 0x3747, 0x11d0, { 0x9f, 0xea, 0x00, 0xaa, 0x00, 0x3f, 0x86, 0x46 } };

int main(void)
{
    HRESULT hr = CoInitialize(NULL);
    printf("CoInitialize hr=0x%lx\n", (unsigned long)hr);

    IMultiLanguage *ml = NULL;
    hr = CoCreateInstance(&CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
                          &iid_imultilanguage, (void **)&ml);
    printf("CoCreateInstance hr=0x%lx null=%d\n", (unsigned long)hr, ml == NULL);
    if (FAILED(hr) || !ml) return 1;

    UINT n = 0;
    hr = ml->lpVtbl->GetNumberOfCodePageInfo(ml, &n);
    printf("GetNumberOfCodePageInfo hr=0x%lx n=%u\n", (unsigned long)hr, n);

    MIMECPINFO cp;
    memset(&cp, 0, sizeof cp);
    hr = ml->lpVtbl->GetCodePageInfo(ml, 1252, &cp);
    printf("GetCodePageInfo(1252) hr=0x%lx cp=%u family=%u\n",
           (unsigned long)hr, (unsigned)cp.uiCodePage, (unsigned)cp.uiFamilyCodePage);
    if (SUCCEEDED(hr)) printf("  desc=%ls webcs=%ls\n", cp.wszDescription, cp.wszWebCharset);

    DWORD mode = 0;
    UINT srcn = 2, dstn = 8;
    WCHAR dst[8];
    memset(dst, 0, sizeof dst);
    hr = ml->lpVtbl->ConvertStringToUnicode(ml, &mode, 1252, (CHAR *)"Az", &srcn, dst, &dstn);
    printf("ConvertStringToUnicode hr=0x%lx srcn=%u dstn=%u [%u %u]\n",
           (unsigned long)hr, srcn, dstn, (unsigned)dst[0], (unsigned)dst[1]);

    printf("Release=%lu\n", (unsigned long)ml->lpVtbl->Release(ml));
    CoUninitialize();
    return 0;
}
