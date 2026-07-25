/* C++ exception with a destructor whose side effect the optimizer cannot fold away (it
   calls printf), so clang emits a real UnwindMap destructor funclet that the unwind runs.
   ARET's __CxxFrameHandler3 dispatch runs the local destructors (aret_cxx_local_unwind,
   mirroring Wine's cxx_local_unwind) before transferring to the catch. Oracle: Wine prints
   "dtor" then "r=42"; ARET matches (the destructor's side effect appears, in order). */
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
