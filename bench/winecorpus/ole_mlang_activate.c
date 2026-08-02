/* mlang COM activation: CoCreateInstance(CLSID_CMultiLanguage, IID_IMultiLanguage) and
 * the IUnknown contract. WinMerge/MFC do exactly this at startup. The 15 interface
 * methods are instrument-first stubs (they name themselves + abort), so this fixture
 * exercises only the ACTIVATION + IUnknown, which is what must be bit-identical Wine.
 * Exact refcounts are intentionally NOT printed (the HLE object is a stateless singleton
 * whereas a real CMultiLanguage destroys at 0 — not an observable-soundness property). */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <mlang.h>
#include <stdio.h>

/* Self-contained GUID constants (mlang's are not in mingw's libuuid), distinct names to
 * avoid clashing with any the headers already declare. */
static const GUID MY_CLSID_CMultiLanguage =
    { 0x275C23E2, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };
static const GUID MY_IID_IMultiLanguage =
    { 0x275C23E1, 0x3747, 0x11D0, { 0x9F, 0xEA, 0x00, 0xAA, 0x00, 0x3F, 0x86, 0x46 } };

int main(void) {
    CoInitialize(NULL);
    IMultiLanguage *ml = NULL;
    HRESULT hr = CoCreateInstance(&MY_CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
                                  &MY_IID_IMultiLanguage, (void **)&ml);
    printf("cocreate hr:%ld nonnull:%d\n", (long)hr, ml != NULL);
    if (ml) {
        IUnknown *unk = NULL;
        HRESULT q = IMultiLanguage_QueryInterface(ml, &IID_IUnknown, (void **)&unk);
        printf("qi_unknown hr:%ld nonnull:%d\n", (long)q, unk != NULL);
        if (unk) IUnknown_Release(unk);

        void *disp = NULL;
        HRESULT q2 = IMultiLanguage_QueryInterface(ml, &IID_IDispatch, (void **)&disp);
        printf("qi_foreign nointerface:%d null:%d\n", q2 == E_NOINTERFACE, disp == NULL);

        IMultiLanguage_Release(ml);
    }
    CoUninitialize();
    return 0;
}
