/* C++ exception handling — the base cases, measured against Wine so ARET's
   __CxxFrameHandler3 / _CxxThrowException match. We enter at mainCRTStartup and use only
   printf, so the PE's sole runtime dependency is msvcrt (provided by both Wine and ARET).
   Kept to the cases that run cleanly under Wine with this minimal (no-CRT-init) setup;
   nested-unwind / rethrow are added as later fixtures once the base handler exists. */
extern "C" __declspec(dllimport) int printf(const char*, ...);
struct E { int code; };

extern "C" int mainCRTStartup() {
    int r = 0;
    try { throw E{42}; } catch (E& e) { r += e.code; }   /* class, caught by reference */
    try { throw 7;      } catch (int x) { r += x;      }   /* fundamental type */
    printf("r=%d\n", r);                                    /* Wine: 42 + 7 = 49 */
    return 0;
}
