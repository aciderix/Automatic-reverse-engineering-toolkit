/* CoCreateInstance of a REAL in-proc COM class, served by a LIFTED DLL.
 *
 * This is the fixture behind the MLang wall in WinMerge: `CoCreateInstance(
 * CLSID_CMultiLanguage, ..., IID_IMultiLanguage, ...)`. Until now ARET aborted on
 * it, and correctly so — answering REGDB_E_CLASSNOTREG would have been a DEFINED
 * failure but not a sound one, because under a real system the class IS registered
 * and the program takes a completely different path.
 *
 * What is being pinned down here is not "does mlang work" but the COM ACTIVATION
 * PATH itself: the class object comes from the lifted `mlang.dll`
 * (`--with-dll mlang.dll=...`), so
 *
 *     CoCreateInstance -> [lifted] DllGetClassObject -> [lifted] IClassFactory
 *                      -> [lifted] CreateInstance -> [lifted] IMultiLanguage
 *
 * is four calls INTO transpiled code, two of them through a vtable that lives in
 * the lifted module's own rebased data. Wine (the oracle) loads the very same
 * builtin PE and runs the very same code natively, so any difference is ours.
 *
 * The methods called afterwards are deliberately boring and deterministic (a
 * count, a codepage description, a well-known conversion) — the point is the
 * activation, and a flashy method would only add its own divergences. */
#include <windows.h>
#include <objbase.h>
#include <mlang.h>
#include <stdio.h>

/* mingw's libuuid ships CLSID_CMultiLanguage but NOT IID_IMultiLanguage, so the IID
 * is spelled out here. It is the one WinMerge asks for, read off the ARET abort:
 * {275C23E1-3747-11D0-9FEA-00AA003F8646}. */
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

    /* The object is real COM: QueryInterface back to IUnknown, and the refcount
     * must move. Pointers are never printed — only the RELATIONS between them. */
    IUnknown *unk = NULL;
    hr = ml->lpVtbl->QueryInterface(ml, &IID_IUnknown, (void **)&unk);
    printf("QI IUnknown hr=0x%lx same=%d\n", (unsigned long)hr, (void *)unk == (void *)ml);
    if (unk) printf("Release(unk)=%lu\n", (unsigned long)unk->lpVtbl->Release(unk));

    UINT n = 0;
    hr = ml->lpVtbl->GetNumberOfCodePageInfo(ml, &n);
    printf("GetNumberOfCodePageInfo hr=0x%lx n=%u\n", (unsigned long)hr, n);

    /* A specific, stable codepage: 1252 (Windows Latin-1). */
    MIMECPINFO cp;
    memset(&cp, 0, sizeof cp);
    hr = ml->lpVtbl->GetCodePageInfo(ml, 1252, &cp);
    printf("GetCodePageInfo(1252) hr=0x%lx cp=%u family=%u\n",
           (unsigned long)hr, (unsigned)cp.uiCodePage, (unsigned)cp.uiFamilyCodePage);
    if (SUCCEEDED(hr)) printf("  desc=%ls webcs=%ls\n", cp.wszDescription, cp.wszWebCharset);

    /* And one real conversion through the object: "Az" in CP1252 -> UTF-16. */
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
