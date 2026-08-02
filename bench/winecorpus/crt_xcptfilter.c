/* `_XcptFilter` — the CRT's top-level exception filter, where a Win32 structured
 * exception meets the C `signal()` world.
 *
 * The sweep is THREE-STATE (nothing installed / handlers installed / SIG_IGN) and
 * that is the whole point, not thoroughness for its own sake: delivering a signal
 * RESETS its disposition to SIG_DFL, so with handlers installed only the FIRST code
 * of each signal group ever fires. A single-state sweep would show an almost-empty
 * mapping and an implementation built from it would be wrong for every code but one
 * per group. The SIG_IGN pass consumes nothing and is what reveals the real groups.
 *
 * Two measured results that contradict the names:
 *   - STATUS_INTEGER_DIVIDE_BY_ZERO is NOT mapped to SIGFPE (nor INTEGER_OVERFLOW,
 *     ARRAY_BOUNDS_EXCEEDED or STACK_OVERFLOW). Only the seven floating-point
 *     statuses are.
 *   - With nothing installed the answer is CONTINUE_SEARCH (0), not EXECUTE_HANDLER:
 *     the CRT does not swallow the exception here.
 *
 * The .def beside this file forces the msvcrt import; mingw would otherwise resolve
 * nothing and the fixture would guard nothing (doc 70 section 7).
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
int __cdecl _XcptFilter(unsigned long code, EXCEPTION_POINTERS *ptr);
static const struct { unsigned long code; const char *nm; } CODES[] = {
  {0xC0000005,"ACCESS_VIOLATION"}, {0xC000008C,"ARRAY_BOUNDS"},
  {0xC000008D,"FLT_DENORMAL"},     {0xC000008E,"FLT_DIVIDE_BY_ZERO"},
  {0xC000008F,"FLT_INEXACT"},      {0xC0000090,"FLT_INVALID_OP"},
  {0xC0000091,"FLT_OVERFLOW"},     {0xC0000092,"FLT_STACK_CHECK"},
  {0xC0000093,"FLT_UNDERFLOW"},    {0xC0000094,"INT_DIVIDE_BY_ZERO"},
  {0xC0000095,"INT_OVERFLOW"},     {0xC0000096,"PRIV_INSTRUCTION"},
  {0xC000001D,"ILLEGAL_INSTRUCTION"}, {0xC00000FD,"STACK_OVERFLOW"},
  {0xE06D7363,"CXX_EXCEPTION"},    {0x12345678,"UNKNOWN"},
};
static volatile int hit;
static void __cdecl h_segv(int s){ printf("    [handler SIGSEGV s=%d]\n", s); fflush(stdout); hit=1; }
static void __cdecl h_fpe(int s){ printf("    [handler SIGFPE s=%d]\n", s); fflush(stdout); hit=1; }
static void __cdecl h_ill(int s){ printf("    [handler SIGILL s=%d]\n", s); fflush(stdout); hit=1; }
static void sweep(const char *tag){
  printf("== %s ==\n", tag); fflush(stdout);
  for (unsigned i=0;i<sizeof CODES/sizeof CODES[0];i++){
    EXCEPTION_RECORD rec; CONTEXT ctx; EXCEPTION_POINTERS p;
    memset(&rec,0,sizeof rec); memset(&ctx,0,sizeof ctx);
    rec.ExceptionCode = CODES[i].code; rec.ExceptionAddress=(void*)0x401000;
    p.ExceptionRecord=&rec; p.ContextRecord=&ctx;
    hit=0;
    int r = _XcptFilter(CODES[i].code, &p);
    printf("  %-22s -> %d handler-ran=%d\n", CODES[i].nm, r, hit); fflush(stdout);
  }
}
int main(void){
  sweep("no signal handlers");
  signal(SIGSEGV, h_segv); signal(SIGFPE, h_fpe); signal(SIGILL, h_ill);
  sweep("handlers installed");
  signal(SIGSEGV, SIG_IGN); signal(SIGFPE, SIG_IGN); signal(SIGILL, SIG_IGN);
  sweep("SIG_IGN");
  hit = 0;
  /* NULL EXCEPTION_POINTERS, in EVERY disposition state: the answer must not
   * depend on the signal table if the pointer is checked first, and one state
   * alone cannot tell those two designs apart. */
  printf("== NULL ptrs ==\n"); fflush(stdout);
  signal(SIGSEGV, SIG_DFL);
  printf("  NULL, SIGSEGV=SIG_DFL -> %d\n", _XcptFilter(0xC0000005, NULL)); fflush(stdout);
  signal(SIGSEGV, SIG_IGN);
  printf("  NULL, SIGSEGV=SIG_IGN -> %d\n", _XcptFilter(0xC0000005, NULL)); fflush(stdout);
  signal(SIGSEGV, h_segv);
  printf("  NULL, SIGSEGV=handler -> %d\n", _XcptFilter(0xC0000005, NULL)); fflush(stdout);
  printf("  handler-ran=%d\n", hit); fflush(stdout);
  printf("  NULL, unmapped code    -> %d\n", _XcptFilter(0x12345678, NULL)); fflush(stdout);
  return 0;
}
