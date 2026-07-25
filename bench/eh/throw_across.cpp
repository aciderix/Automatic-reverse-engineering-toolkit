/* C++ exception thrown across frames: the throw is in a callee (inner), the catch is in the
   caller (mainCRTStartup), and an intermediate frame (inner) has a local whose destructor must
   run during the unwind. This exercises the two-phase dispatch: search finds the caller's catch,
   then the unwind pass runs the destructors of every frame between the throw and the catch
   (here inner's Guard) — innermost first. Oracle vs Wine: "inner-dtor" then "r=42". This is the
   common real pattern (MFC/WinZip: throw deep, catch shallow, clean up in between). */
extern "C" __declspec(dllimport) int printf(const char*, ...);
struct Guard { ~Guard() { printf("inner-dtor\n"); } };
struct E { int code; };

__attribute__((noinline)) static void inner() {
    Guard g;               /* its destructor must run while the exception unwinds inner's frame */
    throw E{42};
    asm volatile("");      /* thwart tail/inline folding of the throw */
}

extern "C" int mainCRTStartup() {
    int r = 0;
    try {
        inner();
    } catch (E& e) {
        r = e.code;
    }
    printf("r=%d\n", r);   /* Wine: inner-dtor, then r=42 */
    return 0;
}
