/* Guard for the winetest-prioritised batch: SetConsoleMode (the #1 missing import
 * across the Wine conformance suite), the oleaut32 BSTR ABI, and minimal ole32
 * (CoInitialize/CoUninitialize + the task allocator). Every line must match the
 * same PE under Wine bit-for-bit. Under the differential harness stdout is a pipe,
 * so the console handle is NOT a console — Get/SetConsoleMode must both report
 * FALSE, exactly like Windows/Wine. The BSTR checks pin the contractual layout
 * (length prefix, char access, round-trip). */
#include <windows.h>
#include <oleauto.h>
#include <objbase.h>
#include <stdio.h>

int main(void) {
    /* --- console: redirected handle -> both report failure (non-console) --- */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    int gm = GetConsoleMode(h, &mode) ? 1 : 0;
    int sm = SetConsoleMode(h, ENABLE_PROCESSED_OUTPUT) ? 1 : 0;
    printf("console get=%d set=%d\n", gm, sm);

    /* --- BSTR: contractual [len][wchars][NUL] layout --- */
    BSTR b = SysAllocString(L"hello");
    printf("bstr len=%u c0=%d c4=%d\n", (unsigned)SysStringLen(b), (int)b[0], (int)b[4]);
    BSTR b2 = SysAllocStringLen(L"abcdef", 3);
    printf("bstrlen len=%u bytelen=%u c0=%d c2=%d nul=%d\n",
           (unsigned)SysStringLen(b2), (unsigned)SysStringByteLen(b2),
           (int)b2[0], (int)b2[2], (int)b2[3]);
    SysFreeString(b);
    SysFreeString(b2);
    SysFreeString(NULL); /* must be a safe no-op */

    /* --- minimal COM: first init S_OK, nested S_FALSE, task alloc round-trip --- */
    HRESULT r1 = CoInitialize(NULL);
    HRESULT r2 = CoInitialize(NULL);
    void *p = CoTaskMemAlloc(32);
    int allocated = p != NULL;
    p = CoTaskMemRealloc(p, 64);
    int realloced = p != NULL;
    CoTaskMemFree(p);
    CoUninitialize();
    CoUninitialize();
    printf("co r1=%ld r2=%ld alloc=%d realloc=%d\n", (long)r1, (long)r2, allocated, realloced);
    return 0;
}
