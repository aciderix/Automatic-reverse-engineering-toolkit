/* CoGetMalloc / IMalloc (ole32) — the first COM INTERFACE in the HLE.
 *
 * Everything COM-shaped so far has been a flat function (CoInitialize,
 * CoTaskMemAlloc). This is the first call that hands the program a VTABLE it will
 * then call through, so what is being pinned down here is not one return value but
 * nine methods and the object's identity rules.
 *
 * Measured rather than assumed, because the interesting answers are not the obvious
 * ones and a wrong one here is a wrong pointer the program then uses:
 *   - is the task allocator a SINGLETON (does a second CoGetMalloc return the same
 *     pointer)? and does AddRef/Release actually count, or is it fixed?
 *   - does QueryInterface for IUnknown return the SAME pointer as for IMalloc?
 *   - GetSize of a block, of NULL, and after a Realloc that moved it
 *   - DidAlloc: 1 / 0 / -1 ("don't know") — which does it really answer, and for a
 *     pointer it did NOT allocate?
 *   - is memory from IMalloc::Alloc freeable by CoTaskMemFree and vice versa (i.e.
 *     are they one allocator or two)?
 *
 * Pointers are never printed: they are addresses. What is printed is the RELATIONS
 * between them (same/different) and the sizes, which is the contract.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

int main(void)
{
    HRESULT hr = CoInitialize(NULL);
    printf("CoInitialize hr=0x%lx\n", (unsigned long)hr);

    IMalloc *m = NULL, *m2 = NULL;
    hr = CoGetMalloc(1, &m);
    printf("CoGetMalloc hr=0x%lx null=%d\n", (unsigned long)hr, m == NULL);
    if (!m) return 1;

    hr = CoGetMalloc(1, &m2);
    printf("second call hr=0x%lx same=%d\n", (unsigned long)hr, m == m2);

    /* Reference counting on a process singleton: does it move at all? */
    ULONG r1 = m->lpVtbl->AddRef(m);
    ULONG r2 = m->lpVtbl->Release(m);
    printf("addref=%lu release=%lu\n", (unsigned long)r1, (unsigned long)r2);

    /* Identity across interfaces: COM requires IUnknown to be canonical. */
    void *pu = NULL, *pm = NULL;
    HRESULT h1 = m->lpVtbl->QueryInterface(m, &IID_IUnknown, &pu);
    HRESULT h2 = m->lpVtbl->QueryInterface(m, &IID_IMalloc, &pm);
    printf("QI unk hr=0x%lx same=%d | QI malloc hr=0x%lx same=%d\n",
           (unsigned long)h1, pu == (void *)m, (unsigned long)h2, pm == (void *)m);
    {   /* An interface it does not implement must be refused, and must NULL the out. */
        void *px = (void *)(size_t)0x1234;
        HRESULT h3 = m->lpVtbl->QueryInterface(m, &IID_IDataObject, &px);
        printf("QI other hr=0x%lx outnull=%d\n", (unsigned long)h3, px == NULL);
    }

    /* Does QueryInterface AddRef, as COM requires? The rows above do not
     * discriminate it, and guessing either way would be a guess. */
    {
        ULONG a = m->lpVtbl->AddRef(m);
        printf("refs after 3 QI = %lu\n", (unsigned long)(a - 1));
        m->lpVtbl->Release(m);
    }

    /* Allocation, size, and whether Alloc and CoTaskMemAlloc are ONE allocator. */
    void *p = m->lpVtbl->Alloc(m, 100);
    printf("alloc null=%d size=%lu\n", p == NULL,
           (unsigned long)m->lpVtbl->GetSize(m, p));
    memset(p, 0xAB, 100);
    printf("didalloc mine=%d\n", m->lpVtbl->DidAlloc(m, p));

    void *q = m->lpVtbl->Realloc(m, p, 300);
    printf("realloc null=%d size=%lu kept=%d\n", q == NULL,
           (unsigned long)m->lpVtbl->GetSize(m, q),
           q ? (((unsigned char *)q)[0] == 0xAB && ((unsigned char *)q)[99] == 0xAB) : 0);

    /* Cross-freeing proves the two entry points share one heap. */
    void *t = CoTaskMemAlloc(64);
    printf("taskalloc size-via-imalloc=%lu didalloc=%d\n",
           (unsigned long)m->lpVtbl->GetSize(m, t), m->lpVtbl->DidAlloc(m, t));
    m->lpVtbl->Free(m, t);            /* freed through IMalloc */
    CoTaskMemFree(q);                 /* freed through the flat API */

    /* The edges: NULL everywhere, and a pointer we never allocated. */
    printf("size(NULL)=%lu didalloc(NULL)=%d\n",
           (unsigned long)m->lpVtbl->GetSize(m, NULL), m->lpVtbl->DidAlloc(m, NULL));
    {
        char stackbuf[8];
        printf("didalloc(stack)=%d\n", m->lpVtbl->DidAlloc(m, stackbuf));
    }
    m->lpVtbl->Free(m, NULL);         /* must not crash */
    m->lpVtbl->HeapMinimize(m);
    puts("heapminimize ok");

    m->lpVtbl->Release(m);
    CoUninitialize();
    puts("done");
    return 0;
}
