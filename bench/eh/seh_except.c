/* Structured Exception Handling (__try/__except/__finally) — the SEH scope-table model
   MSVC/clang emit via _except_handler3. A software RaiseException inside __try transfers
   to the __except filter/handler; __finally runs on both normal and exceptional exit; the
   filter can inspect the code and decline (CONTINUE_SEARCH) so an outer handler catches.
   Measured against Wine so ARET's _except_handler3 matches. Enters at mainCRTStartup;
   msvcrt (printf) + kernel32 (RaiseException) at runtime. */
__declspec(dllimport) int printf(const char*, ...);
__declspec(dllimport) void __stdcall RaiseException(unsigned code, unsigned flags,
                                                    unsigned nargs, const unsigned *args);
#define MYCODE 0xE0001234u

static int g_fin;
static int simple(void) {
    __try { RaiseException(MYCODE, 0, 0, 0); return 999; }   /* raise -> handler */
    __except (1 /*EXECUTE_HANDLER*/) { return 42; }
}
static int finally_normal(void) {
    __try { __try { return 1; } __finally { g_fin += 10; } }  /* normal exit runs finally */
    __except (1) { return 2; }
}
static int finally_raise(void) {
    __try { __try { RaiseException(MYCODE, 0, 0, 0); return 1; } __finally { g_fin += 100; } }
    __except (1) { return 3; }                                 /* finally unwinds, then handler */
}
static int filter_search(void) {
    /* inner filter declines (CONTINUE_SEARCH=0), outer catches */
    __try { __try { RaiseException(MYCODE, 0, 0, 0); return 1; }
            __except (0 /*CONTINUE_SEARCH*/) { return 4; } }
    __except (1) { return 5; }
}
int mainCRTStartup(void) {
    int a = simple();          /* 42 */
    int b = finally_normal();  /* 1, g_fin+=10 */
    int c = finally_raise();   /* 3, g_fin+=100 */
    int d = filter_search();   /* 5 */
    printf("a=%d b=%d c=%d d=%d fin=%d\n", a, b, c, d, g_fin);  /* Wine: 42 1 3 5 110 */
    return 0;
}
