/* C++ exception with a destructor whose side effect the optimizer cannot fold away (it
   calls printf), so clang must emit a real UnwindMap destructor funclet that the unwind
   runs. ARET does not yet model C++ unwind destructors, so rather than silently skip it
   (which would drop the "dtor" line and present a wrong stdout as correct), the HLE
   __CxxFrameHandler3 dispatch must ABORT loudly (sacred principle §0). Oracle: Wine prints
   "dtor" then "r=42"; ARET must abort, never print "r=42" alone. Checked separately from the
   ehdiff pass/fail set (which compares stdout equality) as an abort oracle. */
extern "C" __declspec(dllimport) int printf(const char*, ...);
struct Guard { ~Guard() { printf("dtor\n"); } };
struct E { int code; };

extern "C" int mainCRTStartup() {
    int r = 0;
    try {
        Guard g;               /* destructor (printf) must run while the exception unwinds */
        throw E{42};
    } catch (E& e) {
        r = e.code;
    }
    printf("r=%d\n", r);
    return 0;
}
