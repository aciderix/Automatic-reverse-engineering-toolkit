/* I4 probe A: LAYOUT, with cookie = 0 so the XOR decode is the identity and no
 * layout mistake can be confused with a decode mistake. Filters print and return
 * CONTINUE_SEARCH(0), so the handler walks every level and returns without ever
 * transferring control -> observable, and nothing crashes. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { DWORD prev, handler, scopetable, trylevel, _ebp, xpointers; } REG4;
typedef struct { int enclosing; DWORD filter, handler; } REC;
typedef struct { int gs_off, gs_xor, eh_off, eh_xor; REC r[4]; } TBL_HDR4;
typedef struct { REC r[4]; } TBL_NOHDR;

int __cdecl _except_handler4_common(ULONG *cookie, void (*check)(void),
        EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp);

static void __cdecl check_cookie(void){ printf("  CHECK-COOKIE CALLED\n"); fflush(stdout); }
static int __cdecl f0(void){ printf("  filter 0\n"); fflush(stdout); return 0; }
static int __cdecl f1(void){ printf("  filter 1\n"); fflush(stdout); return 0; }
static int __cdecl f2(void){ printf("  filter 2\n"); fflush(stdout); return 0; }
static int __cdecl f3(void){ printf("  filter 3\n"); fflush(stdout); return 0; }

static ULONG cookie = 0;
static TBL_HDR4 t_hdr;
static TBL_NOHDR t_raw;

static void call_it(const char *tag, DWORD table, int trylevel, DWORD flags)
{
    REG4 f; EXCEPTION_RECORD rec; CONTEXT ctx; void *disp = 0;
    memset(&rec,0,sizeof rec); memset(&ctx,0,sizeof ctx);
    rec.ExceptionCode = 0xC0000005; rec.ExceptionFlags = flags;
    f.prev = 0xFFFFFFFFu; f.handler = 0; f.scopetable = table;
    f.trylevel = (DWORD)trylevel; f._ebp = 0; f.xpointers = 0;
    printf("%s (trylevel=%d flags=%lx)\n", tag, trylevel, (unsigned long)flags); fflush(stdout);
    int r = _except_handler4_common(&cookie, check_cookie, &rec, &f, &ctx, &disp);
    printf("  -> returned %d\n", r); fflush(stdout);
}

int main(void)
{
    DWORD fs[4] = { (DWORD)(ULONG_PTR)f0, (DWORD)(ULONG_PTR)f1,
                    (DWORD)(ULONG_PTR)f2, (DWORD)(ULONG_PTR)f3 };
    /* Hypothesis 1: four-int header, then the records. */
    t_hdr.gs_off = -2; t_hdr.gs_xor = 0; t_hdr.eh_off = -2; t_hdr.eh_xor = 0;
    for (int i=0;i<4;i++){ t_hdr.r[i].enclosing = (i==0) ? -2 : i-1; t_hdr.r[i].filter = fs[i]; t_hdr.r[i].handler = 0; }
    /* Hypothesis 2: no header at all (v3 layout). */
    for (int i=0;i<4;i++){ t_raw.r[i].enclosing = (i==0) ? -2 : i-1; t_raw.r[i].filter = fs[i]; t_raw.r[i].handler = 0; }

    printf("== A: header-then-records, trylevel 2 ==\n");
    call_it("hdr4", (DWORD)(ULONG_PTR)&t_hdr, 2, 0);
    printf("== B: same, trylevel 0 ==\n");
    call_it("hdr4", (DWORD)(ULONG_PTR)&t_hdr, 0, 0);
    printf("== D: unwinding pass (flags=EH_UNWINDING) ==\n");
    call_it("hdr4", (DWORD)(ULONG_PTR)&t_hdr, 2, 2);
    printf("== E: trylevel = -2 (v4 terminator) ==\n");
    call_it("hdr4", (DWORD)(ULONG_PTR)&t_hdr, -2, 0);
    printf("== F: GS cookie offset REAL (must fire check_cookie) ==\n");
    t_hdr.gs_off = 0; t_hdr.gs_xor = 0;      /* frame->_ebp is 0, cookie is 0 -> matches! */
    call_it("gs=0 match", (DWORD)(ULONG_PTR)&t_hdr, 0, 0);
    t_hdr.gs_xor = 0x5a5a5a5a;               /* now it cannot match */
    call_it("gs=0 mismatch", (DWORD)(ULONG_PTR)&t_hdr, 0, 0);
    t_hdr.gs_off = -2; t_hdr.gs_xor = 0;
    return 0;
}
