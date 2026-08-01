/* GetUserNameA/W (advapi32) — the SIZE contract, which is where these get written
 * wrong, not the name.
 *
 * The name itself is environmental: it is whoever runs the test. But this fixture
 * still compares it, because Wine and ARET run as the same user on the same machine
 * in the same second — the value has one source of truth on both sides, so it is a
 * legitimate comparison rather than a machine-dependent one. What is *not* compared
 * is anything derived from a specific name (no hard-coded length), so the fixture
 * keeps working for any user.
 *
 * The interesting part is the in/out `pcbBuffer`, which a plausible implementation
 * gets wrong in at least four ways: does the count include the terminating NUL on
 * the way in, on the way out, on success, on failure? Each of those is probed
 * separately, on a poisoned buffer whose raw head is dumped, so "wrote nothing" and
 * "wrote a truncated name" cannot be confused. Sizes are in CHARACTERS for W and in
 * BYTES for A, which is itself a thing to get wrong, so both are shown.
 *
 * Every probe resets the last error first: an earlier call's error code leaking into
 * a later reading is the exact trap that produced a wrong conclusion in the SPI
 * work, so it is designed out here.
 *
 * Expected identical under Wine and ARET. */
#include <windows.h>
#include <stdio.h>

static void head_a(const char *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++) printf("%02x", (unsigned char)b[i]);
}
static void head_w(const wchar_t *b, int n)
{
    printf(" raw=");
    for (int i = 0; i < n; i++) printf("%04x", (unsigned)b[i]);
}

int main(void)
{
    char ba[64];
    wchar_t bw[64];
    DWORD n;
    BOOL ok;

    /* Baseline: a buffer that is certainly big enough. The name is echoed so the two
     * engines are compared on it; nothing below depends on its length being some
     * particular number. */
    SetLastError(0);
    memset(ba, 0x7e, sizeof ba);
    n = sizeof ba;
    ok = GetUserNameA(ba, &n);
    printf("A big   ok=%d err=%lu cb=%lu name=[%s] cb-vs-strlen=%d\n",
           !!ok, GetLastError(), (unsigned long)n, ba, (int)(n - strlen(ba)));

    SetLastError(0);
    for (int i = 0; i < 64; i++) bw[i] = 0x7e7e;
    n = 64;
    ok = GetUserNameW(bw, &n);
    printf("W big   ok=%d err=%lu cch=%lu name=[%ls] cch-vs-wcslen=%d\n",
           !!ok, GetLastError(), (unsigned long)n, bw, (int)(n - wcslen(bw)));

    /* A and W must agree on the name and on the count, since one is characters and
     * the other bytes only for a non-ASCII name. Printed as a relation, not a value. */
    {
        char a2[64]; wchar_t w2[64]; DWORD na = sizeof a2, nw = 64;
        SetLastError(0); GetUserNameA(a2, &na);
        SetLastError(0); GetUserNameW(w2, &nw);
        printf("A==W    same_len=%d same_count=%d\n",
               (int)(strlen(a2) == wcslen(w2)), (int)(na == nw));
    }

    /* Size query: zero-length buffer. Does it fail, with which error, and does it
     * report the required size — including the NUL or not? Buffer must be untouched. */
    SetLastError(0);
    memset(ba, 0x7e, sizeof ba);
    n = 0;
    ok = GetUserNameA(ba, &n);
    printf("A zero  ok=%d err=%lu cb=%lu", !!ok, GetLastError(), (unsigned long)n);
    head_a(ba, 8);
    puts("");

    SetLastError(0);
    for (int i = 0; i < 64; i++) bw[i] = 0x7e7e;
    n = 0;
    ok = GetUserNameW(bw, &n);
    printf("W zero  ok=%d err=%lu cch=%lu", !!ok, GetLastError(), (unsigned long)n);
    head_w(bw, 8);
    puts("");

    /* One short of what the baseline reported: the boundary between success and
     * ERROR_INSUFFICIENT_BUFFER, and whether a partial name is left behind. */
    {
        DWORD need = sizeof ba;
        SetLastError(0);
        GetUserNameA(ba, &need);          /* need = whatever the success case reports */
        SetLastError(0);
        memset(ba, 0x7e, sizeof ba);
        n = need - 1;
        ok = GetUserNameA(ba, &n);
        printf("A short ok=%d err=%lu cb=%lu", !!ok, GetLastError(), (unsigned long)n);
        head_a(ba, 8);
        puts("");

        /* And exactly what it reported: this must succeed. */
        SetLastError(0);
        memset(ba, 0x7e, sizeof ba);
        n = need;
        ok = GetUserNameA(ba, &n);
        printf("A exact ok=%d err=%lu cb=%lu name_ok=%d\n",
               !!ok, GetLastError(), (unsigned long)n, (int)(strlen(ba) == need - 1));
    }
    {
        DWORD need = 64;
        SetLastError(0);
        GetUserNameW(bw, &need);
        SetLastError(0);
        for (int i = 0; i < 64; i++) bw[i] = 0x7e7e;
        n = need - 1;
        ok = GetUserNameW(bw, &n);
        printf("W short ok=%d err=%lu cch=%lu", !!ok, GetLastError(), (unsigned long)n);
        head_w(bw, 8);
        puts("");

        SetLastError(0);
        for (int i = 0; i < 64; i++) bw[i] = 0x7e7e;
        n = need;
        ok = GetUserNameW(bw, &n);
        printf("W exact ok=%d err=%lu cch=%lu name_ok=%d\n",
               !!ok, GetLastError(), (unsigned long)n, (int)(wcslen(bw) == need - 1));
    }
    return 0;
}
