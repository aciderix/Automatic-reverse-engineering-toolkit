/* `_except_handler4_common` — the MSVC /GS ("v4") scope-table SEH handler.
 *
 * mingw cannot emit __try/__except, so — exactly as bricks B and C did — the frame
 * and the scope table are built BY HAND and the handler is called directly. The .def
 * beside this file forces the msvcrt import: without it mingw would resolve nothing
 * and the fixture would guard nothing (doc 70 section 7).
 *
 * Only the NON-TRANSFERRING paths are exercised here: filters that return
 * CONTINUE_SEARCH, the unwinding pass, and the chain terminator. Reaching
 * EXECUTE_HANDLER requires a real `mov fs:[0]` establish so the injected setjmp
 * exists to longjmp back to, which a hand-built frame does not have; that path is
 * shared verbatim with `_except_handler3` and is already covered by ehdiff.
 *
 * What each group pins down, all of it measured before it was implemented:
 *   - the scope-table pointer in the frame is XOR-ENCODED with *cookie (this fixture
 *     stores it encoded; storing it plain makes the handler read elsewhere);
 *   - the table opens with a FOUR-INT header before the records, so the records
 *     start 16 bytes in;
 *   - the chain terminator is -2, NOT -1. Level 0 below ends in -2, and there is a
 *     separate trylevel=-1 case: under v3's terminator that would run off the front
 *     of the array and CALL the header's third int. The two must not print alike.
 *   - the unwinding pass runs __finally blocks and calls NO filter.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { DWORD prev, handler, scopetable, trylevel, _ebp, xpointers; } REG4;
typedef struct { int enclosing; DWORD filter, handler; } REC;
typedef struct { int gs_off, gs_xor, eh_off, eh_xor; REC r[4]; } TBL;

int __cdecl _except_handler4_common(ULONG *, void (*)(void), EXCEPTION_RECORD *,
                                    void *, CONTEXT *, void *);

static void __cdecl check_cookie(void) { printf("  CHECK-COOKIE CALLED\n"); fflush(stdout); }
static int __cdecl f0(void) { printf("  filter 0\n"); fflush(stdout); return 0; }
static int __cdecl f1(void) { printf("  filter 1\n"); fflush(stdout); return 0; }
static int __cdecl f2(void) { printf("  filter 2\n"); fflush(stdout); return 0; }
static int __cdecl fin0(void) { printf("  finally 0\n"); fflush(stdout); return 0; }
static int __cdecl fin1(void) { printf("  finally 1\n"); fflush(stdout); return 0; }

static ULONG cookie = 0x0f0f0f0f;
static TBL tbl;

static void call_it(const char *tag, int trylevel, DWORD flags, int encode)
{
    REG4 f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    void *disp = 0;
    memset(&rec, 0, sizeof rec);
    memset(&ctx, 0, sizeof ctx);
    rec.ExceptionCode = 0xC0000005;
    rec.ExceptionFlags = flags;
    f.prev = 0xFFFFFFFFu;
    f.handler = 0;
    f.scopetable = encode ? ((DWORD)(ULONG_PTR)&tbl ^ cookie) : (DWORD)(ULONG_PTR)&tbl;
    f.trylevel = (DWORD)trylevel;
    f._ebp = 0;
    f.xpointers = 0;
    printf("%s (trylevel=%d flags=%lu)\n", tag, trylevel, (unsigned long)flags);
    fflush(stdout);
    int r = _except_handler4_common(&cookie, check_cookie, &rec, &f, &ctx, &disp);
    printf("  -> returned %d\n", r);
    fflush(stdout);
}

int main(void)
{
    tbl.gs_off = -2; tbl.gs_xor = 0; tbl.eh_off = -2; tbl.eh_xor = 0;
    /* levels 2 -> 1 -> 0 -> end(-2); levels 0 and 1 also carry a __finally so the
     * unwinding pass has something to show. A __finally is filter == 0. */
    tbl.r[0].enclosing = -2; tbl.r[0].filter = (DWORD)(ULONG_PTR)f0;   tbl.r[0].handler = 0;
    tbl.r[1].enclosing = 0;  tbl.r[1].filter = (DWORD)(ULONG_PTR)f1;   tbl.r[1].handler = 0;
    tbl.r[2].enclosing = 1;  tbl.r[2].filter = (DWORD)(ULONG_PTR)f2;   tbl.r[2].handler = 0;
    tbl.r[3].enclosing = 2;  tbl.r[3].filter = 0;                      tbl.r[3].handler = 0;

    call_it("search from 2", 2, 0, 1);
    call_it("search from 0", 0, 0, 1);
    call_it("terminator -2", -2, 0, 1);

    /* Now a __finally at levels 0 and 1, exercised by the unwinding pass. A filter of
     * 0 marks __finally, and its cleanup block is the third word. */
    tbl.r[0].filter = 0; tbl.r[0].handler = (DWORD)(ULONG_PTR)fin0;
    tbl.r[1].filter = 0; tbl.r[1].handler = (DWORD)(ULONG_PTR)fin1;
    call_it("unwind from 1", 1, EXCEPTION_UNWINDING, 1);
    call_it("unwind from -2", -2, EXCEPTION_UNWINDING, 1);
    return 0;
}
