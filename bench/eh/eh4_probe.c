/* INSTRUMENT-FIRST harness for `_except_handler4_common` (doc 81 I4, doc 80 §1.3).
 *
 * This is a MEASUREMENT probe, not a fixture and not a gate. It exists so the next
 * session does not have to rebuild the harness: the hard part of this brick is not
 * the logic (brick C's `_except_handler3` already has the walk, the local/global
 * unwind and the non-local transfer) but finding out how the v4 frame ENCODES its
 * scope table before any pointer in it can be trusted.
 *
 * Build and run (the .def is required: `_except_handler4_common` is not in mingw's
 * headers, and without forcing the import you measure nothing — doc 70 §7):
 *
 *   i686-w64-mingw32-dlltool -d eh4_probe.def -l eh4.a
 *   i686-w64-mingw32-gcc -O0 -w eh4_probe.c eh4.a -o eh4.exe
 *   i686-w64-mingw32-objdump -p eh4.exe | grep _except_handler4_common   # MUST show it
 *   wine eh4.exe 0    # scopetable stored PLAIN
 *   wine eh4.exe 1    # scopetable stored XOR'd with *cookie
 *
 * ── WHAT IS ALREADY PROVEN BY THIS PROBE (2026-08-01) ────────────────────────────
 *   * The signature is `(ULONG *cookie, void (*check_cookie)(void),
 *     EXCEPTION_RECORD *rec, FRAME *frame, CONTEXT *ctx, void **dispatcher)` — Wine's
 *     own symbols print the parameter names and values in the backtrace, so this is
 *     read off the oracle rather than assumed.
 *   * ⭐ The frame's `scopetable` field is XOR-ENCODED WITH `*cookie`, proven in BOTH
 *     directions on one run each:
 *       - stored PLAIN  -> it faults reading `stored ^ cookie` (edi = &tbl ^ cookie);
 *       - stored XOR'd  -> edi = &tbl exactly, i.e. it read the table we built.
 *     One direction alone would not have settled it; a coincidence has to survive
 *     both.
 *   * The layout {prev, handler, scopetable, trylevel, _ebp, xpointers} is accepted
 *     (no early rejection), matching brick C's `funclet ebp = EstablisherFrame + 16`.
 *
 * ── STILL OPEN, EACH NEEDING ITS OWN EXPERIMENT (do not guess these) ─────────────
 *   * Is `trylevel` encoded too, or stored plain? (The XOR run walked off into a call
 *     to 0xfffffffe, which is consistent with a mis-decoded level but is NOT proof.)
 *   * What do `gs_cookie_offset` / `eh_cookie_offset` = -2 mean — "absent", or a real
 *     offset? MSVC uses -2 as a sentinel; Wine's behaviour must be measured.
 *   * Where does the ScopeRecord array start relative to the four-int header, and is
 *     the record layout still {EnclosingLevel, FilterFunc, HandlerFunc}?
 *   * The `check_cookie` callback protocol (called with what, allowed to return?).
 *
 * Wine's implementation is at dlls/msvcrt/except_i386.c (the fault above pointed at
 * line 940), which is the recipe to read if the source becomes reachable — doc 70 §7
 * "Wine = livre de recettes, pas runtime".
 *
 * ── A CHEAPER ROUTE WORTH MEASURING FIRST ────────────────────────────────────────
 * WinMerge imports this from its OWN msvcr90.dll, which ships beside the exe. Under
 * the corrected DLL rule (thunks AND forwarders, doc 70 §5.0) that DLL may well
 * implement it, in which case lifting msvcr90 could serve this the way lifting
 * shell32 served SHGetSpecialFolderLocation — for the price of one --with-dll. The
 * lifted handler would still have to walk OUR synthetic fs:[0] and reach our
 * longjmp, so it is not free, but it is one measurement away and should be tried
 * before writing the handler by hand.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { DWORD prev, handler, scopetable, trylevel, _ebp, xpointers; } REG4;
typedef struct {
    int gs_cookie_offset, gs_cookie_xor, eh_cookie_offset, eh_cookie_xor;
    struct { int enclosing; DWORD filter; DWORD handler; } e[4];
} TABLE4;

int __cdecl _except_handler4_common(ULONG *cookie, void (*check)(void),
        EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp);

static const char *g_tag = "?";
static void __cdecl check_cookie(void) { printf("  %s: COOKIE-CHECK CALLED\n", g_tag); fflush(stdout); _exit(7); }

static TABLE4 tbl;
static ULONG cookie = 0x12345678;

static void run(const char *tag, DWORD stored)
{
    REG4 f;
    EXCEPTION_RECORD rec;
    CONTEXT ctx;
    void *disp = 0;
    memset(&rec, 0, sizeof rec); memset(&ctx, 0, sizeof ctx);
    rec.ExceptionCode = 0xC0000005; rec.ExceptionFlags = 0;
    f.prev = 0xFFFFFFFFu; f.handler = 0; f.scopetable = stored;
    f.trylevel = 0; f._ebp = 0; f.xpointers = 0;
    g_tag = tag;
    printf("%s: calling (stored=%08lx, cookie=%08lx)\n", tag,
           (unsigned long)stored, (unsigned long)cookie); fflush(stdout);
    int r = _except_handler4_common(&cookie, check_cookie, &rec, &f, &ctx, &disp);
    printf("  %s: returned %d (no cookie check)\n", tag, r); fflush(stdout);
}

int main(int argc, char **argv)
{
    /* A GS cookie the handler must find at frame->_ebp + gs_cookie_offset. Point it
     * at f._ebp itself (offset 0) and store a value that cannot match, so the check
     * fires whenever the table was really read. */
    tbl.gs_cookie_offset = 0;
    tbl.gs_cookie_xor    = 0;
    tbl.eh_cookie_offset = -2;
    tbl.eh_cookie_xor    = 0;
    for (int i = 0; i < 4; i++) { tbl.e[i].enclosing = -1; tbl.e[i].filter = 0; tbl.e[i].handler = 0; }

    int which = argc > 1 ? atoi(argv[1]) : 0;
    if (which == 0) run("PLAIN", (DWORD)(ULONG_PTR)&tbl);
    else            run("XOR  ", (DWORD)(ULONG_PTR)&tbl ^ cookie);
    return 0;
}
