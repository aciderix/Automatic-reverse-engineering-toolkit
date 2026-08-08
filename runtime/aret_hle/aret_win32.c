/* ARET Win32 layer — the "colle Windows" (doc 30, reco #2 / brique [3]).
 *
 * Native, POSIX-backed implementations of the common kernel32 surface that has a
 * clean Linux equivalent (timing, environment, heap, atomics, paths, debug out).
 * Same model as aret_crt.c: each `aret_<Name>` reads its stdcall arguments from
 * the shared machine stack ([esp+0], [esp+4], …) and calls native POSIX. These
 * are strong definitions overriding the builder's weak stubs.
 *
 * This is the unique-to-us glue between lifted code and the host OS — the part
 * neither rev.ng nor RetDec provide. It is *native* (not Wine): a real Windows
 * program's kernel32 calls run directly on Linux primitives.
 *
 * Honest scope: this is the POSIX-mappable subset (no GUI/USER32, no registry,
 * no real PE module machinery). Functions with no clean POSIX analogue stay weak
 * stubs reported at runtime; the full path for those is Winelib (doc 30 §4 [3]).
 */

#include "aret_hle.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#ifndef __wasm__
#include <sys/statvfs.h>   /* no filesystem statvfs under wasm32-wasi */
#include <ucontext.h>      /* cooperative threads (fibers) — no ucontext in WASM */
#endif
#include <sys/ioctl.h>
#include <pwd.h>          /* GetUserName reads the passwd database, as Wine does */

/* G2b (doc 72): a *visible* window is presented via SDL2 (portable: Linux/macOS,
 * and WASM via Emscripten later). SDL2 is linked ONLY when the program creates a
 * window (builder gates `-DARET_HAVE_SDL` on a window-creating import + pkg-config
 * sdl2). When absent, the whole window layer stays display-free (sound no-op
 * drawing), exactly as before. The SDL layer is strictly *additive*: it never
 * injects a message that Wine would not, so the deterministic message/geometry
 * oracles stay bit-identical. */
#ifdef ARET_HAVE_SDL
#include <SDL.h>
#endif

/* G3-text (doc 72): GDI text (TextOut/…) is rasterized with FreeType — the SAME
 * rasterizer Wine uses, so glyphs are bit-identical to Wine's ground truth — and
 * the logical face name is resolved to a real font file with fontconfig, the SAME
 * mechanism Wine uses on Linux. Linked ONLY when the program draws text (builder
 * gates `-DARET_HAVE_FREETYPE` on a text import + pkg-config freetype2/fontconfig).
 * When absent, TextOut is a sound abort (never a silent wrong render). The binary
 * stays autonomous: it carries the real font glyphs, no Wine runtime dependency
 * (FreeType is statically linkable and WASM-capable). */
#ifdef ARET_HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_LCD_FILTER_H
#include <fontconfig/fontconfig.h>
#endif

static inline uint32_t w32_arg(uint32_t esp, int i) {
    return ((const uint32_t *)(uintptr_t)esp)[i];
}
#define WI(i)  ((int)w32_arg(esp, (i)))
#define WU(i)  (w32_arg(esp, (i)))
#define WP(i)  ((void *)(uintptr_t)w32_arg(esp, (i)))
#define WS(i)  ((char *)(uintptr_t)w32_arg(esp, (i)))
#define WCS(i) ((const char *)(uintptr_t)w32_arg(esp, (i)))
#define WRP(x) ((uint32_t)(uintptr_t)(x))

/* ------------------------------------------------------------------ */
/* Timing                                                             */
/* ------------------------------------------------------------------ */

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint32_t aret_GetTickCount(uint32_t esp) { (void)esp; return (uint32_t)(mono_ns() / 1000000ull); }

/* QueryPerformanceCounter(LARGE_INTEGER* out) — 64-bit nanosecond counter. */
uint32_t aret_QueryPerformanceCounter(uint32_t esp) {
    uint64_t v = mono_ns();
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = (uint32_t)v; ((uint32_t *)(uintptr_t)p)[1] = (uint32_t)(v >> 32); }
    return 1;
}
/* Frequency = 1e9 (we report the counter in nanoseconds). */
uint32_t aret_QueryPerformanceFrequency(uint32_t esp) {
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = 1000000000u; ((uint32_t *)(uintptr_t)p)[1] = 0; }
    return 1;
}
/* GetSystemTimeAsFileTime(FILETIME*) — 100ns ticks since 1601. */
uint32_t aret_GetSystemTimeAsFileTime(uint32_t esp) {
    struct timeval tv; gettimeofday(&tv, NULL);
    uint64_t ft = ((uint64_t)tv.tv_sec * 10000000ull) + ((uint64_t)tv.tv_usec * 10ull)
                + 116444736000000000ull; /* Unix epoch -> 1601 epoch */
    uint32_t p = WU(0);
    if (p) { ((uint32_t *)(uintptr_t)p)[0] = (uint32_t)ft; ((uint32_t *)(uintptr_t)p)[1] = (uint32_t)(ft >> 32); }
    return 0;
}

/* Fill a SYSTEMTIME (8 WORDs: year,month,dow,day,hour,min,sec,ms) from a broken-
 * down time. Shared by GetSystemTime (UTC) and GetLocalTime. */
static void aret_fill_systemtime(uint32_t p, int local) {
    if (!p) return;
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec; struct tm tmv;
    if (local) localtime_r(&t, &tmv); else gmtime_r(&t, &tmv);
    uint16_t *w = (uint16_t *)(uintptr_t)p;
    w[0] = (uint16_t)(tmv.tm_year + 1900);
    w[1] = (uint16_t)(tmv.tm_mon + 1);
    w[2] = (uint16_t)tmv.tm_wday;
    w[3] = (uint16_t)tmv.tm_mday;
    w[4] = (uint16_t)tmv.tm_hour;
    w[5] = (uint16_t)tmv.tm_min;
    w[6] = (uint16_t)tmv.tm_sec;
    w[7] = (uint16_t)(tv.tv_usec / 1000);
}
uint32_t aret_GetSystemTime(uint32_t esp) { aret_fill_systemtime(WU(0), 0); return 0; }
uint32_t aret_GetLocalTime(uint32_t esp) { aret_fill_systemtime(WU(0), 1); return 0; }

/* SystemTimeToFileTime(const SYSTEMTIME*, FILETIME*) -> 100ns ticks since 1601. */
uint32_t aret_SystemTimeToFileTime(uint32_t esp) {
    const uint16_t *w = (const uint16_t *)(uintptr_t)WU(0);
    uint32_t out = WU(1);
    if (!w || !out) return 0;
    struct tm tmv; memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = w[0] - 1900; tmv.tm_mon = w[1] - 1; tmv.tm_mday = w[3];
    tmv.tm_hour = w[4]; tmv.tm_min = w[5]; tmv.tm_sec = w[6];
    time_t t = timegm(&tmv);
    uint64_t ft = ((uint64_t)t * 10000000ull) + ((uint64_t)w[7] * 10000ull) + 116444736000000000ull;
    ((uint32_t *)(uintptr_t)out)[0] = (uint32_t)ft;
    ((uint32_t *)(uintptr_t)out)[1] = (uint32_t)(ft >> 32);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Environment / process identity / paths                             */
/* ------------------------------------------------------------------ */

/* A stable non-zero thread id (single-threaded model: the process id serves). */
uint32_t aret_GetCurrentThreadId(uint32_t esp) { (void)esp; return (uint32_t)getpid(); }

uint32_t aret_GetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0);
    char *buf = WS(1);
    uint32_t size = WU(2);
    const char *v = name ? getenv(name) : NULL;
    if (!v) return 0;
    uint32_t len = (uint32_t)strlen(v);
    if (buf && size > len) { memcpy(buf, v, len + 1); return len; }
    return len + 1; /* required size incl. NUL */
}
/* GetEnvironmentVariableW(name(wide), buf(wide), size(WCHARs)) — wide sibling of
 * the A variant. Narrows the UTF-16 name (ASCII/Latin-1, the CLI case) to look up
 * via getenv, widens the value back. Returns WCHARs written excl. NUL, the
 * required size incl. NUL if buf is too small, or 0 (not found). */
uint32_t aret_GetEnvironmentVariableW(uint32_t esp) {
    const uint16_t *wname = (const uint16_t *)WP(0);
    uint16_t *buf = (uint16_t *)WP(1);
    uint32_t size = WU(2);
    if (!wname) return 0;
    char name[256]; size_t i = 0;
    for (; wname[i] && i + 1 < sizeof name; i++) name[i] = (char)(wname[i] & 0xffu);
    name[i] = 0;
    const char *v = getenv(name);
    if (!v) return 0;
    uint32_t len = (uint32_t)strlen(v);
    if (buf && size > len) {
        for (uint32_t j = 0; j < len; j++) buf[j] = (uint16_t)(unsigned char)v[j];
        buf[len] = 0;
        return len;
    }
    return len + 1; /* required size incl. NUL */
}
uint32_t aret_SetEnvironmentVariableA(uint32_t esp) {
    const char *name = WCS(0), *val = WCS(1);
    if (!name) return 0;
    return (uint32_t)(val ? (setenv(name, val, 1) == 0) : (unsetenv(name) == 0));
}
/* GetEnvironmentStringsW/A() -> a freshly allocated, double-NUL-terminated block
 * of "VAR=VALUE\0" strings built from the host environment (wide for W, bytes for
 * A). Programs that read the whole environment (PuTTY/plink at startup) walk this
 * block; the old stub returned NULL and the caller dereferenced it -> segfault.
 * Freed by FreeEnvironmentStrings{W,A}. */
extern char **environ;
uint32_t aret_GetEnvironmentStringsW(uint32_t esp) {
    (void)esp;
    size_t n = 1; /* the block's trailing NUL */
    for (char **e = environ; e && *e; e++) n += strlen(*e) + 1;
    uint16_t *block = (uint16_t *)malloc(n * sizeof(uint16_t));
    if (!block) return 0;
    uint16_t *p = block;
    for (char **e = environ; e && *e; e++) {
        for (const char *s = *e; *s; s++) *p++ = (uint16_t)(unsigned char)*s;
        *p++ = 0;
    }
    *p = 0; /* block terminator (empty string) */
    return (uint32_t)(uintptr_t)block;
}
uint32_t aret_GetEnvironmentStringsA(uint32_t esp) {
    (void)esp;
    size_t n = 1;
    for (char **e = environ; e && *e; e++) n += strlen(*e) + 1;
    char *block = (char *)malloc(n);
    if (!block) return 0;
    char *p = block;
    for (char **e = environ; e && *e; e++) {
        size_t l = strlen(*e) + 1;
        memcpy(p, *e, l);
        p += l;
    }
    *p = 0;
    return (uint32_t)(uintptr_t)block;
}
/* GetEnvironmentStrings (no suffix) is the ANSI entry on Win32. */
uint32_t aret_GetEnvironmentStrings(uint32_t esp) { return aret_GetEnvironmentStringsA(esp); }
uint32_t aret_FreeEnvironmentStringsW(uint32_t esp) { free(WP(0)); return 1; }
uint32_t aret_FreeEnvironmentStringsA(uint32_t esp) { free(WP(0)); return 1; }

static void u32_w2n(const uint16_t *s, char *d, int cap);   /* fwd: UTF-16 -> ANSI */
/* ---- In-memory registry (advapi32) ----------------------------------------
 * A real, process-local registry tree so a program that WRITES its settings and READS
 * them back round-trips exactly — the dominant pattern (RegCreateKey was the measured
 * head, 17/29 Win95 binaries). It starts EMPTY: a value never written this run (a
 * system key, or one a prior run / installer would have set) is honestly
 * ERROR_FILE_NOT_FOUND, never a guessed value (sound; the program takes its default
 * path). Verified bit-exact vs Wine on the create/set/query/enum/delete round-trip.
 * Predefined roots exist from the start; keys/values live in bounded arrays. LSTATUS:
 * SUCCESS=0, FILE_NOT_FOUND=2, ACCESS_DENIED=5, INVALID_HANDLE=6, MORE_DATA=234,
 * NO_MORE_ITEMS=259. Disposition: REG_CREATED_NEW_KEY=1, REG_OPENED_EXISTING_KEY=2. */
#define U32_REG_BASE   0x75000000u
#define U32_MAX_REGKEY 512
#define U32_MAX_REGVAL 24
#define U32_REGNAME    80
#define U32_REGDATA    512
#define U32_REG_ROOTS  7
struct u32_regval { int used; char name[U32_REGNAME]; uint32_t type, len; uint8_t data[U32_REGDATA]; };
struct u32_regkey { int used, parent; uint32_t root; char name[U32_REGNAME]; struct u32_regval val[U32_MAX_REGVAL]; };
static struct u32_regkey g_reg[U32_MAX_REGKEY];
static int g_reg_ready = 0;
static void u32_reg_init(void) {
    if (g_reg_ready) return;
    g_reg_ready = 1;
    static const uint32_t roots[U32_REG_ROOTS] = {
        0x80000000u,0x80000001u,0x80000002u,0x80000003u,0x80000004u,0x80000005u,0x80000006u };
    for (int i = 0; i < U32_REG_ROOTS; i++) { g_reg[i].used = 1; g_reg[i].parent = -1; g_reg[i].root = roots[i]; }
}
static int u32_reg_idx(uint32_t h) {
    u32_reg_init();
    for (int i = 0; i < U32_REG_ROOTS; i++) if (g_reg[i].root == h) return i;
    if ((h & 0xFF000000u) == U32_REG_BASE) { uint32_t i = h & 0x00FFFFFFu; return (i < U32_MAX_REGKEY && g_reg[i].used) ? (int)i : -1; }
    return -1;
}
static int u32_reg_child(int parent, const char *name) {
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++)
        if (g_reg[i].used && g_reg[i].parent == parent && !strcasecmp(g_reg[i].name, name)) return i;
    return -1;
}
static int u32_reg_new(int parent, const char *name) {
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++)
        if (!g_reg[i].used) {
            memset(&g_reg[i], 0, sizeof g_reg[i]);
            g_reg[i].used = 1; g_reg[i].parent = parent; g_reg[i].root = 0;
            strncpy(g_reg[i].name, name, U32_REGNAME - 1);
            return i;
        }
    return -1;
}
/* Walk hparent\path (backslash- or slash-separated); create missing components if
 * `create`. *disp = 1 if the FINAL key was newly created, else 2. Returns key index. */
static int u32_reg_walk(uint32_t hparent, const char *path, int create, uint32_t *disp) {
    int cur = u32_reg_idx(hparent);
    if (cur < 0) return -1;
    int last_created = 0;
    if (path) {
        const char *p = path; char comp[U32_REGNAME];
        while (*p) {
            int n = 0;
            while (*p && *p != '\\' && *p != '/' && n < U32_REGNAME - 1) comp[n++] = *p++;
            comp[n] = 0;
            while (*p == '\\' || *p == '/') p++;
            if (n == 0) continue;
            int nxt = u32_reg_child(cur, comp);
            if (nxt < 0) { if (!create) return -1; nxt = u32_reg_new(cur, comp); if (nxt < 0) return -1; last_created = 1; }
            else last_created = 0;
            cur = nxt;
        }
    }
    if (disp) *disp = last_created ? 1u : 2u;
    return cur;
}
static uint32_t u32_reg_hkey(int k) { return (k >= 0 && k < U32_REG_ROOTS) ? g_reg[k].root : (U32_REG_BASE | (uint32_t)k); }
static struct u32_regval *u32_reg_findval(int k, const char *name, int create) {
    if (!name) name = "";
    struct u32_regkey *K = &g_reg[k];
    for (int i = 0; i < U32_MAX_REGVAL; i++) if (K->val[i].used && !strcasecmp(K->val[i].name, name)) return &K->val[i];
    if (!create) return NULL;
    for (int i = 0; i < U32_MAX_REGVAL; i++) if (!K->val[i].used) {
        memset(&K->val[i], 0, sizeof K->val[i]); K->val[i].used = 1;
        strncpy(K->val[i].name, name, U32_REGNAME - 1);
        return &K->val[i];
    }
    return NULL;
}
static void u32_reg_del_subtree(int k) {
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++) if (g_reg[i].used && g_reg[i].parent == k) u32_reg_del_subtree(i);
    g_reg[k].used = 0;
}
/* Narrow cores (A shims pass their string; W shims convert name then call these). */
static uint32_t u32_reg_create(uint32_t hk, const char *sub, uint32_t phk, uint32_t pdisp) {
    uint32_t disp = 0; int k = u32_reg_walk(hk, sub, 1, &disp);
    if (k < 0) { if (phk) *(uint32_t *)(uintptr_t)phk = 0; return 5; }
    if (phk) *(uint32_t *)(uintptr_t)phk = u32_reg_hkey(k);
    if (pdisp) *(uint32_t *)(uintptr_t)pdisp = disp;
    return 0;
}
static uint32_t u32_reg_open(uint32_t hk, const char *sub, uint32_t phk) {
    int k = u32_reg_walk(hk, sub, 0, NULL);
    if (k < 0) { if (phk) *(uint32_t *)(uintptr_t)phk = 0; return 2; }
    if (phk) *(uint32_t *)(uintptr_t)phk = u32_reg_hkey(k);
    return 0;
}
static uint32_t u32_reg_setval(uint32_t hk, const char *name, uint32_t type, uint32_t data, uint32_t cb) {
    int k = u32_reg_idx(hk); if (k < 0) return 6;
    struct u32_regval *v = u32_reg_findval(k, name, 1); if (!v) return 5;
    if (cb > U32_REGDATA) cb = U32_REGDATA;
    v->type = type; v->len = cb;
    if (data && cb) memcpy(v->data, (const void *)(uintptr_t)data, cb);
    return 0;
}
static uint32_t u32_reg_queryval(uint32_t hk, const char *name, uint32_t ptype, uint32_t data, uint32_t pcb) {
    int k = u32_reg_idx(hk); if (k < 0) return 6;
    struct u32_regval *v = u32_reg_findval(k, name, 0); if (!v) return 2;
    if (ptype) *(uint32_t *)(uintptr_t)ptype = v->type;
    if (!data) { if (pcb) *(uint32_t *)(uintptr_t)pcb = v->len; return 0; }  /* size query */
    if (!pcb) return 87;                                                     /* data w/o size */
    uint32_t *cbp = (uint32_t *)(uintptr_t)pcb;
    if (*cbp < v->len) { *cbp = v->len; return 234; }
    memcpy((void *)(uintptr_t)data, v->data, v->len); *cbp = v->len;
    return 0;
}
uint32_t aret_RegCreateKeyExA(uint32_t esp) { return u32_reg_create(WU(0), WCS(1), WU(7), WU(8)); }
uint32_t aret_RegCreateKeyA(uint32_t esp)   { return u32_reg_create(WU(0), WCS(1), WU(2), 0); }
uint32_t aret_RegOpenKeyExA(uint32_t esp)   { return u32_reg_open(WU(0), WCS(1), WU(4)); }
uint32_t aret_RegOpenKeyA(uint32_t esp)     { return u32_reg_open(WU(0), WCS(1), WU(2)); }
uint32_t aret_RegSetValueExA(uint32_t esp)  { return u32_reg_setval(WU(0), WCS(1), WU(3), WU(4), WU(5)); }
uint32_t aret_RegQueryValueExA(uint32_t esp){ return u32_reg_queryval(WU(0), WCS(1), WU(3), WU(4), WU(5)); }
uint32_t aret_RegCloseKey(uint32_t esp) { (void)esp; return 0; }   /* keys persist in the tree; handle close is a no-op */
uint32_t aret_RegFlushKey(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_RegDeleteValueA(uint32_t esp) {
    int k = u32_reg_idx(WU(0)); if (k < 0) return 6;
    struct u32_regval *v = u32_reg_findval(k, WCS(1), 0); if (!v) return 2;
    v->used = 0; return 0;
}
uint32_t aret_RegDeleteKeyA(uint32_t esp) {
    int k = u32_reg_walk(WU(0), WCS(1), 0, NULL);
    if (k < 0 || k < U32_REG_ROOTS) return 2;
    u32_reg_del_subtree(k); return 0;
}
uint32_t aret_RegEnumValueA(uint32_t esp) {
    int k = u32_reg_idx(WU(0)); if (k < 0) return 6;
    uint32_t idx = WU(1), seen = 0; struct u32_regkey *K = &g_reg[k]; struct u32_regval *v = NULL;
    for (int i = 0; i < U32_MAX_REGVAL; i++) if (K->val[i].used) { if (seen == idx) { v = &K->val[i]; break; } seen++; }
    if (!v) return 259;
    char *name = (char *)WP(2); uint32_t *pnl = (uint32_t *)WP(3);
    uint32_t *ptype = (uint32_t *)WP(5); uint8_t *data = (uint8_t *)WP(6); uint32_t *pcb = (uint32_t *)WP(7);
    uint32_t nlen = (uint32_t)strlen(v->name);
    if (name && pnl) { if (*pnl < nlen + 1) return 234; memcpy(name, v->name, nlen + 1); *pnl = nlen; }
    if (ptype) *ptype = v->type;
    if (data && pcb) { if (*pcb < v->len) { *pcb = v->len; return 234; } memcpy(data, v->data, v->len); *pcb = v->len; }
    else if (pcb) *pcb = v->len;
    return 0;
}
static uint32_t u32_reg_enumkey(uint32_t hk, uint32_t idx, char *name, uint32_t namecap) {
    int k = u32_reg_idx(hk); if (k < 0) return 6;
    uint32_t seen = 0; int child = -1;
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++) if (g_reg[i].used && g_reg[i].parent == k) { if (seen == idx) { child = i; break; } seen++; }
    if (child < 0) return 259;
    uint32_t nlen = (uint32_t)strlen(g_reg[child].name);
    if (name) { if (namecap < nlen + 1) return 234; memcpy(name, g_reg[child].name, nlen + 1); }
    return (nlen << 24) | 0u;   /* low byte 0 = success; caller writes namelen separately below */
}
uint32_t aret_RegEnumKeyExA(uint32_t esp) {
    /* (hKey, dwIndex, lpName, lpcchName, Reserved, lpClass, lpcchClass, lpftLastWrite) */
    char *name = (char *)WP(2); uint32_t *pnl = (uint32_t *)WP(3);
    uint32_t cap = pnl ? *pnl : 0;
    uint32_t r = u32_reg_enumkey(WU(0), WU(1), name, cap);
    if ((r & 0xFFu) != 0) return r & 0xFFu;
    if (pnl) *pnl = r >> 24;
    return 0;
}
uint32_t aret_RegEnumKeyA(uint32_t esp) {
    /* (hKey, dwIndex, lpName, cbName) — old form; cbName in bytes */
    uint32_t r = u32_reg_enumkey(WU(0), WU(1), (char *)WP(2), WU(3));
    return (r & 0xFFu);
}
uint32_t aret_RegQueryInfoKeyA(uint32_t esp) {
    int k = u32_reg_idx(WU(0)); if (k < 0) return 6;
    uint32_t nsub = 0, maxsub = 0, nval = 0, maxvn = 0, maxvl = 0;
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++) if (g_reg[i].used && g_reg[i].parent == k) {
        nsub++; uint32_t l = (uint32_t)strlen(g_reg[i].name); if (l > maxsub) maxsub = l;
    }
    struct u32_regkey *K = &g_reg[k];
    for (int i = 0; i < U32_MAX_REGVAL; i++) if (K->val[i].used) {
        nval++; uint32_t l = (uint32_t)strlen(K->val[i].name); if (l > maxvn) maxvn = l; if (K->val[i].len > maxvl) maxvl = K->val[i].len;
    }
    uint32_t *p;
    if ((p = (uint32_t *)WP(2)))  *p = 0;       /* lpcchClass */
    if ((p = (uint32_t *)WP(4)))  *p = nsub;    /* lpcSubKeys */
    if ((p = (uint32_t *)WP(5)))  *p = maxsub;  /* lpcbMaxSubKeyLen */
    if ((p = (uint32_t *)WP(6)))  *p = 0;       /* lpcbMaxClassLen */
    if ((p = (uint32_t *)WP(7)))  *p = nval;    /* lpcValues */
    if ((p = (uint32_t *)WP(8)))  *p = maxvn;   /* lpcbMaxValueNameLen */
    if ((p = (uint32_t *)WP(9)))  *p = maxvl;   /* lpcbMaxValueLen */
    if ((p = (uint32_t *)WP(10))) *p = 0;       /* lpcbSecurityDescriptor */
    return 0;
}
/* W variants: convert the sub-key / value name to narrow, then share the A cores
 * (value data is stored/returned verbatim, so REG_SZ set via W and read via W agrees;
 * mixing A/W on one value is out of the proven subset). */
uint32_t aret_RegCreateKeyExW(uint32_t esp) { char s[256]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_create(WU(0), s, WU(7), WU(8)); }
uint32_t aret_RegCreateKeyW(uint32_t esp)   { char s[256]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_create(WU(0), s, WU(2), 0); }
uint32_t aret_RegOpenKeyExW(uint32_t esp)   { char s[256]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_open(WU(0), s, WU(4)); }
uint32_t aret_RegOpenKeyW(uint32_t esp)     { char s[256]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_open(WU(0), s, WU(2)); }
uint32_t aret_RegSetValueExW(uint32_t esp)  { char s[U32_REGNAME]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_setval(WU(0), s, WU(3), WU(4), WU(5)); }
uint32_t aret_RegQueryValueExW(uint32_t esp){ char s[U32_REGNAME]; u32_w2n((const uint16_t *)WP(1), s, sizeof s); return u32_reg_queryval(WU(0), s, WU(3), WU(4), WU(5)); }
uint32_t aret_RegDeleteValueW(uint32_t esp) {
    int k = u32_reg_idx(WU(0)); if (k < 0) return 6;
    char s[U32_REGNAME]; u32_w2n((const uint16_t *)WP(1), s, sizeof s);
    struct u32_regval *v = u32_reg_findval(k, s, 0); if (!v) return 2; v->used = 0; return 0;
}
uint32_t aret_RegDeleteKeyW(uint32_t esp) {
    char s[256]; u32_w2n((const uint16_t *)WP(1), s, sizeof s);
    int k = u32_reg_walk(WU(0), s, 0, NULL);
    if (k < 0 || k < U32_REG_ROOTS) return 2;
    u32_reg_del_subtree(k); return 0;
}

/* ---- ntdll Nt* registry: the syscall floor beneath advapi32 Reg*, on the SAME g_reg tree.
 * ARET's registry is empty by design (sound, §0): a system key that was never written this run
 * is absent, so Nt* reads of pre-existing keys return STATUS_OBJECT_NAME_NOT_FOUND — a round-trip
 * (create -> set -> query) is what matches Wine, exactly like the Reg* model. Nt handles are the
 * same opaque values as HKEY (u32_reg_hkey). Measured vs Wine (win32_ntreg). ---- */
#define NT_STATUS_SUCCESS               0u
#define NT_STATUS_INVALID_HANDLE        0xC0000008u
#define NT_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define NT_STATUS_BUFFER_OVERFLOW       0x80000005u
#define NT_STATUS_BUFFER_TOO_SMALL      0xC0000023u

/* Narrow a UNICODE_STRING* (Length@0 bytes, Buffer@4) into an ASCII buffer. */
static void u32_us_narrow(uint32_t pus, char *out, int cap) {
    out[0] = 0;
    if (!pus) return;
    uint16_t len = *(const uint16_t *)(uintptr_t)pus;             /* bytes */
    const uint16_t *w = (const uint16_t *)(uintptr_t)*(const uint32_t *)(uintptr_t)(pus + 4);
    int n = len / 2, i = 0;
    if (w) for (; i < n && i < cap - 1; i++) out[i] = (char)(w[i] & 0xFF);
    out[i] = 0;
}
/* Resolve an OBJECT_ATTRIBUTES* (RootDirectory@4, ObjectName@8) to a g_reg key index. An
 * absolute name starts "\Registry\Machine|User\..."; a relative name walks from RootDirectory. */
static int u32_nt_reg_resolve(uint32_t poa, int create, uint32_t *disp) {
    if (!poa) return -1;
    uint32_t root = *(const uint32_t *)(uintptr_t)(poa + 4);
    uint32_t pname = *(const uint32_t *)(uintptr_t)(poa + 8);
    char name[512]; u32_us_narrow(pname, name, sizeof name);
    if (root) return u32_reg_walk(root, name, create, disp);
    const char *p = name;
    if (*p == '\\') p++;
    if (!strncasecmp(p, "Registry", 8)) { p += 8; while (*p == '\\') p++; }
    uint32_t hive;
    if (!strncasecmp(p, "Machine", 7)) { hive = 0x80000002u; p += 7; }
    else if (!strncasecmp(p, "User", 4)) { hive = 0x80000003u; p += 4; }
    else return -1;                       /* unmodelled hive -> caller returns NOT_FOUND */
    while (*p == '\\') p++;
    return u32_reg_walk(hive, p, create, disp);
}
/* ---- Real-ABI registry cores (tranche 5): the SAME g_reg logic the esp shims run, exposed as
 * ordinary (non-static) functions so the wine_heavy floor's NTAPI Nt* wrappers -- i.e. a COMPILED
 * Wine ntdll .c -- route here. Every argument is a 32-bit address in the shared address space, so
 * a pointer off the emulated stack and a real pointer from compiled floor code are identical.
 * The esp shims below are now thin unpackers over these cores (single source of truth). ---- */
uint32_t aret_ntreg_create(uint32_t poa, uint32_t phkey, uint32_t pdisp) {
    uint32_t disp = 0; int k = u32_nt_reg_resolve(poa, 1, &disp);
    if (k < 0) return NT_STATUS_OBJECT_NAME_NOT_FOUND;
    if (phkey) *(uint32_t *)(uintptr_t)phkey = u32_reg_hkey(k);
    if (pdisp) *(uint32_t *)(uintptr_t)pdisp = disp;   /* REG_CREATED_NEW_KEY(1)/OPENED_EXISTING(2) */
    return NT_STATUS_SUCCESS;
}
uint32_t aret_ntreg_open(uint32_t poa, uint32_t phkey) {
    int k = u32_nt_reg_resolve(poa, 0, NULL);
    if (k < 0) return NT_STATUS_OBJECT_NAME_NOT_FOUND;
    if (phkey) *(uint32_t *)(uintptr_t)phkey = u32_reg_hkey(k);
    return NT_STATUS_SUCCESS;
}
uint32_t aret_ntreg_setval(uint32_t hkey, uint32_t pvalname, uint32_t type, uint32_t data, uint32_t size) {
    char vn[256]; u32_us_narrow(pvalname, vn, sizeof vn);
    return u32_reg_setval(hkey, vn, type, data, size) == 0 ? NT_STATUS_SUCCESS : NT_STATUS_INVALID_HANDLE;
}
uint32_t aret_ntreg_queryval(uint32_t hkey, uint32_t pvalname, uint32_t infoclass, uint32_t info, uint32_t length, uint32_t presult) {
    /* Only KeyValuePartialInformation(2) modelled: { TitleIndex, Type, DataLength, Data[] }. */
    if (infoclass != 2) { aret_partial("NtQueryValueKey: only KeyValuePartialInformation modelled"); return NT_STATUS_INVALID_HANDLE; }
    int k = u32_reg_idx(hkey);
    if (k < 0) return NT_STATUS_INVALID_HANDLE;
    char vn[256]; u32_us_narrow(pvalname, vn, sizeof vn);
    struct u32_regval *v = u32_reg_findval(k, vn, 0);
    if (!v) return NT_STATUS_OBJECT_NAME_NOT_FOUND;
    uint32_t need = 12 + v->len;                       /* 3 ULONG header + data */
    if (presult) *(uint32_t *)(uintptr_t)presult = need;
    if (length < 12) return NT_STATUS_BUFFER_TOO_SMALL;    /* not even the header fits */
    uint32_t *pi = (uint32_t *)(uintptr_t)info;
    pi[0] = 0; pi[1] = v->type; pi[2] = v->len;
    uint32_t cap = length - 12, copy = v->len < cap ? v->len : cap;
    if (copy) memcpy((uint8_t *)(uintptr_t)info + 12, v->data, copy);
    return copy < v->len ? NT_STATUS_BUFFER_OVERFLOW : NT_STATUS_SUCCESS;
}
uint32_t aret_ntreg_delval(uint32_t hkey, uint32_t pvalname) {
    int k = u32_reg_idx(hkey); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    char vn[256]; u32_us_narrow(pvalname, vn, sizeof vn);
    struct u32_regval *v = u32_reg_findval(k, vn, 0);
    if (!v) return NT_STATUS_OBJECT_NAME_NOT_FOUND;
    v->used = 0; return NT_STATUS_SUCCESS;
}
uint32_t aret_NtCreateKey(uint32_t esp) {
    /* (KeyHandle@0, DesiredAccess@1, ObjectAttributes@2, TitleIndex@3, Class@4, Options@5, Disposition@6) */
    return aret_ntreg_create(WU(2), WU(0), WU(6));
}
uint32_t aret_NtOpenKey(uint32_t esp) {
    /* (KeyHandle@0, DesiredAccess@1, ObjectAttributes@2) */
    return aret_ntreg_open(WU(2), WU(0));
}
uint32_t aret_NtSetValueKey(uint32_t esp) {
    /* (KeyHandle@0, ValueName@1, TitleIndex@2, Type@3, Data@4, DataSize@5) */
    return aret_ntreg_setval(WU(0), WU(1), WU(3), WU(4), WU(5));
}
uint32_t aret_NtQueryValueKey(uint32_t esp) {
    /* (KeyHandle@0, ValueName@1, InfoClass@2, Info@3, Length@4, ResultLength@5) */
    return aret_ntreg_queryval(WU(0), WU(1), WU(2), WU(3), WU(4), WU(5));
}
uint32_t aret_NtDeleteValueKey(uint32_t esp) {
    /* (KeyHandle@0, ValueName@1) */
    return aret_ntreg_delval(WU(0), WU(1));
}
/* NtClose is generic (keys/files/events). A file fd we opened via the Nt* file layer is really
 * closed (and its pending delete-on-close honored) by aret_ntfile_close; a registry key / event /
 * other tagged handle isn't a tracked fd, so that returns 0 and we no-op (keys persist in g_reg,
 * like RegCloseKey/CloseHandle). */
uint32_t aret_NtClose(uint32_t esp) { aret_ntfile_close(WU(0)); return NT_STATUS_SUCCESS; }

/* ---- Nt* registry tranche 2: enumeration / info (NtQueryKey/NtEnumerateKey/
 * NtEnumerateValueKey/NtFlushKey/NtDeleteKey), same g_reg tree. Struct layouts and buffer/
 * ordering semantics MEASURED vs Wine (scratchpad ntenum_probe):
 *   - MaxNameLen/MaxValueNameLen are in BYTES (chars*2), MaxValueDataLen in bytes;
 *   - out-of-range index -> STATUS_NO_MORE_ENTRIES;
 *   - key-info (Query/EnumerateKey): len<fixed -> BUFFER_TOO_SMALL, fixed<=len<need -> BUFFER_OVERFLOW;
 *   - value-ENUM (EnumerateValueKey): any len<need -> BUFFER_OVERFLOW (no TOO_SMALL regime) --
 *     genuinely a different Wine code path from NtQueryValueKey (which keeps the TOO_SMALL regime);
 *   - Wine enumerates subkeys AND values in case-insensitive (upcased) SORTED order, not
 *     creation order -> we sort by an upcased-ASCII compare (bit-identical Wine on the proven
 *     ASCII subset; names are stored narrow). LastWriteTime is environmental -> written 0,
 *     the fixture excludes it. ---- */
#define NT_STATUS_NO_MORE_ENTRIES 0x8000001Au

/* Widen an ASCII key/value name to the UTF-16 the Nt* structs return (round-trip exact in the
 * proven ASCII subset). Returns the number of WCHARs written (no NUL). */
static int u32_reg_widen(const char *s, uint16_t *out, int cap) {
    int i = 0; for (; s[i] && i < cap; i++) out[i] = (uint16_t)(uint8_t)s[i]; return i;
}
/* Case-insensitive compare the way Wine orders the registry: upcase ASCII 'a'-'z' -> 'A'-'Z'
 * (so '_'(0x5F) sorts AFTER letters, matching RtlCompareUnicodeString case-insensitive, unlike
 * a tolower compare). */
static int u32_reg_ncmp(const char *a, const char *b) {
    for (;; a++, b++) {
        unsigned ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
}
/* The idx-th live subkey of key k in Wine's SORTED order -> child index, or -1. */
static int u32_reg_nth_subkey(int k, uint32_t idx) {
    int ids[U32_MAX_REGKEY]; int n = 0;
    for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++)
        if (g_reg[i].used && g_reg[i].parent == k) ids[n++] = i;
    for (int a = 1; a < n; a++) { int t = ids[a], b = a - 1;
        while (b >= 0 && u32_reg_ncmp(g_reg[ids[b]].name, g_reg[t].name) > 0) { ids[b + 1] = ids[b]; b--; }
        ids[b + 1] = t; }
    return idx < (uint32_t)n ? ids[idx] : -1;
}
/* The idx-th live value of key k in Wine's SORTED order -> val ptr, or NULL. */
static struct u32_regval *u32_reg_nth_value(int k, uint32_t idx) {
    struct u32_regkey *K = &g_reg[k];
    int ids[U32_MAX_REGVAL]; int n = 0;
    for (int i = 0; i < U32_MAX_REGVAL; i++) if (K->val[i].used) ids[n++] = i;
    for (int a = 1; a < n; a++) { int t = ids[a], b = a - 1;
        while (b >= 0 && u32_reg_ncmp(K->val[ids[b]].name, K->val[t].name) > 0) { ids[b + 1] = ids[b]; b--; }
        ids[b + 1] = t; }
    return idx < (uint32_t)n ? &K->val[ids[idx]] : NULL;
}
/* Fill a KEY_BASIC/NODE/FULL_INFORMATION for key kidx into info[0..length). LastWriteTime=0
 * (environmental). Shared by NtQueryKey (self) and NtEnumerateKey (a subkey). */
static uint32_t u32_fill_key_info(uint32_t cls, int kidx, uint32_t info, uint32_t length, uint32_t presult) {
    uint8_t *o = (uint8_t *)(uintptr_t)info;
    uint16_t wn[U32_REGNAME]; int wl = u32_reg_widen(g_reg[kidx].name, wn, U32_REGNAME);
    uint32_t namelen = 2u * (uint32_t)wl;
    uint32_t fixed, need, noff = 0;
    if (cls == 0)      { fixed = 16; noff = 16; need = 16 + namelen; }   /* KeyBasicInformation */
    else if (cls == 1) { fixed = 24; noff = 24; need = 24 + namelen; }   /* KeyNodeInformation  */
    else if (cls == 2) { fixed = 44;            need = 44;           }   /* KeyFullInformation  */
    else { aret_partial("NtQueryKey/NtEnumerateKey: only KeyBasic/Node/FullInformation modelled"); return NT_STATUS_INVALID_HANDLE; }
    if (presult) *(uint32_t *)(uintptr_t)presult = need;
    if (length < fixed) return NT_STATUS_BUFFER_TOO_SMALL;
    if (cls == 2) {                              /* Full: fixed-only, no variable part */
        uint32_t nsub = 0, maxname = 0, nval = 0, maxvname = 0, maxvdata = 0;
        for (int i = U32_REG_ROOTS; i < U32_MAX_REGKEY; i++) if (g_reg[i].used && g_reg[i].parent == kidx) {
            nsub++; uint32_t l = 2u * (uint32_t)strlen(g_reg[i].name); if (l > maxname) maxname = l; }
        struct u32_regkey *K = &g_reg[kidx];
        for (int i = 0; i < U32_MAX_REGVAL; i++) if (K->val[i].used) {
            nval++; uint32_t l = 2u * (uint32_t)strlen(K->val[i].name); if (l > maxvname) maxvname = l;
            if (K->val[i].len > maxvdata) maxvdata = K->val[i].len; }
        memset(o, 0, 44);
        *(uint32_t *)(o + 12) = 0xFFFFFFFFu;     /* ClassOffset (no class) */
        *(uint32_t *)(o + 20) = nsub;   *(uint32_t *)(o + 24) = maxname;
        *(uint32_t *)(o + 32) = nval;   *(uint32_t *)(o + 36) = maxvname; *(uint32_t *)(o + 40) = maxvdata;
        return NT_STATUS_SUCCESS;
    }
    memset(o, 0, fixed);
    if (cls == 0)      { *(uint32_t *)(o + 12) = namelen; }
    else /* node */    { *(uint32_t *)(o + 12) = 0xFFFFFFFFu; *(uint32_t *)(o + 20) = namelen; }
    uint32_t avail = length - noff, cpy = namelen < avail ? namelen : avail;
    if (cpy) memcpy(o + noff, wn, cpy);          /* partial-name write on overflow is contract-undefined */
    return cpy < namelen ? NT_STATUS_BUFFER_OVERFLOW : NT_STATUS_SUCCESS;
}
/* Fill a KEY_VALUE_BASIC/FULL/PARTIAL_INFORMATION for value v (Enumerate path: any short buffer
 * -> BUFFER_OVERFLOW, no TOO_SMALL). */
static uint32_t u32_fill_value_info_enum(uint32_t cls, struct u32_regval *v, uint32_t info, uint32_t length, uint32_t presult) {
    uint8_t *o = (uint8_t *)(uintptr_t)info;
    uint16_t wn[U32_REGNAME]; int wl = u32_reg_widen(v->name, wn, U32_REGNAME);
    uint32_t namelen = 2u * (uint32_t)wl, dataoff = 0, need;
    if (cls == 0)      { need = 12 + namelen; }                          /* KeyValueBasicInformation   */
    else if (cls == 1) { dataoff = 20 + namelen; need = dataoff + v->len; } /* KeyValueFullInformation */
    else if (cls == 2) { need = 12 + v->len; }                          /* KeyValuePartialInformation */
    else { aret_partial("NtEnumerateValueKey: only KeyValueBasic/Full/PartialInformation modelled"); return NT_STATUS_INVALID_HANDLE; }
    if (presult) *(uint32_t *)(uintptr_t)presult = need;
    if (length < need) return NT_STATUS_BUFFER_OVERFLOW;                 /* enum: no TOO_SMALL regime */
    if (cls == 0) {                              /* TitleIndex, Type, NameLength, Name */
        *(uint32_t *)(o + 0) = 0; *(uint32_t *)(o + 4) = v->type; *(uint32_t *)(o + 8) = namelen;
        if (namelen) memcpy(o + 12, wn, namelen);
    } else if (cls == 1) {                        /* + DataOffset, DataLength, Name, Data */
        *(uint32_t *)(o + 0) = 0; *(uint32_t *)(o + 4) = v->type; *(uint32_t *)(o + 8) = dataoff;
        *(uint32_t *)(o + 12) = v->len; *(uint32_t *)(o + 16) = namelen;
        if (namelen) memcpy(o + 20, wn, namelen);
        if (v->len) memcpy(o + dataoff, v->data, v->len);
    } else {                                      /* Partial: TitleIndex, Type, DataLength, Data */
        *(uint32_t *)(o + 0) = 0; *(uint32_t *)(o + 4) = v->type; *(uint32_t *)(o + 8) = v->len;
        if (v->len) memcpy(o + 12, v->data, v->len);
    }
    return NT_STATUS_SUCCESS;
}
uint32_t aret_NtQueryKey(uint32_t esp) {
    /* (KeyHandle@0, KeyInformationClass@1, KeyInformation@2, Length@3, ResultLength@4) */
    int k = u32_reg_idx(WU(0)); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    return u32_fill_key_info(WU(1), k, WU(2), WU(3), WU(4));
}
uint32_t aret_NtEnumerateKey(uint32_t esp) {
    /* (KeyHandle@0, Index@1, KeyInformationClass@2, KeyInformation@3, Length@4, ResultLength@5) */
    int k = u32_reg_idx(WU(0)); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    int child = u32_reg_nth_subkey(k, WU(1));
    if (child < 0) return NT_STATUS_NO_MORE_ENTRIES;
    return u32_fill_key_info(WU(2), child, WU(3), WU(4), WU(5));
}
uint32_t aret_NtEnumerateValueKey(uint32_t esp) {
    /* (KeyHandle@0, Index@1, KeyValueInformationClass@2, KeyValueInformation@3, Length@4, ResultLength@5) */
    int k = u32_reg_idx(WU(0)); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    struct u32_regval *v = u32_reg_nth_value(k, WU(1));
    if (!v) return NT_STATUS_NO_MORE_ENTRIES;
    return u32_fill_value_info_enum(WU(2), v, WU(3), WU(4), WU(5));
}
uint32_t aret_NtFlushKey(uint32_t esp) {
    /* (KeyHandle@0) — in-memory tree, flush is a no-op like RegFlushKey; validate the handle. */
    int k = u32_reg_idx(WU(0)); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    return NT_STATUS_SUCCESS;
}
uint32_t aret_NtDeleteKey(uint32_t esp) {
    /* (KeyHandle@0) — delete the key this handle refers to (subtree), like RegDeleteKey by handle. */
    int k = u32_reg_idx(WU(0)); if (k < 0) return NT_STATUS_INVALID_HANDLE;
    if (k < U32_REG_ROOTS) return NT_STATUS_INVALID_HANDLE;   /* a hive root is not deletable */
    u32_reg_del_subtree(k); return NT_STATUS_SUCCESS;
}

/* ExpandEnvironmentStringsA(src, dst, size): substitute %NAME% with getenv(NAME),
 * copy literals through. Returns the length written including the NUL (or the
 * required size if dst is too small / NULL), matching the Win32 contract. */
uint32_t aret_ExpandEnvironmentStringsA(uint32_t esp) {
    const char *src = WCS(0); char *dst = WS(1); uint32_t size = WU(2);
    if (!src) return 0;
    char out[4096]; size_t o = 0;
    for (const char *p = src; *p && o < sizeof out - 1;) {
        const char *e;
        if (*p == '%' && (e = strchr(p + 1, '%'))) {
            size_t nl = (size_t)(e - p - 1);
            char name[256];
            if (nl < sizeof name) {
                memcpy(name, p + 1, nl); name[nl] = 0;
                const char *v = getenv(name);
                if (v) { size_t vl = strlen(v); if (o + vl < sizeof out - 1) { memcpy(out + o, v, vl); o += vl; } }
                p = e + 1; continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    uint32_t need = (uint32_t)o + 1;
    if (dst && size >= need) memcpy(dst, out, need);
    return need;
}
/* GetFullPathNameA(name, len, buf, *filePart) -> length. Resolves against the
 * cwd; sets *filePart to the last component within buf. The full prefix (cwd)
 * differs by host, but callers use filePart and the basename, which are stable. */
uint32_t aret_GetFullPathNameA(uint32_t esp) {
    const char *name = WCS(0);
    uint32_t buflen = WU(1);
    char *buf = WS(2);
    uint32_t *filepart = (uint32_t *)(uintptr_t)WU(3);
    char tmp[4096];
    if (name && (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':'))) {
        snprintf(tmp, sizeof tmp, "%s", name);
    } else {
        char cwd[2048];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, name ? name : "");
    }
    uint32_t len = (uint32_t)strlen(tmp);
    if (!buf || buflen <= len) return len + 1; /* required size incl. NUL */
    memcpy(buf, tmp, len + 1);
    if (filepart) {
        char *sep = NULL;
        for (char *p = buf; *p; p++) if (*p == '/' || *p == '\\') sep = p;
        *filepart = (uint32_t)(uintptr_t)(sep ? sep + 1 : buf);
    }
    return len;
}
uint32_t aret_GetCurrentDirectoryA(uint32_t esp) {
    uint32_t size = WU(0); char *buf = WS(1);
    char tmp[4096];
    if (!getcwd(tmp, sizeof tmp)) return 0;
    uint32_t len = (uint32_t)strlen(tmp);
    if (buf && size > len) { memcpy(buf, tmp, len + 1); return len; }
    return len + 1;
}
uint32_t aret_SetCurrentDirectoryA(uint32_t esp) { return (uint32_t)(chdir(WCS(0)) == 0); }
uint32_t aret_GetTempPathA(uint32_t esp) {
    uint32_t size = WU(0); char *buf = WS(1);
    const char *e = getenv("TMPDIR"); if (!e || !*e) e = "/tmp";
    char t[4096]; size_t len = strlen(e);
    if (len + 2 > sizeof t) return 0;
    memcpy(t, e, len);
    if (t[len - 1] != '/') t[len++] = '/';   /* Windows GetTempPath always ends in a sep */
    t[len] = 0;
    if (buf && size > len) { memcpy(buf, t, len + 1); return (uint32_t)len; }
    return (uint32_t)len + 1;
}

/* OutputDebugStringA — route debug text to stderr. */
uint32_t aret_OutputDebugStringA(uint32_t esp) {
    const char *s = WCS(0);
    if (s) { size_t n = strlen(s); ssize_t w = write(2, s, n); (void)w; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* lstr* — kernel32's string helpers are just libc str*                */
/* ------------------------------------------------------------------ */

uint32_t aret_lstrlenA(uint32_t esp) { const char *s = WCS(0); return s ? (uint32_t)strlen(s) : 0; }
uint32_t aret_lstrcpyA(uint32_t esp) { return WRP(strcpy(WS(0), WCS(1))); }
uint32_t aret_lstrcatA(uint32_t esp) { return WRP(strcat(WS(0), WCS(1))); }
uint32_t aret_lstrcmpA(uint32_t esp) { return (uint32_t)(int32_t)strcmp(WCS(0), WCS(1)); }
uint32_t aret_lstrcmpiA(uint32_t esp){ return (uint32_t)(int32_t)strcasecmp(WCS(0), WCS(1)); }
/* Wide (16-bit) kernel32 string length/copy/cat — ordinal, exact. */
uint32_t aret_lstrlenW(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)WP(0);
    if (!s) return 0;
    uint32_t n = 0; while (s[n]) n++; return n;
}
uint32_t aret_lstrcpyW(uint32_t esp) {
    uint16_t *d = (uint16_t *)WP(0); const uint16_t *s = (const uint16_t *)WP(1);
    if (d && s) while ((*d++ = *s++)) {}
    return WU(0);
}
uint32_t aret_lstrcatW(uint32_t esp) {
    uint16_t *d = (uint16_t *)WP(0); const uint16_t *s = (const uint16_t *)WP(1);
    if (d && s) { while (*d) d++; while ((*d++ = *s++)) {} }
    return WU(0);
}

/* ------------------------------------------------------------------ */
/* Linguistic string comparison (CompareStringW / lstrcmpW / lstrcmpiW). */
/* ------------------------------------------------------------------ */
/* Windows compares strings *linguistically* (word sort), NOT ordinally: uppercase
 * sorts after lowercase, digits before letters, etc. The result equals the byte
 * comparison of the Windows "sort key" (LCMAP_SORTKEY). We reproduce the sort key
 * BIT-FOR-BIT for a proven ASCII subset — the per-character weights below were
 * MEASURED from Wine's LCMAP_SORTKEY (not guessed) — and ABORT for any character
 * outside it (control chars, the primary-ignorable '-'/'\'', non-ASCII), so we are
 * never silently wrong. Structure: PRI(2 bytes/char) 01 01 CASE(0x12 upper/0x02
 * else, trailing-0x02 trimmed; dropped when case-insensitive) 01 01 00. */
static const uint16_t u32_pri[128] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0702, 0x071c, 0x071d, 0x071f, 0x0721, 0x0723, 0x0725, 0x0000,
    0x0727, 0x072a, 0x072d, 0x0803, 0x072f, 0x0000, 0x0733, 0x0735,
    0x0d03, 0x0d1a, 0x0d1c, 0x0d1e, 0x0d20, 0x0d22, 0x0d24, 0x0d26,
    0x0d28, 0x0d2a, 0x0737, 0x073a, 0x080e, 0x0812, 0x0814, 0x073c,
    0x073e, 0x0e02, 0x0e09, 0x0e0a, 0x0e1a, 0x0e21, 0x0e23, 0x0e25,
    0x0e2c, 0x0e32, 0x0e35, 0x0e36, 0x0e48, 0x0e51, 0x0e70, 0x0e7c,
    0x0e7e, 0x0e89, 0x0e8a, 0x0e91, 0x0e99, 0x0e9f, 0x0ea2, 0x0ea4,
    0x0ea6, 0x0ea7, 0x0ea9, 0x073f, 0x0741, 0x0742, 0x0743, 0x0744,
    0x0748, 0x0e02, 0x0e09, 0x0e0a, 0x0e1a, 0x0e21, 0x0e23, 0x0e25,
    0x0e2c, 0x0e32, 0x0e35, 0x0e36, 0x0e48, 0x0e51, 0x0e70, 0x0e7c,
    0x0e7e, 0x0e89, 0x0e8a, 0x0e91, 0x0e99, 0x0e9f, 0x0ea2, 0x0ea4,
    0x0ea6, 0x0ea7, 0x0ea9, 0x074a, 0x074c, 0x074e, 0x0750, 0x0000,
};
/* Primary-ignorable punctuation: contributes nothing to the primary/case levels,
 * only a 4-byte entry to the special level `ff (0xff-nBefore) <weight> 12` (weights
 * measured from Wine). 0 = not ignorable. */
static uint8_t u32_ign(uint16_t c) { return c == '\'' ? 0x80 : c == '-' ? 0x82 : 0; }
/* Build the sort key for `len` code units of `s` (len<0 = until NUL). Returns the
 * key length, or -1 if any unit is outside the proven subset. Structure:
 * PRI 01 01 CASE 01 01 SPECIAL 00 (CASE dropped when case-insensitive). */
static int u32_sortkey(uint8_t *dst, const uint16_t *s, int len, int ignore_case) {
    uint8_t pri[1024], cas[512], spc[1024]; int np = 0, nc = 0, ns = 0, nbefore = 0;
    for (int i = 0; (len < 0) ? (s[i] != 0) : (i < len); i++) {
        uint16_t c = s[i];
        if (len >= 0 && c == 0) break;
        uint8_t iw = u32_ign(c);
        if (iw) {                                        /* primary-ignorable -> special level */
            if (ns > 1018 || nbefore >= 0x80) return -1; /* position byte stays in proven range */
            spc[ns++] = 0xff; spc[ns++] = (uint8_t)(0xff - nbefore); spc[ns++] = iw; spc[ns++] = 0x12;
            continue;
        }
        if (c >= 128 || u32_pri[c] == 0) return -1;      /* unmodelled -> abort */
        if (np > 1020 || nc > 508) return -1;            /* too long -> abort */
        uint16_t w = u32_pri[c];
        pri[np++] = (uint8_t)(w >> 8); pri[np++] = (uint8_t)w;
        cas[nc++] = (c >= 'A' && c <= 'Z') ? 0x12 : 0x02;
        nbefore++;
    }
    if (ignore_case) nc = 0;
    else while (nc > 0 && cas[nc - 1] == 0x02) nc--;     /* trim trailing default case */
    int o = 0;
    for (int i = 0; i < np; i++) dst[o++] = pri[i];
    dst[o++] = 0x01; dst[o++] = 0x01;
    for (int i = 0; i < nc; i++) dst[o++] = cas[i];
    dst[o++] = 0x01; dst[o++] = 0x01;
    for (int i = 0; i < ns; i++) dst[o++] = spc[i];
    dst[o++] = 0x00;
    return o;
}
/* Linguistic compare -> -1/0/1, or -2 = unmodelled (caller aborts sound). */
static int u32_collate(const uint16_t *a, int na, const uint16_t *b, int nb, int ignore_case) {
    uint8_t ka[2600], kb[2600];
    int la = u32_sortkey(ka, a, na, ignore_case);
    int lb = u32_sortkey(kb, b, nb, ignore_case);
    if (la < 0 || lb < 0) return -2;
    int m = la < lb ? la : lb;
    int c = memcmp(ka, kb, (size_t)m);
    if (c == 0) c = la - lb;
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}
/* Binary-identical strings are linguistically equal for ANY content, so this fast
 * path lets equality checks succeed even on strings we could not collate. */
static int u32_wstr_eq(const uint16_t *a, const uint16_t *b) {
    int i = 0; while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
uint32_t aret_lstrcmpW(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)WP(0), *b = (const uint16_t *)WP(1);
    if (!a || !b) return (uint32_t)(int32_t)(a == b ? 0 : a ? 1 : -1);
    if (u32_wstr_eq(a, b)) return 0;
    int r = u32_collate(a, -1, b, -1, 0);
    if (r == -2) { aret_unmodelled("lstrcmpW: unmodelled linguistic collation (non-ASCII / control char)"); return 0; }
    return (uint32_t)(int32_t)r;
}
uint32_t aret_lstrcmpiW(uint32_t esp) {
    const uint16_t *a = (const uint16_t *)WP(0), *b = (const uint16_t *)WP(1);
    if (!a || !b) return (uint32_t)(int32_t)(a == b ? 0 : a ? 1 : -1);
    if (u32_wstr_eq(a, b)) return 0;
    int r = u32_collate(a, -1, b, -1, 1);
    if (r == -2) { aret_unmodelled("lstrcmpiW: unmodelled linguistic collation"); return 0; }
    return (uint32_t)(int32_t)r;
}
/* CompareStringW(LCID, dwFlags, s1, cch1, s2, cch2) -> CSTR_LESS(1)/EQUAL(2)/
 * GREATER(3), 0 on failure. Only NORM_IGNORECASE (0x1) is modelled; other flags
 * change the collation -> abort. */
uint32_t aret_CompareStringW(uint32_t esp) {
    uint32_t flags = WU(1);
    const uint16_t *s1 = (const uint16_t *)WP(2); int n1 = WI(3);
    const uint16_t *s2 = (const uint16_t *)WP(4); int n2 = WI(5);
    if (flags & ~0x00000001u) { aret_unmodelled("CompareStringW: unmodelled flags"); return 0; }
    if (!s1 || !s2) return 0;
    if (n1 < 0 && n2 < 0 && !(flags & 1) && u32_wstr_eq(s1, s2)) return 2;
    int r = u32_collate(s1, n1 < 0 ? -1 : n1, s2, n2 < 0 ? -1 : n2, flags & 1);
    if (r == -2) { aret_unmodelled("CompareStringW: unmodelled linguistic collation"); return 0; }
    return (uint32_t)(r < 0 ? 1 : r > 0 ? 3 : 2);
}
/* CompareStringA — widen the ASCII bytes and defer to the wide collation (abort on
 * a high byte, which is codepage-dependent). */
uint32_t aret_CompareStringA(uint32_t esp) {
    uint32_t flags = WU(1);
    const uint8_t *s1 = (const uint8_t *)WP(2); int n1 = WI(3);
    const uint8_t *s2 = (const uint8_t *)WP(4); int n2 = WI(5);
    if (flags & ~0x00000001u) { aret_unmodelled("CompareStringA: unmodelled flags"); return 0; }
    if (!s1 || !s2) return 0;
    uint16_t w1[1200], w2[1200]; int m1 = 0, m2 = 0;
    for (int i = 0; (n1 < 0) ? (s1[i] != 0) : (i < n1); i++) {
        if (s1[i] >= 0x80 || m1 >= 1199) { aret_unmodelled("CompareStringA: non-ASCII (codepage)"); return 0; }
        w1[m1++] = s1[i];
    }
    for (int i = 0; (n2 < 0) ? (s2[i] != 0) : (i < n2); i++) {
        if (s2[i] >= 0x80 || m2 >= 1199) { aret_unmodelled("CompareStringA: non-ASCII (codepage)"); return 0; }
        w2[m2++] = s2[i];
    }
    int r = u32_collate(w1, m1, w2, m2, flags & 1);
    if (r == -2) { aret_unmodelled("CompareStringA: unmodelled linguistic collation"); return 0; }
    return (uint32_t)(r < 0 ? 1 : r > 0 ? 3 : 2);
}

/* ------------------------------------------------------------------ */
/* Heap — kernel32 heap maps onto the C allocator                      */
/* ------------------------------------------------------------------ */

uint32_t aret_GetProcessHeap(uint32_t esp) { (void)esp; return 1; } /* a non-null pseudo-handle */
uint32_t aret_HeapAlloc(uint32_t esp) {
    /* (hHeap, dwFlags, dwBytes) — HEAP_ZERO_MEMORY (0x8) -> calloc. */
    uint32_t flags = WU(1), bytes = WU(2);
    void *p = (flags & 0x8) ? calloc(1, bytes) : malloc(bytes);
    return WRP(p);
}
uint32_t aret_HeapFree(uint32_t esp) { free(WP(2)); return 1; }
uint32_t aret_HeapReAlloc(uint32_t esp) { return WRP(realloc(WP(2), WU(3))); }
/* HeapSize(hHeap, dwFlags, lpMem) — the usable size of an allocation. sqlite's
 * Windows allocator (`winMemSize`) uses it to track memory; returning 0 made it
 * believe every block was empty and abort with "out of memory". */
uint32_t aret_HeapSize(uint32_t esp) {
    void *p = WP(2);
    return p ? (uint32_t)malloc_usable_size(p) : 0;
}
uint32_t aret_HeapCreate(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_HeapDestroy(uint32_t esp) { (void)esp; return 1; }

/* VirtualAlloc(addr, size, type, protect) — back it with the allocator (we
 * ignore the requested base; the program uses the returned pointer). */
uint32_t aret_VirtualAlloc(uint32_t esp) {
    uint32_t size = WU(1);
    void *p = calloc(1, size ? size : 1);
    return WRP(p);
}
uint32_t aret_VirtualFree(uint32_t esp) { free(WP(0)); return 1; }
/* NtAllocateVirtualMemory(ProcessHandle, *BaseAddress, ZeroBits, *RegionSize, AllocationType,
 * Protect) -- the ntdll syscall VirtualAlloc/RtlAllocateHeap bottom out on. Backed by the
 * allocator like VirtualAlloc (requested base ignored; calloc = the zeroed MEM_COMMIT contract).
 * *RegionSize rounds UP to a 4096 page and is written back; *BaseAddress receives the base
 * (measured Wine: STATUS_SUCCESS, RegionSize 100->4096 / 5000->8192, memory zeroed + writable). */
uint32_t aret_NtAllocateVirtualMemory(uint32_t esp) {
    uint32_t pbase = WU(1), pregion = WU(3);
    if (!pregion) return 0xC000000Du;                    /* STATUS_INVALID_PARAMETER */
    uint32_t req = *(uint32_t *)(uintptr_t)pregion;
    uint32_t rounded = (req + 0xFFFu) & ~0xFFFu; if (!rounded) rounded = 0x1000u;
    void *p = calloc(1, rounded);
    if (!p) return 0xC0000017u;                          /* STATUS_NO_MEMORY */
    if (pbase) *(uint32_t *)(uintptr_t)pbase = WRP(p);
    *(uint32_t *)(uintptr_t)pregion = rounded;
    return 0;                                            /* STATUS_SUCCESS */
}
/* NtFreeVirtualMemory(ProcessHandle, *BaseAddress, *RegionSize, FreeType) -> STATUS_SUCCESS. */
uint32_t aret_NtFreeVirtualMemory(uint32_t esp) {
    uint32_t pbase = WU(1);
    if (pbase) { uint32_t b = *(uint32_t *)(uintptr_t)pbase; if (b) free((void *)(uintptr_t)b); }
    return 0;
}
uint32_t aret_GlobalAlloc(uint32_t esp) {
    /* (uFlags, dwBytes) — GMEM_ZEROINIT (0x40) -> calloc. */
    uint32_t flags = WU(0), bytes = WU(1);
    return WRP((flags & 0x40) ? calloc(1, bytes) : malloc(bytes));
}
uint32_t aret_GlobalFree(uint32_t esp) { free(WP(0)); return 0; }
uint32_t aret_LocalAlloc(uint32_t esp) { return aret_GlobalAlloc(esp); }
uint32_t aret_LocalFree(uint32_t esp) { free(WP(0)); return 0; }
/* GlobalHandle(pMem) -> HGLOBAL. Our heap is fixed (handle == pointer, GlobalLock
 * is identity), so the handle for a locked pointer is the pointer itself. */
uint32_t aret_GlobalHandle(uint32_t esp) { return WU(0); }
uint32_t aret_LocalHandle(uint32_t esp) { return WU(0); }

/* ------------------------------------------------------------------ */
/* Interlocked atomics — map onto GCC __atomic builtins                */
/* ------------------------------------------------------------------ */

uint32_t aret_InterlockedIncrement(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedDecrement(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedExchangeAdd(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_fetch_add(p, WI(1), __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedExchange(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    return (uint32_t)__atomic_exchange_n(p, WI(1), __ATOMIC_SEQ_CST);
}
uint32_t aret_InterlockedCompareExchange(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    int32_t cmp = WI(2), xch = WI(1);
    __atomic_compare_exchange_n(p, &cmp, xch, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return (uint32_t)cmp; /* the prior value */
}

/* ------------------------------------------------------------------ */
/* System info / locale / process metadata                            */
/* ------------------------------------------------------------------ */

/* GetSystemInfo(LPSYSTEM_INFO) — fill a plausible 32-bit layout. */
uint32_t aret_GetSystemInfo(uint32_t esp) {
    uint32_t *si = (uint32_t *)WP(0);
    if (si) {
        si[0] = 0;            /* wProcessorArchitecture/wReserved (x86=0) */
        si[1] = 0x1000;       /* dwPageSize */
        si[2] = 0x10000;      /* lpMinimumApplicationAddress */
        si[3] = 0x7ffeffff;   /* lpMaximumApplicationAddress */
        si[4] = 1;            /* dwActiveProcessorMask */
        si[5] = 1;            /* dwNumberOfProcessors */
        si[6] = 586;          /* dwProcessorType */
        si[7] = 0x10000;      /* dwAllocationGranularity */
        si[8] = (6 << 8) | 1; /* wProcessorLevel/Revision (cosmetic) */
    }
    return 0;
}
uint32_t aret_GetNativeSystemInfo(uint32_t esp) { return aret_GetSystemInfo(esp); }

/* GetVersionExA/W(LPOSVERSIONINFO): report a real Windows version and return
 * TRUE, exactly as Windows/Wine do (Wine 9 reports 6.2.9200 NT). The caller sets
 * dwOSVersionInfoSize (v[0]); we fill the version fields. szCSDVersion is left
 * zeroed (empty service pack — a valid state the program handles). Faithful
 * values matter: the CRT/VFS pick their code path from the reported version. */
uint32_t aret_GetVersionExA(uint32_t esp) {
    uint32_t *v = (uint32_t *)WP(0);
    if (v) {
        v[1] = 6;      /* dwMajorVersion */
        v[2] = 2;      /* dwMinorVersion (6.2 = Windows 8 / Server 2012) */
        v[3] = 9200;   /* dwBuildNumber */
        v[4] = 2;      /* dwPlatformId = VER_PLATFORM_WIN32_NT */
        memset((char *)&v[5], 0, 128); /* szCSDVersion[128] (ANSI) */
    }
    return 1; /* TRUE */
}
uint32_t aret_GetVersionExW(uint32_t esp) {
    uint32_t *v = (uint32_t *)WP(0);
    if (v) {
        v[1] = 6; v[2] = 2; v[3] = 9200; v[4] = 2;
        memset((char *)&v[5], 0, 256); /* szCSDVersion[128] (WCHAR) */
    }
    return 1;
}
/* GetVersion(): the legacy packed form, kept consistent with GetVersionEx above
 * (6.2.9200, NT). LOBYTE(LOWORD)=major, HIBYTE(LOWORD)=minor; bit 31 = 0 for NT,
 * and then HIWORD = build number. Old programs branch on this, so it must agree
 * with GetVersionEx (Wine's two version APIs agree too). 6 | 2<<8 | 9200<<16. */
uint32_t aret_GetVersion(uint32_t esp) {
    (void)esp;
    return 6u | (2u << 8) | (9200u << 16); /* 0x23F00206 — 6.2.9200, NT */
}

/* RtlMoveMemory(dst, src, len) -> void. Overlap-safe copy = memmove. */
uint32_t aret_RtlMoveMemory(uint32_t esp) {
    void *d = WP(0);
    const void *s = (const void *)WP(1);
    if (d && s) memmove(d, s, WU(2));
    return 0;
}

/* Days since 1970-01-01 for a proleptic-Gregorian date (Howard Hinnant's
 * algorithm) — portable (no timegm; works under wasi too). */
static int64_t aret_days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
/* DosDateTimeToFileTime(wFatDate, wFatTime, LPFILETIME) -> BOOL. MS-DOS packed
 * date/time -> a 64-bit FILETIME (100ns ticks since 1601), no timezone shift —
 * the canonical field->FILETIME calendar computation (matches Wine). FAT date:
 * bits 0-4 day, 5-8 month, 9-15 year-since-1980. FAT time: 0-4 sec/2, 5-10 min,
 * 11-15 hour. */
uint32_t aret_DosDateTimeToFileTime(uint32_t esp) {
    uint32_t d = WU(0) & 0xffff, t = WU(1) & 0xffff;
    uint32_t *ft = (uint32_t *)WP(2);
    int year = (int)((d >> 9) & 0x7f) + 1980;
    unsigned mon = (d >> 5) & 0x0f, day = d & 0x1f;
    unsigned hour = (t >> 11) & 0x1f, min = (t >> 5) & 0x3f, sec = (t & 0x1f) * 2;
    if (mon < 1) mon = 1;
    if (day < 1) day = 1;
    int64_t days = aret_days_from_civil(year, mon, day);
    int64_t secs = days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
    uint64_t ticks = ((uint64_t)(secs + 11644473600LL)) * 10000000ULL;
    if (ft) { ft[0] = (uint32_t)ticks; ft[1] = (uint32_t)(ticks >> 32); }
    return 1;
}

/* AreFileApisANSI(): the process uses the ANSI code page for narrow file APIs,
 * the Windows default (Wine returns TRUE). */
uint32_t aret_AreFileApisANSI(uint32_t esp) { (void)esp; return 1; }

uint32_t aret_GetACP(uint32_t esp)   { (void)esp; return 1252; } /* Windows-1252 */
uint32_t aret_GetOEMCP(uint32_t esp) { (void)esp; return 437; }

/* ANSI(CP1252) <-> OEM(CP437) byte translation for CharToOem/OemToChar. The
 * tables are the ground-truth best-fit mapping extracted verbatim from the very
 * Wine (msvcrt/user32) that is our oracle: running CharToOemA/OemToCharA over all
 * 256 byte values and recording the result. Best-fit (not strict) is what Windows
 * uses here — e.g. U+201A single low quote -> ','; a char with no CP437 form -> '?'.
 * Bit-identical to Wine by construction (ACP=1252, OEMCP=437). */
static const uint8_t u32_ansi_to_oem[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    63, 63, 44, 159, 44, 46, 43, 216, 94, 37, 83, 60, 79, 63, 90, 63,
    63, 96, 39, 34, 34, 7, 45, 45, 126, 84, 115, 62, 111, 63, 122, 89,
    255, 173, 155, 156, 15, 157, 221, 21, 34, 99, 166, 174, 170, 45, 114, 95,
    248, 241, 253, 51, 39, 230, 20, 250, 44, 49, 167, 175, 172, 171, 95, 168,
    65, 65, 65, 65, 142, 143, 146, 128, 69, 144, 69, 69, 73, 73, 73, 73,
    68, 165, 79, 79, 79, 79, 153, 120, 79, 85, 85, 85, 154, 89, 95, 225,
    133, 160, 131, 97, 132, 134, 145, 135, 138, 130, 136, 137, 141, 161, 140, 139,
    100, 164, 149, 162, 147, 111, 148, 246, 111, 151, 163, 150, 129, 121, 95, 152,
};
static const uint8_t u32_oem_to_ansi[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
    96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    199, 252, 233, 226, 228, 224, 229, 231, 234, 235, 232, 239, 238, 236, 196, 197,
    201, 230, 198, 244, 246, 242, 251, 249, 255, 214, 220, 162, 163, 165, 80, 131,
    225, 237, 243, 250, 241, 209, 170, 186, 191, 172, 172, 189, 188, 161, 171, 187,
    166, 166, 166, 166, 166, 166, 166, 43, 43, 166, 166, 43, 43, 43, 43, 43,
    43, 45, 45, 43, 45, 43, 166, 166, 43, 43, 45, 45, 166, 45, 43, 45,
    45, 45, 45, 43, 43, 43, 43, 43, 43, 43, 43, 166, 95, 166, 166, 175,
    97, 223, 71, 112, 83, 115, 181, 116, 70, 84, 79, 100, 56, 102, 101, 110,
    61, 177, 61, 61, 40, 41, 247, 152, 176, 183, 183, 118, 110, 178, 166, 160,
};
/* CharToOem(Buff)A / OemToChar(Buff)A — translate a string between the ANSI and OEM
 * code pages in place per byte. The A forms are NUL-terminated (and copy the NUL);
 * the Buff forms take an explicit length and do not stop at NUL. Return TRUE. */
static uint32_t u32_xlate_z(uint32_t src, uint32_t dst, const uint8_t *tab) {
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    uint8_t *d = (uint8_t *)(uintptr_t)dst;
    if (!s || !d) return 0;
    do { *d = tab[*s]; } while (*s++ && (d++, 1));
    return 1;
}
static uint32_t u32_xlate_n(uint32_t src, uint32_t dst, uint32_t n, const uint8_t *tab) {
    const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    uint8_t *d = (uint8_t *)(uintptr_t)dst;
    if ((!s || !d) && n) return 0;
    for (uint32_t i = 0; i < n; i++) d[i] = tab[s[i]];
    return 1;
}
uint32_t aret_CharToOemA(uint32_t esp)  { return u32_xlate_z(WP(0), WP(1), u32_ansi_to_oem); }
uint32_t aret_OemToCharA(uint32_t esp)  { return u32_xlate_z(WP(0), WP(1), u32_oem_to_ansi); }
uint32_t aret_CharToOemBuffA(uint32_t esp) { return u32_xlate_n(WP(0), WP(1), WP(2), u32_ansi_to_oem); }
uint32_t aret_OemToCharBuffA(uint32_t esp) { return u32_xlate_n(WP(0), WP(1), WP(2), u32_oem_to_ansi); }

/* System error message table — the exact strings Wine's FormatMessage(FROM_SYSTEM)
 * produces (extracted verbatim, our oracle), each ending ".\r\n". Covers the common
 * error codes programs format; a code NOT here aborts soundly (never an empty/wrong
 * message) and is added when a real binary needs it. */
static const struct { uint32_t code; const char *msg; } u32_sys_msg[] = {
    {0, "Success.\r\n"},        {1, "Invalid function.\r\n"},   {2, "File not found.\r\n"},
    {3, "Path not found.\r\n"}, {4, "Too many open files.\r\n"},{5, "Access denied.\r\n"},
    {6, "Invalid handle.\r\n"}, {8, "Not enough memory.\r\n"},  {13, "Invalid data.\r\n"},
    {14, "Out of memory.\r\n"}, {32, "Sharing violation.\r\n"}, {33, "Lock violation.\r\n"},
    {38, "End of file.\r\n"},   {50, "Request not supported.\r\n"}, {87, "Invalid parameter.\r\n"},
    {112, "Disk full.\r\n"},    {122, "Insufficient buffer.\r\n"}, {183, "File already exists.\r\n"},
    {206, "File name is too long.\r\n"}, {234, "More data available.\r\n"},
    {259, "No more data available.\r\n"}, {1223, "Operation canceled by user.\r\n"},
};
static const char *u32_sys_msg_lookup(uint32_t code) {
    for (int i = 0; i < (int)(sizeof u32_sys_msg / sizeof u32_sys_msg[0]); i++)
        if (u32_sys_msg[i].code == code) return u32_sys_msg[i].msg;
    return 0;
}
/* FormatMessage(A/W)(flags, source, msgId, langId, buffer, size, args) -> chars
 * written (excl. NUL). Only FORMAT_MESSAGE_FROM_SYSTEM is modelled (the dominant use:
 * turn an error code into its text), optionally with ALLOCATE_BUFFER (LocalAlloc the
 * result and store the pointer through `buffer`). FROM_STRING / FROM_HMODULE / insert
 * processing, and a code not in the verified table, abort soundly — never a wrong or
 * empty string. */
uint32_t aret_FormatMessageA(uint32_t esp) {
    uint32_t flags = WU(0), msgId = WU(2), buf = WU(4), size = WU(5);
    if (!(flags & 0x00001000u)) { aret_partial("FormatMessageA: only FORMAT_MESSAGE_FROM_SYSTEM modelled"); return 0; }
    const char *msg = u32_sys_msg_lookup(msgId);
    if (!msg) { char m[80]; snprintf(m, sizeof m, "FormatMessageA: unmodelled system message %u", msgId); aret_partial(m); return 0; }
    uint32_t len = (uint32_t)strlen(msg);
    if (flags & 0x00000100u /*ALLOCATE_BUFFER*/) {
        char *p = (char *)malloc(len + 1);
        if (!p) { g_last_error = 8u; return 0; }
        memcpy(p, msg, len + 1);
        if (buf) *(uint32_t *)(uintptr_t)buf = (uint32_t)(uintptr_t)p;   /* buffer is LPSTR* */
        return len;
    }
    if (!buf || size == 0) return 0;
    uint32_t n = len < size - 1 ? len : size - 1;
    char *d = (char *)(uintptr_t)buf;
    memcpy(d, msg, n); d[n] = 0;
    return n;
}
uint32_t aret_FormatMessageW(uint32_t esp) {
    uint32_t flags = WU(0), msgId = WU(2), buf = WU(4), size = WU(5);
    if (!(flags & 0x00001000u)) { aret_partial("FormatMessageW: only FORMAT_MESSAGE_FROM_SYSTEM modelled"); return 0; }
    const char *msg = u32_sys_msg_lookup(msgId);
    if (!msg) { char m[80]; snprintf(m, sizeof m, "FormatMessageW: unmodelled system message %u", msgId); aret_partial(m); return 0; }
    uint32_t len = (uint32_t)strlen(msg);
    if (flags & 0x00000100u /*ALLOCATE_BUFFER*/) {
        uint16_t *p = (uint16_t *)malloc((len + 1) * 2);
        if (!p) { g_last_error = 8u; return 0; }
        for (uint32_t i = 0; i <= len; i++) p[i] = (uint16_t)(unsigned char)msg[i];
        if (buf) *(uint32_t *)(uintptr_t)buf = (uint32_t)(uintptr_t)p;
        return len;
    }
    if (!buf || size == 0) return 0;
    uint32_t n = len < size - 1 ? len : size - 1;
    uint16_t *d = (uint16_t *)(uintptr_t)buf;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint16_t)(unsigned char)msg[i];
    d[n] = 0;
    return n;
}
/* Locale IDs: report en-US (LCID 0x0409) — the CRT reads GetThreadLocale for
 * locale-dependent classification (GNU m4/grep query it at startup; a 0 stub made
 * their locale setup misbehave and swallow output). */
uint32_t aret_GetThreadLocale(uint32_t esp)          { (void)esp; return 0x0409; }
uint32_t aret_GetUserDefaultLCID(uint32_t esp)       { (void)esp; return 0x0409; }
uint32_t aret_GetSystemDefaultLCID(uint32_t esp)     { (void)esp; return 0x0409; }
uint32_t aret_GetUserDefaultUILanguage(uint32_t esp) { (void)esp; return 0x0409; }
uint32_t aret_SetThreadLocale(uint32_t esp)          { (void)esp; return 0x0409; }
/* ConvertDefaultLocale(LCID) -> LCID: replaces the "default" pseudo-LCIDs with a
 * concrete one. The WHOLE 16-bit LCID space was swept against Wine, and the sweep —
 * not reasoning — decided what this models:
 *   - the five defaults (0 NEUTRAL, 0x400 USER, 0x800 SYSTEM, 0xC00 CUSTOM_DEFAULT,
 *     0x1000 CUSTOM_UNSPECIFIED) resolve to the user locale, 0x0409 here, matching
 *     the invariant GetThreadLocale/GetUserDefaultLCID publish;
 *   - an LCID with a NON-neutral sublang passes through unchanged — for 64416 of the
 *     64449 such values. The other 33 are remapped (0x641a->0x201a, 0x0460->0x1000,
 *     alternate sorts and script variants) and are listed below;
 *   - a NEUTRAL sublang (0) does NOT simply become sublang 1: ~40 values disagree
 *     (Chinese 0x0004 -> 0x0804, 0x000a -> 0x0c0a). It is a TABLE, not a rule.
 * The remappings and the neutral-sublang table are Wine's locale DATABASE —
 * environmental, version-dependent — so by the EnumFontFamilies rule (70 §4.5) the
 * CONTRACT is modelled and the DATA is not: those cases ABORT rather than ship a
 * rule the sweep proved wrong, or embed a table that would silently rot. */
static int u32_lcid_is_remapped(uint32_t l) {
    static const uint16_t remap[] = {
        0x0460, 0x641a, 0x681a, 0x6c1a, 0x701a, 0x703b, 0x742c, 0x743b, 0x7804,
        0x7814, 0x781a, 0x782c, 0x783b, 0x7843, 0x7850, 0x785d, 0x7c04, 0x7c14,
        0x7c1a, 0x7c28, 0x7c2e, 0x7c3b, 0x7c43, 0x7c46, 0x7c50, 0x7c59, 0x7c5c,
        0x7c5d, 0x7c5f, 0x7c67, 0x7c68, 0x7c86, 0x7c92,
    };
    for (unsigned i = 0; i < sizeof remap / sizeof *remap; i++)
        if (remap[i] == l) return 1;
    return 0;
}
/* A NEUTRAL sublang (0) asks for "this language's default locale". Swept across all
 * 1024 primary ids, the answers fall into exactly three groups, and the shape is what
 * makes this modellable at all rather than a 1024-entry blob:
 *   - 122 assigned languages take SUBLANG_DEFAULT, i.e. simply `id | 0x400`;
 *   - 12 take a DIFFERENT sublang, because their conventional default is not the
 *     first one (Chinese 0x004 -> 0x804 Simplified, Serbian-ish 0x01a -> 0xc0a, and
 *     three that answer 0x1000 CUSTOM_UNSPECIFIED outright);
 *   - the remaining 889 ids are UNASSIGNED and pass through untouched.
 * So the naive "neutral becomes sublang 1" is wrong 901 times out of 1023 — it is
 * right only for the assigned set, which has to be enumerated.
 * This is Wine's locale database, but unlike a font list it is not machine-dependent:
 * it is compiled into Wine, so it is deterministic for a given version and CAN be
 * gated. `winecorpus/win32_convlocale.c` sweeps every one of the 1024 ids, so a Wine
 * that gained or lost a language turns this red instead of letting it rot silently.
 * That gateability is the whole reason it is embedded rather than aborted on. */
static uint32_t u32_lcid_neutral(uint32_t id) {
    static const uint16_t assigned[] = {
        0x001, 0x002, 0x003, 0x005, 0x006, 0x007, 0x008, 0x009, 0x00b, 0x00c, 0x00d, 0x00e,
        0x00f, 0x010, 0x011, 0x012, 0x013, 0x014, 0x015, 0x016, 0x017, 0x018, 0x019, 0x01a,
        0x01b, 0x01c, 0x01d, 0x01e, 0x01f, 0x020, 0x021, 0x022, 0x023, 0x024, 0x025, 0x026,
        0x027, 0x028, 0x029, 0x02a, 0x02b, 0x02c, 0x02d, 0x02e, 0x02f, 0x030, 0x031, 0x032,
        0x033, 0x034, 0x035, 0x036, 0x037, 0x038, 0x039, 0x03a, 0x03b, 0x03e, 0x03f, 0x040,
        0x041, 0x042, 0x043, 0x044, 0x046, 0x047, 0x048, 0x049, 0x04a, 0x04b, 0x04c, 0x04d,
        0x04e, 0x04f, 0x050, 0x051, 0x052, 0x053, 0x054, 0x055, 0x056, 0x057, 0x058, 0x05a,
        0x05b, 0x05c, 0x05e, 0x061, 0x062, 0x063, 0x064, 0x065, 0x066, 0x068, 0x06a, 0x06b,
        0x06c, 0x06d, 0x06e, 0x06f, 0x070, 0x071, 0x072, 0x074, 0x075, 0x076, 0x077, 0x078,
        0x07a, 0x07c, 0x07e, 0x080, 0x081, 0x082, 0x083, 0x085, 0x086, 0x087, 0x088, 0x08c,
        0x091, 0x092,
    };
    static const uint16_t special[][2] = {
        { 0x004, 0x0804 }, { 0x00a, 0x0c0a }, { 0x03c, 0x083c }, { 0x03d, 0x1000 },
        { 0x045, 0x0845 }, { 0x059, 0x0859 }, { 0x05d, 0x085d }, { 0x05f, 0x085f },
        { 0x060, 0x1000 }, { 0x067, 0x0867 }, { 0x073, 0x0873 }, { 0x084, 0x1000 },
    };
    for (unsigned i = 0; i < sizeof special / sizeof *special; i++)
        if (special[i][0] == id) return special[i][1];
    for (unsigned i = 0; i < sizeof assigned / sizeof *assigned; i++)
        if (assigned[i] == id) return id | 0x400u;
    return id;                                   /* unassigned: unchanged */
}

uint32_t aret_ConvertDefaultLocale(uint32_t esp) {
    uint32_t lcid = w32_arg(esp, 0);
    if (lcid == 0x0000 || lcid == 0x0400 || lcid == 0x0800 ||
        lcid == 0x0c00 || lcid == 0x1000)
        return 0x0409;
    if (lcid <= 0xFFFFu && (lcid >> 10) != 0 && !u32_lcid_is_remapped(lcid))
        return lcid;                                   /* proven pass-through */
    if (lcid <= 0xFFFFu && (lcid >> 10) == 0)
        return u32_lcid_neutral(lcid);
    {
        char m[112];
        snprintf(m, sizeof m,
                 "ConvertDefaultLocale(%#x): this remapped LCID is Wine's locale table",
                 lcid);
        aret_unmodelled(m);
    }
    return 0;
}
/* PeekNamedPipe(h, buf, size, *read, *avail, *left): CLI tools call it to see
 * whether stdin has data / is a pipe. Our handle is a POSIX fd; use FIONREAD for
 * the available-byte count. Non-pipe fds (a file/tty) fail as on Windows. A true
 * peek (reading without consuming) is not needed by the callers — they only test
 * `*avail`; report 0 read. Returns BOOL. */
uint32_t aret_PeekNamedPipe(uint32_t esp) {
    int fd = WI(0);
    uint32_t *bytes_read = (uint32_t *)WP(3);
    uint32_t *total_avail = (uint32_t *)WP(4);
    uint32_t *left = (uint32_t *)WP(5);
    int navail = 0;
    if (ioctl(fd, FIONREAD, &navail) != 0) return 0; /* not a pipe -> FALSE */
    if (bytes_read) *bytes_read = 0;
    if (total_avail) *total_avail = (uint32_t)navail;
    if (left) *left = (uint32_t)navail;
    return 1;
}
uint32_t aret_IsValidCodePage(uint32_t esp) {
    switch (WU(0)) {
        case 437: case 850: case 852: case 866: case 874: case 932: case 936:
        case 949: case 950: case 1200: case 1201: case 1250: case 1251: case 1252:
        case 1253: case 1254: case 1255: case 1256: case 1257: case 1258:
        case 65000: case 65001: return 1;
        default: return 0;
    }
}
/* GetCPInfo: the code pages we report are single-byte (MaxCharSize 1); default
 * char '?', no DBCS lead-byte ranges. CPINFO = {UINT MaxCharSize; BYTE Default[2];
 * BYTE LeadByte[12];}. */
uint32_t aret_GetCPInfo(uint32_t esp) {
    uint8_t *ci = (uint8_t *)(uintptr_t)WU(1);
    if (!ci) return 0;
    *(uint32_t *)(ci + 0) = 1;
    ci[4] = '?'; ci[5] = 0;
    memset(ci + 6, 0, 12);
    return 1;
}
/* GetStringTypeW(CT_CTYPE1, src, count, out): classify each WCHAR into the C1_*
 * character-type bits (ASCII subset via ctype). */
uint32_t aret_GetStringTypeW(uint32_t esp) {
    if (WU(0) != 1) return 0; /* only CT_CTYPE1 modelled */
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(1);
    int n = (int)WU(2);
    uint16_t *out = (uint16_t *)(uintptr_t)WU(3);
    if (!s || !out) return 0;
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (int i = 0; i < n; i++) {
        unsigned c = s[i];
        uint16_t t = 0;
        if (c < 256) {
            if (isupper((int)c)) t |= 0x0001 | 0x0100; /* UPPER | ALPHA */
            if (islower((int)c)) t |= 0x0002 | 0x0100; /* LOWER | ALPHA */
            if (isdigit((int)c)) t |= 0x0004;          /* DIGIT */
            if (isspace((int)c)) t |= 0x0008;          /* SPACE */
            if (ispunct((int)c)) t |= 0x0010;          /* PUNCT */
            if (iscntrl((int)c)) t |= 0x0020;          /* CNTRL */
            if (isblank((int)c)) t |= 0x0040;          /* BLANK */
            if (isxdigit((int)c)) t |= 0x0080;         /* XDIGIT */
            if (t) t |= 0x0200;                        /* DEFINED */
        }
        out[i] = t;
    }
    return 1;
}
/* LCMapStringW: the upper/lower-case mappings (the common CRT use); other flags
 * pass the string through unchanged. */
uint32_t aret_LCMapStringW(uint32_t esp) {
    uint32_t flags = WU(1);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(2);
    int srclen = (int)WU(3);
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(4);
    int dstlen = (int)WU(5);
    if (!src) return 0;
    if (srclen < 0) { srclen = 0; while (src[srclen]) srclen++; srclen++; }
    if (dstlen == 0 || !dst) return (uint32_t)srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        unsigned c = src[i];
        if (c < 256) {
            if (flags & 0x00000200u) c = (unsigned)toupper((int)c); /* LCMAP_UPPERCASE */
            else if (flags & 0x00000100u) c = (unsigned)tolower((int)c); /* LCMAP_LOWERCASE */
        }
        dst[i] = (uint16_t)c;
    }
    return (uint32_t)n;
}
/* GetStringTypeA(Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType) — ANSI twin of
 * GetStringTypeW (note the extra leading Locale arg). Single-byte ctype. */
uint32_t aret_GetStringTypeA(uint32_t esp) {
    if (WU(1) != 1) return 0;                    /* CT_CTYPE1 only */
    const unsigned char *s = (const unsigned char *)WP(2);
    int n = WI(3);
    uint16_t *out = (uint16_t *)WP(4);
    if (!s || !out) return 0;
    if (n < 0) { n = 0; while (s[n]) n++; }
    for (int i = 0; i < n; i++) {
        unsigned c = s[i]; uint16_t t = 0;
        if (isupper((int)c)) t |= 0x0001 | 0x0100;
        if (islower((int)c)) t |= 0x0002 | 0x0100;
        if (isdigit((int)c)) t |= 0x0004;
        if (isspace((int)c)) t |= 0x0008;
        if (ispunct((int)c)) t |= 0x0010;
        if (iscntrl((int)c)) t |= 0x0020;
        if (isblank((int)c)) t |= 0x0040;
        if (isxdigit((int)c)) t |= 0x0080;
        if (t) t |= 0x0200;
        out[i] = t;
    }
    return 1;
}
/* LCMapStringA — ANSI twin of LCMapStringW (upper/lower-case; else pass-through). */
uint32_t aret_LCMapStringA(uint32_t esp) {
    uint32_t flags = WU(1);
    const unsigned char *src = (const unsigned char *)WP(2);
    int srclen = WI(3);
    char *dst = (char *)WP(4);
    int dstlen = WI(5);
    if (!src) return 0;
    if (srclen < 0) { srclen = 0; while (src[srclen]) srclen++; srclen++; }
    if (dstlen == 0 || !dst) return (uint32_t)srclen;
    int n = srclen < dstlen ? srclen : dstlen;
    for (int i = 0; i < n; i++) {
        unsigned c = src[i];
        if (flags & 0x00000200u) c = (unsigned)toupper((int)c);        /* LCMAP_UPPERCASE */
        else if (flags & 0x00000100u) c = (unsigned)tolower((int)c);   /* LCMAP_LOWERCASE */
        dst[i] = (char)c;
    }
    return (uint32_t)n;
}
/* CompareStringA/W(Locale, dwCmpFlags, s1, n1, s2, n2) -> CSTR_LESS_THAN(1)/
 * EQUAL(2)/GREATER_THAN(3). Real linguistic collation (measured sort keys) lives
 * with lstrcmpW above; these stubs were removed. */
uint32_t aret_IsProcessorFeaturePresent(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_IsDebuggerPresent(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_IsBadReadPtr(uint32_t esp)  { return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadWritePtr(uint32_t esp) { return (uint32_t)(WU(0) == 0); }

/* Copy a fixed Windows-ish path into the caller's buffer. These APIs take
   (LPSTR lpBuffer, UINT uSize) — buffer first, then size. */
static uint32_t fill_path(uint32_t esp, const char *path) {
    char *buf = WS(0); uint32_t size = WU(1);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { memcpy(buf, path, len + 1); return len; }
    return len;
}
uint32_t aret_GetSystemDirectoryA(uint32_t esp)  { return fill_path(esp, "C:\\Windows\\System32"); }
uint32_t aret_GetWindowsDirectoryA(uint32_t esp) { return fill_path(esp, "C:\\Windows"); }
/* MulDiv(a,b,c) = round(a*b/c) to nearest, ties AWAY from zero; -1 on c==0 or a
 * 32-bit overflow (measured on Wine: 10*3/4=8, -10*3/4=-8, x/0=-1). */
uint32_t aret_MulDiv(uint32_t esp) {
    int64_t a = (int32_t)WU(0), b = (int32_t)WU(1), c = (int32_t)WU(2);
    if (c == 0) return (uint32_t)-1;
    int64_t num = a * b, q = num / c, rem = num % c;
    int64_t ac = c < 0 ? -c : c, arem = rem < 0 ? -rem : rem;
    if (2 * arem >= ac) q += ((num < 0) ^ (c < 0)) ? -1 : 1;
    if (q > 2147483647LL || q < -2147483648LL) return (uint32_t)-1;
    return (uint32_t)(int32_t)q;
}
/* Locale/UI language: this runtime fixes en-US (0x0409), consistent with GetThreadLocale. */
uint32_t aret_GetUserDefaultLangID(uint32_t esp)       { (void)esp; return 0x0409; }
uint32_t aret_GetSystemDefaultLangID(uint32_t esp)     { (void)esp; return 0x0409; }
uint32_t aret_GetSystemDefaultUILanguage(uint32_t esp) { (void)esp; return 0x0409; }
/* LoadLibraryW — same non-NULL pseudo-module handle as LoadLibraryA. */
uint32_t aret_LoadLibraryW(uint32_t esp) { (void)esp; return 0x10000000u; }
/* SleepEx(ms, alertable) -> 0 (no APC/IO completion modelled). Yields like Sleep. */
uint32_t aret_SleepEx(uint32_t esp) {
    uint32_t ms = WU(0);
    if (!aret_fiber_sleep(ms)) usleep((useconds_t)ms * 1000u);
    return 0;
}
/* NtDelayExecution(Alertable, DelayInterval) -- the ntdll syscall Sleep bottoms out on. The
 * interval is a LARGE_INTEGER in 100 ns units; a NEGATIVE value is a relative delay, so
 * |value|/10000 = ms, routed to the same fiber sleep as Sleep. No APC/alert modelled -> returns
 * STATUS_SUCCESS like SleepEx (measured Wine). A positive (absolute) interval is a rare unmodelled
 * sub-case -> aret_partial (defined, not guessed). */
uint32_t aret_NtDelayExecution(uint32_t esp) {
    uint32_t pdi = WU(1);
    if (!pdi) return 0;                                  /* NULL interval: nothing to wait on */
    int64_t iv = *(const int64_t *)(uintptr_t)pdi;
    if (iv > 0) { aret_partial("NtDelayExecution: absolute (positive) interval not modelled"); return 0; }
    uint32_t ms = (uint32_t)((-iv) / 10000);             /* 100 ns ticks -> ms */
    if (!aret_fiber_sleep(ms)) usleep((useconds_t)ms * 1000u);
    return 0;                                            /* STATUS_SUCCESS */
}
/* Wide (16-bit) directory helpers — widen the narrow results (consistent with the A
 * versions; paths are ASCII). */
static uint32_t u32_fill_pathw(uint32_t esp, const char *path) {
    uint16_t *buf = (uint16_t *)WP(0); uint32_t size = WU(1);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { for (uint32_t i = 0; i <= len; i++) buf[i] = (uint8_t)path[i]; return len; }
    return len + 1;
}
uint32_t aret_GetSystemDirectoryW(uint32_t esp)  { return u32_fill_pathw(esp, "C:\\Windows\\System32"); }
uint32_t aret_GetWindowsDirectoryW(uint32_t esp) { return u32_fill_pathw(esp, "C:\\Windows"); }
uint32_t aret_GetCurrentDirectoryW(uint32_t esp) {
    uint32_t size = WU(0); uint16_t *buf = (uint16_t *)WP(1);
    char tmp[4096]; if (!getcwd(tmp, sizeof tmp)) return 0;
    uint32_t len = (uint32_t)strlen(tmp);
    if (buf && size > len) { for (uint32_t i = 0; i <= len; i++) buf[i] = (uint8_t)tmp[i]; return len; }
    return len + 1;
}
uint32_t aret_SetCurrentDirectoryW(uint32_t esp) {
    const uint16_t *w = (const uint16_t *)WP(0);
    char p[4096]; int i = 0;
    if (w) for (; w[i] && i < 4095; i++) p[i] = (char)w[i];
    p[i] = 0;
    return (uint32_t)(chdir(p) == 0);
}

/* GetModuleFileNameA(hModule, buf, size) — report a stable fake image path. */
uint32_t aret_GetModuleFileNameA(uint32_t esp) {
    const char *path = "C:\\program.exe";
    char *buf = WS(1); uint32_t size = WU(2);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { memcpy(buf, path, len + 1); return len; }
    return 0;
}
uint32_t aret_GetModuleFileNameW(uint32_t esp) {
    const char *path = "C:\\program.exe";
    uint16_t *buf = (uint16_t *)(uintptr_t)WU(1); uint32_t size = WU(2);
    uint32_t len = (uint32_t)strlen(path);
    if (buf && size > len) { for (uint32_t i = 0; i <= len; i++) buf[i] = (unsigned char)path[i]; return len; }
    return 0;
}
uint32_t aret_GetModuleHandleExW(uint32_t esp) {
    uint32_t *out = (uint32_t *)(uintptr_t)WU(2);
    if (out) *out = 0x00400000u;
    return 1;
}
/* Pseudo-handles: GetCurrentProcess()/Thread() are the constants -1 / -2. */
uint32_t aret_GetCurrentProcess(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }

/* GetExitCodeProcess(hProcess, lpExitCode) -> BOOL. We create no child processes
 * (CreateProcess is a sound failure), so any handle a program holds is the
 * current-process pseudo-handle — report STILL_ACTIVE (259), exactly as a running
 * process does (matches Wine for GetCurrentProcess). */
uint32_t aret_GetExitCodeProcess(uint32_t esp) {
    uint32_t *code = (uint32_t *)WP(1);
    if (code) *code = 259u; /* STILL_ACTIVE */
    return 1;
}

/* GetHandleInformation(handle, LPDWORD lpdwFlags) -> BOOL. Our handles are host fds;
 * they carry no Win32 inherit/protect flags, so report 0 flags and success. */
uint32_t aret_GetHandleInformation(uint32_t esp) {
    uint32_t *flags = (uint32_t *)WP(1);
    if (flags) *flags = 0;
    return 1;
}
/* SetHandleInformation(handle, dwMask, dwFlags) -> BOOL. The flags (HANDLE_FLAG_INHERIT
 * / PROTECT_FROM_CLOSE) have no effect in this single-process model; accept and succeed. */
uint32_t aret_SetHandleInformation(uint32_t esp) { (void)esp; return 1; }

/* DuplicateHandle(srcProc, srcHandle, tgtProc, LPHANDLE tgtHandle, access, inherit,
 * options) -> BOOL. Handles are host fds, so dup() the source. The process arguments
 * are pseudo-handles here (one process) and ignored. DUPLICATE_CLOSE_SOURCE closes the
 * source; DUPLICATE_SAME_ACCESS is implicit (a dup shares the description). */
uint32_t aret_DuplicateHandle(uint32_t esp) {
    int src = (int)WU(1);
    uint32_t *tgt = (uint32_t *)WP(3);
    uint32_t options = WU(6);
    int nfd = dup(src);
    if (nfd < 0) { if (tgt) *tgt = 0; return 0; }
    if (options & 0x1u /* DUPLICATE_CLOSE_SOURCE */) close(src);
    if (tgt) *tgt = (uint32_t)nfd;
    return 1;
}

/* SetConsoleTextAttribute(handle, WORD attr) -> BOOL. Handles are host fds. The colour
 * is out-of-band and has no effect on the program's byte output, so the effect is a
 * no-op — but the RETURN must be faithful: a console API succeeds only on a real console
 * and fails (FALSE) on a redirected/piped handle. isatty() gives exactly that, matching
 * Windows/Wine both when attached to a terminal and under redirection. */
uint32_t aret_SetConsoleTextAttribute(uint32_t esp) { return (uint32_t)(isatty((int)WU(0)) ? 1 : 0); }

/* GetDiskFreeSpaceA(lpRootPath, lpSectorsPerCluster, lpBytesPerSector,
 * lpFreeClusters, lpTotalClusters) -> BOOL. Host `statvfs` with a fixed geometry
 * (512-byte sectors, 8 sectors/cluster = 4 KiB), free/total clusters from the real
 * filesystem. A Windows root ("C:\\", NULL) maps to the host cwd. */
uint32_t aret_GetDiskFreeSpaceA(uint32_t esp) {
#ifdef __wasm__
    /* wasm32-wasi has no filesystem statvfs. Report failure (BOOL FALSE) rather
     * than a fabricated geometry — sound: no silent wrong disk size. */
    (void)esp; return 0;
#else
    struct statvfs vfs;
    if (statvfs(".", &vfs) != 0) { return 0; }
    uint32_t bps = 512, spc = 8;                 /* 4 KiB cluster */
    uint64_t cluster = (uint64_t)bps * spc;
    uint64_t total = ((uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize) / cluster;
    uint64_t avail = ((uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize) / cluster;
    if (total > 0xFFFFFFFFull) total = 0xFFFFFFFFull; /* 32-bit cluster count */
    if (avail > total) avail = total;
    uint32_t *p;
    if ((p = (uint32_t *)WP(1))) *p = spc;
    if ((p = (uint32_t *)WP(2))) *p = bps;
    if ((p = (uint32_t *)WP(3))) *p = (uint32_t)avail;
    if ((p = (uint32_t *)WP(4))) *p = (uint32_t)total;
    return 1;
#endif
}
uint32_t aret_GetCurrentThread(uint32_t esp)  { (void)esp; return 0xFFFFFFFEu; }
/* GetStartupInfoW: a console process with no inherited startup customization —
 * zero the STARTUPINFOW (68 bytes on 32-bit) and set cb; dwFlags stays 0. */
uint32_t aret_GetStartupInfoW(uint32_t esp) {
    uint8_t *si = (uint8_t *)(uintptr_t)WU(0);
    if (si) { memset(si, 0, 68); *(uint32_t *)si = 68; }
    return 0;
}

/* GetStartupInfoA(LPSTARTUPINFOA) — zero it (cb set); benign for CRT startup. */
uint32_t aret_GetStartupInfoA(uint32_t esp) {
    uint32_t *p = (uint32_t *)WP(0);
    if (p) { memset(p, 0, 68); p[0] = 68; }
    return 0;
}

/* ================================================================== */
/* Cooperative threads (fibers) — doc 80. A Win32 thread becomes a       */
/* coroutine multiplexed on the single host thread; control switches      */
/* ONLY at blocking points (Wait/Sleep/…), so there is never a data race  */
/* and the schedule is round-robin deterministic → the differential       */
/* oracle stays reproducible bit-for-bit. Fiber 0 is the program's main   */
/* thread; absent any CreateThread the scheduler never starts, so a        */
/* single-threaded program is byte-identical to before. `ucontext` is      */
/* libc-native (statically linkable). WASM has no ucontext → CreateThread  */
/* there is a sound abort (Asyncify later), never a silent divergence.     */
/* Per-fiber state that is a plain global while running (last_error) is     */
/* swapped in/out by the scheduler at every context switch.                */
/* ================================================================== */
#ifndef __wasm__
#define U32_MAX_FIBER    64
#define U32_THREAD_BASE  0x70000000u
#define U32_FIBER_MSTACK (1u << 20)     /* 1 MB emulated machine stack per thread */
#define U32_FIBER_HSTACK (4u << 20)     /* 4 MB host C stack for the transpiled code */
enum { FST_FREE = 0, FST_SUSPENDED, FST_READY, FST_RUNNING, FST_BLOCKED, FST_DONE };
struct u32_fiber {
    ucontext_t ctx;
    void      *host_stack;       /* malloc'd host C stack (NULL for fiber 0)        */
    uint8_t   *mstack;           /* malloc'd emulated machine stack (NULL fiber 0)  */
    uint32_t   start, param;     /* thread-proc VA + argument                       */
    uint32_t   exit_code;
    uint32_t   last_error;       /* saved GetLastError() while this fiber is parked  */
    int        state;
    uint32_t   wait_h[64];       /* Wait set (MAXIMUM_WAIT_OBJECTS)                  */
    int        wait_n, wait_all;
    uint32_t   wait_cs;          /* CRITICAL_SECTION being acquired (0 = none)      */
    int        has_timeout;      /* this wait/sleep has a finite deadline           */
    int        timed_out;        /* it was woken by the deadline, not a signal      */
    uint64_t   wake_time;        /* virtual-clock deadline (ms) if has_timeout      */
    int        priority;         /* SetThreadPriority hint (0 = NORMAL); scheduling  */
                                 /* is deterministic round-robin so it only round-   */
                                 /* trips, it does not reorder (see SetThreadPriority)*/
};
static struct u32_fiber g_fiber[U32_MAX_FIBER];
static int g_nfiber = 1;         /* fiber 0 = main, always present */
static int g_cur = 0;            /* running fiber index */
static int g_rr = 0;             /* round-robin cursor */
static ucontext_t g_sched_ctx;   /* scheduler's own context */
static void *g_sched_stack;
static int g_sched_ready;
/* Deterministic virtual clock (ms). It only advances when NO fiber can make
 * progress and a timed wait/Sleep is pending — the scheduler jumps to the earliest
 * deadline and fires it. Independent of wall-clock, so the schedule stays
 * reproducible while finite timeouts (WaitForSingleObject(h, 50), Sleep(100)) get
 * honoured instead of dead-locking. */
static uint64_t g_vclock;
#define U32_INFINITE 0xFFFFFFFFu

/* CRITICAL_SECTION table, keyed by the program's &cs pointer (the struct itself is
 * left opaque, as Wine treats it). owner = fiber index + 1 (0 = free); rec = the
 * recursion depth held by that owner. Under contention a would-be acquirer blocks
 * and the scheduler runs the owner until it fully Leaves. */
#define U32_MAX_CS 256
static struct { uint32_t cs; int owner, rec; } g_cs[U32_MAX_CS];
static int g_ncs;
static int u32_cs_slot(uint32_t cs) {
    for (int i = 0; i < g_ncs; i++) if (g_cs[i].cs == cs) return i;
    return -1;
}
/* Owner fiber index+1 of a CS (0 = free / not yet registered). */
static int u32_cs_owner(uint32_t cs) {
    int s = u32_cs_slot(cs);
    return s < 0 ? 0 : g_cs[s].owner;
}

/* Event objects (doc 80 incr. 3). manual-reset stays signaled until ResetEvent;
 * auto-reset releases exactly one waiter then self-resets (consumed at wait). */
#define U32_EVENT_BASE 0x71000000u
#define U32_MAX_EVENT  128
static struct { int used, manual, signaled; uint32_t name_hash; } g_event[U32_MAX_EVENT];
static int g_nevent;
static int u32_event_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_EVENT_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < (uint32_t)g_nevent && g_event[i].used) ? (int)i : -1;
}
/* FNV hash of an event name (for intra-process named-event sharing); 0 = no name. */
static uint32_t u32_name_hash(const void *p, int wide) {
    if (!p) return 0;
    uint32_t h = 2166136261u;
    if (wide) { const uint16_t *w = p; while (*w) { h ^= (*w & 0xFF); h *= 16777619u; w++; } }
    else      { const uint8_t  *s = p; while (*s) { h ^= *s;          h *= 16777619u; s++; } }
    return h ? h : 1u;
}
static uint32_t u32_create_event(int manual, int initial, uint32_t nh) {
    if (nh) for (int i = 0; i < g_nevent; i++)
        if (g_event[i].used && g_event[i].name_hash == nh) {
            g_last_error = 183u /*ERROR_ALREADY_EXISTS*/; return U32_EVENT_BASE | (uint32_t)i;
        }
    if (g_nevent >= U32_MAX_EVENT) { g_last_error = 8u; return 0; }
    int i = g_nevent++;
    g_event[i].used = 1; g_event[i].manual = manual ? 1 : 0;
    g_event[i].signaled = initial ? 1 : 0; g_event[i].name_hash = nh;
    return U32_EVENT_BASE | (uint32_t)i;
}

/* Mutex objects (doc 80 incr. 4): ownable, recursive, waitable. owner = fiber
 * index+1 (0 = free); an owner that exits without releasing => abandoned. */
#define U32_MUTEX_BASE 0x72000000u
#define U32_MAX_MUTEX  128
static struct { int used, owner, rec; uint32_t name_hash; } g_mutex[U32_MAX_MUTEX];
static int g_nmutex;
static int u32_mutex_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_MUTEX_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < (uint32_t)g_nmutex && g_mutex[i].used) ? (int)i : -1;
}
/* Semaphore objects (doc 80 incr. 4): a bounded counter; wait decrements, release
 * increments (capped at max). Signaled when count > 0. */
#define U32_SEM_BASE 0x73000000u
#define U32_MAX_SEM  128
static struct { int used, count, max; uint32_t name_hash; } g_sem[U32_MAX_SEM];
static int g_nsem;
static int u32_sem_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_SEM_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < (uint32_t)g_nsem && g_sem[i].used) ? (int)i : -1;
}

/* handle -> fiber index, or -1 if it is not one of our thread handles. */
static int u32_thread_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_THREAD_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < (uint32_t)g_nfiber) ? (int)i : -1;
}
/* Resolve a thread handle to a fiber index, honouring the GetCurrentThread()
 * pseudo-handle (-2 = 0xFFFFFFFE, which means "the calling thread"). */
static int u32_thread_resolve(uint32_t h) {
    if (h == 0xFFFFFFFEu) return g_cur;
    return u32_thread_idx(h);
}
/* Is a wait handle signaled *for fiber fi*? Thread → its fiber is DONE; event →
 * signaled flag; mutex → free, held by fi (recursive), or abandoned (owner DONE);
 * semaphore → count>0. Any other handle keeps the legacy always-signaled value
 * (sound in the mono-thread model). */
static int u32_handle_signaled_for(uint32_t h, int fi) {
    int ti = u32_thread_idx(h);
    if (ti >= 0) return g_fiber[ti].state == FST_DONE;
    int ei = u32_event_idx(h);
    if (ei >= 0) return g_event[ei].signaled;
    int mi = u32_mutex_idx(h);
    if (mi >= 0) {
        int o = g_mutex[mi].owner;
        return o == 0 || o == fi + 1 || g_fiber[o - 1].state == FST_DONE;
    }
    int si = u32_sem_idx(h);
    if (si >= 0) return g_sem[si].count > 0;
    return 1;
}
static int u32_wait_ok(struct u32_fiber *f) {
    int fi = (int)(f - g_fiber);
    if (f->wait_n == 0) return 1;
    if (f->wait_all) {
        for (int i = 0; i < f->wait_n; i++) if (!u32_handle_signaled_for(f->wait_h[i], fi)) return 0;
        return 1;
    }
    for (int i = 0; i < f->wait_n; i++) if (u32_handle_signaled_for(f->wait_h[i], fi)) return 1;
    return 0;
}
/* Acquire (consume) a satisfying handle for fiber `me`: auto-reset event resets;
 * mutex takes ownership (recursive); semaphore decrements. Manual event/thread =
 * no side effect. */
static void u32_handle_acquire(uint32_t h, int me) {
    int ei = u32_event_idx(h);
    if (ei >= 0) { if (!g_event[ei].manual) g_event[ei].signaled = 0; return; }
    int mi = u32_mutex_idx(h);
    if (mi >= 0) { g_mutex[mi].owner = me + 1; g_mutex[mi].rec++; return; }
    int si = u32_sem_idx(h);
    if (si >= 0) { if (g_sem[si].count > 0) g_sem[si].count--; return; }
}
/* A blocked fiber is signal-runnable when its wait condition clears: a CRITICAL_
 * SECTION it wants is free, or its handle wait set is satisfied. A *pure* timed
 * sleep (no handles) is never signal-runnable — only the virtual clock wakes it. */
static int u32_fiber_runnable(int i) {
    struct u32_fiber *f = &g_fiber[i];
    if (f->wait_cs) { int o = u32_cs_owner(f->wait_cs); return o == 0 || o == i + 1; }
    if (f->wait_n == 0) return 0;                 /* pure Sleep → woken by the clock only */
    return u32_wait_ok(f);
}
static void u32_sched_loop(void) {
    for (;;) {
        /* Wake any blocked fiber whose wait condition is now satisfied (by signal). */
        for (int i = 0; i < g_nfiber; i++)
            if (g_fiber[i].state == FST_BLOCKED && u32_fiber_runnable(i)) {
                g_fiber[i].state = FST_READY; g_fiber[i].timed_out = 0;
            }
        /* Pick the next READY fiber round-robin (starting AFTER the last one). */
        int pick = -1;
        for (int k = 1; k <= g_nfiber; k++) {
            int i = (g_rr + k) % g_nfiber;
            if (g_fiber[i].state == FST_READY) { pick = i; break; }
        }
        if (pick < 0) {
            /* Nothing signal-runnable. If any fiber has a finite deadline, advance
             * the virtual clock to the earliest and fire the timed-out ones — this
             * is how finite timeouts (and Sleep) resolve deterministically instead
             * of dead-locking. Only an all-infinite block is a true deadlock. */
            uint64_t earliest = (uint64_t)-1;
            for (int i = 0; i < g_nfiber; i++)
                if (g_fiber[i].state == FST_BLOCKED && g_fiber[i].has_timeout && g_fiber[i].wake_time < earliest)
                    earliest = g_fiber[i].wake_time;
            if (earliest != (uint64_t)-1) {
                g_vclock = earliest;
                for (int i = 0; i < g_nfiber; i++)
                    if (g_fiber[i].state == FST_BLOCKED && g_fiber[i].has_timeout && g_fiber[i].wake_time <= g_vclock) {
                        g_fiber[i].state = FST_READY; g_fiber[i].timed_out = 1;
                    }
                continue;                          /* re-loop: some are READY now */
            }
            int blocked = 0;
            for (int i = 0; i < g_nfiber; i++) if (g_fiber[i].state == FST_BLOCKED) blocked = 1;
            if (blocked) aret_unmodelled("fiber scheduler: deadlock (all live threads blocked, no timeout pending)");
            return;   /* nothing runnable and nothing blocked → every fiber is DONE */
        }
        g_rr = pick; g_cur = pick; g_fiber[pick].state = FST_RUNNING;
        g_last_error = g_fiber[pick].last_error;      /* make its last-error current */
        swapcontext(&g_sched_ctx, &g_fiber[pick].ctx);
    }
}
static void u32_sched_ensure(void) {
    if (g_sched_ready) return;
    g_sched_ready = 1;
    g_sched_stack = malloc(U32_FIBER_HSTACK);
    getcontext(&g_sched_ctx);
    g_sched_ctx.uc_stack.ss_sp = g_sched_stack;
    g_sched_ctx.uc_stack.ss_size = U32_FIBER_HSTACK;
    g_sched_ctx.uc_link = NULL;
    makecontext(&g_sched_ctx, u32_sched_loop, 0);
}
/* Park the running fiber and return control to the scheduler (saving its
 * last-error). Resumes here when the scheduler next selects this fiber. */
static void u32_to_sched(void) {
    int me = g_cur;
    g_fiber[me].last_error = g_last_error;
    swapcontext(&g_fiber[me].ctx, &g_sched_ctx);
}
#define U32_WAIT_TIMEOUT 0x102u
/* Block the running fiber on a wait set until satisfied or the timeout expires.
 * ms == 0 polls (→ WAIT_TIMEOUT if not already satisfied); ms == INFINITE never
 * times out; a finite ms registers a virtual-clock deadline the scheduler honours
 * (so timeout-driven code resolves deterministically instead of dead-locking).
 * Returns WAIT_OBJECT_0 + idx, or WAIT_TIMEOUT. */
static uint32_t u32_wait(const uint32_t *handles, int n, int all, uint32_t ms) {
    struct u32_fiber *f = &g_fiber[g_cur];
    if (n > 64) n = 64;
    f->wait_n = n; f->wait_all = all;
    for (int i = 0; i < n; i++) f->wait_h[i] = handles[i];
    if (!u32_wait_ok(f)) {
        if (ms == 0) { f->wait_n = 0; return U32_WAIT_TIMEOUT; }   /* poll */
        u32_sched_ensure();
        f->has_timeout = (ms != U32_INFINITE);
        f->wake_time = g_vclock + ms;
        f->timed_out = 0;
        /* Re-check after every wake: with auto-reset events several waiters may be
         * woken for one signal, but only the one that still finds it satisfied may
         * proceed (and consume it below); the others re-block. A timeout wake ends
         * the loop with WAIT_TIMEOUT. */
        for (;;) {
            f->state = FST_BLOCKED; u32_to_sched();
            if (u32_wait_ok(f)) break;
            if (f->timed_out) { f->has_timeout = 0; f->wait_n = 0; return U32_WAIT_TIMEOUT; }
        }
        f->has_timeout = 0;
    }
    f->wait_n = 0;
    /* Acquire the satisfying handle(s): waitAll consumes all, waitAny only the
     * first signaled (the index we report). Auto-reset event resets, mutex takes
     * ownership, semaphore decrements. */
    int me = g_cur, idx = 0;
    if (!all) for (int i = 0; i < n; i++) if (u32_handle_signaled_for(handles[i], me)) { idx = i; break; }
    if (all) { for (int i = 0; i < n; i++) u32_handle_acquire(handles[i], me); }
    else       u32_handle_acquire(handles[idx], me);
    return (uint32_t)idx;              /* WAIT_OBJECT_0 + idx */
}
/* Sleep(ms) with threads live: block on the virtual clock for `ms`, letting other
 * fibers run. A pure timed block (no handles) — only the clock wakes it. */
static void u32_sleep(uint32_t ms) {
    u32_sched_ensure();
    struct u32_fiber *f = &g_fiber[g_cur];
    f->wait_n = 0; f->wait_cs = 0;
    f->has_timeout = 1; f->wake_time = g_vclock + ms; f->timed_out = 0;
    do { f->state = FST_BLOCKED; u32_to_sched(); } while (!f->timed_out);
    f->has_timeout = 0;
}
/* Trampoline a fresh fiber: lay a __stdcall thread-proc frame on its machine
 * stack and dispatch the lifted proc; on return, record the exit code, mark the
 * fiber DONE, and fall through to uc_link (the scheduler). */
static void u32_fiber_trampoline(void) {
    struct u32_fiber *f = &g_fiber[g_cur];
    uint32_t *sp = (uint32_t *)(uintptr_t)(f->mstack + U32_FIBER_MSTACK - 64);
    sp[0] = 0;                          /* return address (proc never returns here) */
    sp[1] = f->param;                   /* LPVOID lpParameter @ [esp+4]             */
    uint64_t r = aret_call(f->start, (uint64_t)(uintptr_t)sp, 0, 0, 0, 0, 0, 0, 0);
    f->exit_code = (uint32_t)r;
    f->state = FST_DONE;
    /* returns into uc_link = g_sched_ctx */
}
/* Sleep(ms) as a fiber operation. Returns 0 when there is no thread (so the caller
 * keeps its non-threaded usleep). ms == 0 is a plain yield; ms > 0 blocks on the
 * virtual clock, letting other fibers run first. */
int aret_fiber_sleep(uint32_t ms) {
    if (!g_sched_ready) return 0;
    if (ms == 0) { g_fiber[g_cur].state = FST_READY; u32_to_sched(); }
    else u32_sleep(ms);
    return 1;
}
/* CreateThread(lpsa, dwStackSize, lpStartAddress, lpParameter, dwFlags, lpThreadId). */
/* Spawn a fiber for a thread proc (shared by CreateThread and _beginthread(ex)). */
static uint32_t u32_spawn(uint32_t start, uint32_t param, uint32_t flags, uint32_t pTid) {
    if (g_nfiber >= U32_MAX_FIBER) { g_last_error = 8u /*NOT_ENOUGH_MEMORY*/; return 0; }
    int i = g_nfiber;
    struct u32_fiber *f = &g_fiber[i];
    memset(f, 0, sizeof *f);
    f->start = start; f->param = param;
    f->host_stack = malloc(U32_FIBER_HSTACK);
    f->mstack = (uint8_t *)malloc(U32_FIBER_MSTACK);
    if (!f->host_stack || !f->mstack) { free(f->host_stack); free(f->mstack); g_last_error = 8u; return 0; }
    u32_sched_ensure();
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->host_stack;
    f->ctx.uc_stack.ss_size = U32_FIBER_HSTACK;
    f->ctx.uc_link = &g_sched_ctx;
    makecontext(&f->ctx, u32_fiber_trampoline, 0);
    f->state = (flags & 0x4u /*CREATE_SUSPENDED*/) ? FST_SUSPENDED : FST_READY;
    g_nfiber++;                        /* publish only once fully built */
    if (pTid) *(uint32_t *)(uintptr_t)pTid = 0x1000u + (uint32_t)i;
    return U32_THREAD_BASE | (uint32_t)i;
}
uint32_t aret_CreateThread(uint32_t esp) {
    return u32_spawn(WU(2), WU(3), WU(4), WU(5));
}
/* _beginthreadex(security, stacksize, start, arglist, initflag, thrdaddr) — msvcrt
 * CRT wrapper; identical arg layout and __stdcall proc to CreateThread. */
uint32_t aret_beginthreadex(uint32_t esp) {
    return u32_spawn(WU(2), WU(3), WU(4), WU(5));
}
/* _beginthread(start, stacksize, arglist) — the __cdecl variant; the trampoline's
 * frame (param at [esp+4]) serves cdecl and stdcall procs alike. */
uint32_t aret_beginthread(uint32_t esp) {
    return u32_spawn(WU(0), WU(2), 0, 0);
}
uint32_t aret_ResumeThread(uint32_t esp) {
    int fi = u32_thread_idx(WU(0)); if (fi < 0) return (uint32_t)-1;
    if (g_fiber[fi].state == FST_SUSPENDED) { g_fiber[fi].state = FST_READY; return 1; }
    return 0;                          /* was not suspended */
}
uint32_t aret_SuspendThread(uint32_t esp) {
    (void)esp;
    aret_unmodelled("SuspendThread: cooperative fibers cannot pre-empt a running thread");
    return (uint32_t)-1;
}
uint32_t aret_ExitThread(uint32_t esp) {
    uint32_t code = WU(0);
    if (g_cur == 0) { aret_ExitProcess(esp); return 0; }   /* main thread → exit process */
    g_fiber[g_cur].exit_code = code;
    g_fiber[g_cur].last_error = g_last_error;
    g_fiber[g_cur].state = FST_DONE;
    swapcontext(&g_fiber[g_cur].ctx, &g_sched_ctx);        /* never resumed */
    return 0;
}
/* CreateProcess(A/W): launching a Windows child .exe has no faithful native model
 * (there is no Windows to run it), so this is a SOUND FAILURE, never a simulation and
 * never an abort — return 0 (FALSE) and set a plausible last error, exactly the shape
 * a real CreateProcess gives when it cannot start the image. A caller checks the
 * BOOL, sees failure, and takes its error path (doctrine: doc 70 §4.5/§8.3). The 27/37
 * Win95 binaries importing it thus continue gracefully instead of aborting. */
uint32_t aret_CreateProcessA(uint32_t esp) {
    (void)esp; g_last_error = 2u /* ERROR_FILE_NOT_FOUND */; return 0;
}
uint32_t aret_CreateProcessW(uint32_t esp) {
    (void)esp; g_last_error = 2u; return 0;
}
uint32_t aret_GetExitCodeThread(uint32_t esp) {
    int fi = u32_thread_idx(WU(0));
    uint32_t *out = (uint32_t *)WP(1);
    if (fi < 0) { if (out) *out = 0; return 0; }
    if (out) *out = (g_fiber[fi].state == FST_DONE) ? g_fiber[fi].exit_code : 259u /*STILL_ACTIVE*/;
    return 1;
}
/* SetThreadPriority / GetThreadPriority. Priority is a *scheduling hint*: our
 * cooperative scheduler is deterministic round-robin (a program that relied on
 * priority to order threads would be racy under real Windows too), so we only
 * round-trip the value — store it, hand it back — without reordering. Verified vs
 * Wine: default = 0 (THREAD_PRIORITY_NORMAL), Set returns TRUE, Get returns the set
 * value. Unknown handle -> GetThreadPriority returns THREAD_PRIORITY_ERROR_RETURN. */
uint32_t aret_SetThreadPriority(uint32_t esp) {
    int fi = u32_thread_resolve(WU(0)); if (fi < 0) return 0;
    g_fiber[fi].priority = (int)WU(1);
    return 1;
}
uint32_t aret_GetThreadPriority(uint32_t esp) {
    int fi = u32_thread_resolve(WU(0));
    if (fi < 0) return 0x7FFFFFFFu;                     /* THREAD_PRIORITY_ERROR_RETURN */
    return (uint32_t)g_fiber[fi].priority;
}
/* OpenProcess: the only process that exists here is our own (CreateProcess is a
 * sound failure, doc 70 §4.5), so opening our own pid succeeds (own-process handle)
 * and any other pid fails with ERROR_INVALID_PARAMETER — exactly Wine's shape (own
 * ok, bogus -> 0 / err 87). */
uint32_t aret_OpenProcess(uint32_t esp) {
    if (WU(2) == (uint32_t)getpid()) return 0xFFFFFFFFu;   /* own process pseudo-handle */
    g_last_error = 87u /* ERROR_INVALID_PARAMETER */;
    return 0;
}
/* TerminateThread(hThread, exitCode): forcibly end a thread. Cooperative model: the
 * target fiber is marked DONE with the given exit code and is never scheduled again
 * (its stacks leak, exactly as Windows leaks the terminated thread's resources — the
 * documented danger). Waiters see it signaled; GetExitCodeThread returns exitCode.
 * Terminating the calling thread degenerates to ExitThread / process-exit. If the
 * victim still held a lock, no other fiber can ever acquire it -> the scheduler's
 * deadlock detector aborts loudly (a Windows program would hang identically). */
uint32_t aret_TerminateThread(uint32_t esp) {
    int fi = u32_thread_resolve(WU(0)); if (fi < 0) return 0;
    uint32_t code = WU(1);
    if (fi == g_cur) {                                  /* terminating self */
        if (g_cur == 0) { exit((int)code); }            /* main thread -> process exit */
        g_fiber[g_cur].exit_code = code;
        g_fiber[g_cur].last_error = g_last_error;
        g_fiber[g_cur].state = FST_DONE;
        swapcontext(&g_fiber[g_cur].ctx, &g_sched_ctx); /* never resumed */
        return 1;                                       /* unreachable */
    }
    g_fiber[fi].exit_code = code;
    g_fiber[fi].state = FST_DONE;
    return 1;
}
/* WaitForInputIdle: valid only for a child GUI process one created. We never create
 * real processes (CreateProcess is a sound failure), so every handle here is our own
 * or invalid -> WAIT_FAILED + ERROR_INVALID_HANDLE, exactly Wine's headless shape
 * (0xFFFFFFFF, err 6). Never a fake "idle" success. */
uint32_t aret_WaitForInputIdle(uint32_t esp) {
    (void)esp;
    g_last_error = 6u /* ERROR_INVALID_HANDLE */;
    return 0xFFFFFFFFu /* WAIT_FAILED */;
}
/* WinHelp: HELP_QUIT (close the help window) succeeds with nothing open; any other
 * command needs a help viewer (winhlp32) that does not exist headless -> FALSE.
 * Measured vs Wine (HELP_CONTEXT=0, HELP_QUIT=1). Sound: never fakes that help was
 * shown. */
uint32_t aret_WinHelpA(uint32_t esp) { return (WU(2) == 2u /*HELP_QUIT*/) ? 1u : 0u; }
uint32_t aret_WinHelpW(uint32_t esp) { return aret_WinHelpA(esp); }

/* Printer spooler (winspool.drv), display-free. Headless there are NO printers (no
 * spooler / CUPS), and that state is deterministic, so these model the exact "no
 * printer" results Wine gives — never a fake printer:
 *   - OpenPrinter(NULL) opens the local print server (a valid handle); a named
 *     printer/server does not exist -> FALSE + ERROR_INVALID_PRINTER_NAME.
 *   - ClosePrinter succeeds on a handle we issued, else ERROR_INVALID_HANDLE.
 *   - EnumPrinters returns TRUE with 0 needed / 0 returned (empty).
 *   - GetDefaultPrinter -> FALSE + ERROR_FILE_NOT_FOUND (no default), pcchBuffer left
 *     untouched. All measured bit-exact vs Wine. A program then takes its no-printer
 *     path gracefully instead of aborting. */
#define U32_PRINTER_BASE 0x74000000u
static uint32_t g_printer_next = 1;
static int u32_printer_h(uint32_t h) { return (h & 0xFF000000u) == U32_PRINTER_BASE && (h & 0x00FFFFFFu) != 0; }
uint32_t aret_OpenPrinterA(uint32_t esp) {
    uint32_t *ph = (uint32_t *)WP(1);
    if (WU(0) == 0) {                               /* NULL name = local print server */
        uint32_t h = U32_PRINTER_BASE | (g_printer_next & 0x00FFFFFFu);
        g_printer_next = (g_printer_next & 0x00FFFFFFu) == 0x00FFFFFFu ? 1 : g_printer_next + 1;
        if (ph) *ph = h;
        return 1;
    }
    if (ph) *ph = 0;
    g_last_error = 1801u /* ERROR_INVALID_PRINTER_NAME */;
    return 0;
}
uint32_t aret_OpenPrinterW(uint32_t esp) { return aret_OpenPrinterA(esp); }
uint32_t aret_ClosePrinter(uint32_t esp) {
    if (u32_printer_h(WU(0))) return 1;
    g_last_error = 6u /* ERROR_INVALID_HANDLE */;
    return 0;
}
uint32_t aret_EnumPrintersA(uint32_t esp) {
    uint32_t *needed = (uint32_t *)WP(5), *ret = (uint32_t *)WP(6);
    if (needed) *needed = 0;
    if (ret) *ret = 0;
    return 1;                                       /* empty: 0 bytes, 0 printers */
}
uint32_t aret_EnumPrintersW(uint32_t esp) { return aret_EnumPrintersA(esp); }
uint32_t aret_GetDefaultPrinterA(uint32_t esp) {
    (void)esp; g_last_error = 2u /* ERROR_FILE_NOT_FOUND */; return 0;
}
uint32_t aret_GetDefaultPrinterW(uint32_t esp) { return aret_GetDefaultPrinterA(esp); }
/* A handle this layer actually waits on (thread or event); others keep the legacy
 * immediate WAIT_OBJECT_0 (sound in the mono-thread model). */
static int u32_waitable(uint32_t h) {
    return u32_thread_idx(h) >= 0 || u32_event_idx(h) >= 0 || u32_mutex_idx(h) >= 0 || u32_sem_idx(h) >= 0;
}
uint32_t aret_WaitForSingleObject(uint32_t esp) {
    uint32_t h = WU(0);
    if (!u32_waitable(h)) return 0;
    uint32_t r = u32_wait(&h, 1, 1, WU(1));                /* WU(1) = timeout ms */
    return (r == U32_WAIT_TIMEOUT) ? r : 0;                /* WAIT_OBJECT_0 */
}
uint32_t aret_WaitForMultipleObjects(uint32_t esp) {
    uint32_t n = WU(0), ph = WU(1); int all = WI(2);
    const uint32_t *h = (const uint32_t *)(uintptr_t)ph;
    if (!h || n == 0) return 0;
    int any = 0;
    for (uint32_t i = 0; i < n; i++) if (u32_waitable(h[i])) any = 1;
    if (!any) return 0;                                    /* legacy immediate WAIT_OBJECT_0 */
    return u32_wait(h, (int)n, all, WU(3));                /* WU(3) = timeout ms */
}
/* -------- CRITICAL_SECTION (doc 80 incr. 2) -------- */
/* Cooperative fibers: only one fiber runs at a time, but a fiber that yields
 * (Sleep/Wait) while holding a CS must keep others out — so Enter blocks a
 * different-owner acquirer until the owner fully Leaves. Recursive by owner. */
static int u32_cs_ensure(uint32_t cs) {
    int s = u32_cs_slot(cs);
    if (s >= 0) return s;
    if (g_ncs >= U32_MAX_CS) { aret_unmodelled("CriticalSection: table full"); return -1; }
    s = g_ncs++;
    g_cs[s].cs = cs; g_cs[s].owner = 0; g_cs[s].rec = 0;
    return s;
}
uint32_t aret_InitializeCriticalSection(uint32_t esp) { u32_cs_ensure(WU(0)); return 0; }
uint32_t aret_InitializeCriticalSectionAndSpinCount(uint32_t esp) { u32_cs_ensure(WU(0)); return 1; }
uint32_t aret_InitializeCriticalSectionEx(uint32_t esp) { u32_cs_ensure(WU(0)); return 1; }
uint32_t aret_DeleteCriticalSection(uint32_t esp) {
    int s = u32_cs_slot(WU(0));
    if (s >= 0) g_cs[s] = g_cs[--g_ncs];              /* compact (pointers stay stable) */
    return 0;
}
uint32_t aret_EnterCriticalSection(uint32_t esp) {
    uint32_t cs = WU(0);
    int me = g_cur, s = u32_cs_ensure(cs);
    if (s < 0) return 0;
    for (;;) {
        if (g_cs[s].owner == 0 || g_cs[s].owner == me + 1) {
            g_cs[s].owner = me + 1; g_cs[s].rec++;
            return 0;
        }
        u32_sched_ensure();                            /* owned by another → block & retry */
        g_fiber[me].wait_cs = cs;
        g_fiber[me].state = FST_BLOCKED;
        u32_to_sched();
        g_fiber[me].wait_cs = 0;
        s = u32_cs_slot(cs);                           /* table may have compacted */
        if (s < 0) return 0;
    }
}
uint32_t aret_TryEnterCriticalSection(uint32_t esp) {
    int me = g_cur, s = u32_cs_ensure(WU(0));
    if (s < 0) return 0;
    if (g_cs[s].owner == 0 || g_cs[s].owner == me + 1) {
        g_cs[s].owner = me + 1; g_cs[s].rec++;
        return 1;                                      /* acquired */
    }
    return 0;                                          /* held by another fiber */
}
uint32_t aret_LeaveCriticalSection(uint32_t esp) {
    int s = u32_cs_slot(WU(0));
    if (s >= 0 && g_cs[s].owner == g_cur + 1 && g_cs[s].rec > 0)
        if (--g_cs[s].rec == 0) g_cs[s].owner = 0;     /* fully released → waiters may take it */
    return 0;
}
/* -------- Event objects (doc 80 incr. 3) -------- */
uint32_t aret_CreateEventA(uint32_t esp) { return u32_create_event(WI(1), WI(2), u32_name_hash(WP(3), 0)); }
uint32_t aret_CreateEventW(uint32_t esp) { return u32_create_event(WI(1), WI(2), u32_name_hash(WP(3), 1)); }
uint32_t aret_SetEvent(uint32_t esp) {
    int ei = u32_event_idx(WU(0));
    if (ei >= 0) g_event[ei].signaled = 1;             /* auto-reset: a waiter consumes it */
    return 1;
}
uint32_t aret_ResetEvent(uint32_t esp) {
    int ei = u32_event_idx(WU(0));
    if (ei >= 0) g_event[ei].signaled = 0;
    return 1;
}
/* -------- Mutex objects (doc 80 incr. 4) -------- */
static uint32_t u32_create_mutex(int initial_owner, uint32_t nh) {
    if (nh) for (int i = 0; i < g_nmutex; i++)
        if (g_mutex[i].used && g_mutex[i].name_hash == nh) { g_last_error = 183u; return U32_MUTEX_BASE | (uint32_t)i; }
    if (g_nmutex >= U32_MAX_MUTEX) { g_last_error = 8u; return 0; }
    int i = g_nmutex++;
    g_mutex[i].used = 1; g_mutex[i].owner = initial_owner ? (g_cur + 1) : 0;
    g_mutex[i].rec = initial_owner ? 1 : 0; g_mutex[i].name_hash = nh;
    return U32_MUTEX_BASE | (uint32_t)i;
}
static uint32_t u32_open_mutex(uint32_t nh) {
    if (nh) for (int i = 0; i < g_nmutex; i++)
        if (g_mutex[i].used && g_mutex[i].name_hash == nh) return U32_MUTEX_BASE | (uint32_t)i;
    g_last_error = 2u; return 0;                       /* ERROR_FILE_NOT_FOUND */
}
uint32_t aret_CreateMutexA(uint32_t esp) { return u32_create_mutex(WI(1), u32_name_hash(WP(2), 0)); }
uint32_t aret_CreateMutexW(uint32_t esp) { return u32_create_mutex(WI(1), u32_name_hash(WP(2), 1)); }
uint32_t aret_OpenMutexA(uint32_t esp)   { return u32_open_mutex(u32_name_hash(WP(2), 0)); }
uint32_t aret_OpenMutexW(uint32_t esp)   { return u32_open_mutex(u32_name_hash(WP(2), 1)); }
uint32_t aret_ReleaseMutex(uint32_t esp) {
    int mi = u32_mutex_idx(WU(0));
    if (mi < 0 || g_mutex[mi].owner != g_cur + 1) return 0;   /* not the owner */
    if (g_mutex[mi].rec > 0 && --g_mutex[mi].rec == 0) g_mutex[mi].owner = 0;
    return 1;
}
/* -------- Semaphore objects (doc 80 incr. 4) -------- */
static uint32_t u32_create_sem(int initial, int maximum, uint32_t nh) {
    if (nh) for (int i = 0; i < g_nsem; i++)
        if (g_sem[i].used && g_sem[i].name_hash == nh) { g_last_error = 183u; return U32_SEM_BASE | (uint32_t)i; }
    if (g_nsem >= U32_MAX_SEM) { g_last_error = 8u; return 0; }
    int i = g_nsem++;
    g_sem[i].used = 1; g_sem[i].count = initial; g_sem[i].max = maximum; g_sem[i].name_hash = nh;
    return U32_SEM_BASE | (uint32_t)i;
}
static uint32_t u32_open_sem(uint32_t nh) {
    if (nh) for (int i = 0; i < g_nsem; i++)
        if (g_sem[i].used && g_sem[i].name_hash == nh) return U32_SEM_BASE | (uint32_t)i;
    g_last_error = 2u; return 0;
}
uint32_t aret_CreateSemaphoreA(uint32_t esp) { return u32_create_sem(WI(1), WI(2), u32_name_hash(WP(3), 0)); }
uint32_t aret_CreateSemaphoreW(uint32_t esp) { return u32_create_sem(WI(1), WI(2), u32_name_hash(WP(3), 1)); }
uint32_t aret_OpenSemaphoreA(uint32_t esp)   { return u32_open_sem(u32_name_hash(WP(2), 0)); }
uint32_t aret_OpenSemaphoreW(uint32_t esp)   { return u32_open_sem(u32_name_hash(WP(2), 1)); }
uint32_t aret_ReleaseSemaphore(uint32_t esp) {
    int si = u32_sem_idx(WU(0));
    if (si < 0) return 0;
    int rel = WI(1);
    if (rel <= 0 || g_sem[si].count + rel > g_sem[si].max) { g_last_error = 298u /*TOO_MANY_POSTS*/; return 0; }
    uint32_t pPrev = WU(2);
    if (pPrev) *(int32_t *)(uintptr_t)pPrev = g_sem[si].count;
    g_sem[si].count += rel;
    return 1;
}
/* Current fiber index (0 = main / no threads) — used by per-fiber TLS in aret_hle.c. */
int aret_current_fiber(void) { return g_cur; }
#else  /* __wasm__ : no ucontext. Threads are a sound abort, never a fake. */
int aret_fiber_sleep(uint32_t ms) { (void)ms; return 0; }
int aret_current_fiber(void) { return 0; }
uint32_t aret_CreateEventA(uint32_t esp) { (void)esp; return 0x101; }
uint32_t aret_CreateEventW(uint32_t esp) { (void)esp; return 0x101; }
uint32_t aret_SetEvent(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_ResetEvent(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_CreateMutexA(uint32_t esp) { (void)esp; return 0x100; }
uint32_t aret_CreateMutexW(uint32_t esp) { (void)esp; return 0x100; }
uint32_t aret_OpenMutexA(uint32_t esp) { (void)esp; return 0x100; }
uint32_t aret_OpenMutexW(uint32_t esp) { (void)esp; return 0x100; }
uint32_t aret_ReleaseMutex(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_CreateSemaphoreA(uint32_t esp) { (void)esp; return 0x102; }
uint32_t aret_CreateSemaphoreW(uint32_t esp) { (void)esp; return 0x102; }
uint32_t aret_OpenSemaphoreA(uint32_t esp) { (void)esp; return 0x102; }
uint32_t aret_OpenSemaphoreW(uint32_t esp) { (void)esp; return 0x102; }
uint32_t aret_ReleaseSemaphore(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_beginthreadex(uint32_t esp) { (void)esp; aret_unmodelled("_beginthreadex: WASM has no ucontext"); return 0; }
uint32_t aret_beginthread(uint32_t esp) { (void)esp; aret_unmodelled("_beginthread: WASM has no ucontext"); return 0; }
/* No threads under WASM → CriticalSection is a correct no-op (never contended). */
uint32_t aret_InitializeCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_InitializeCriticalSectionAndSpinCount(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_InitializeCriticalSectionEx(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_DeleteCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_EnterCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_TryEnterCriticalSection(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_LeaveCriticalSection(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_CreateThread(uint32_t esp) { (void)esp; aret_unmodelled("CreateThread: WASM has no ucontext (Asyncify pending)"); return 0; }
uint32_t aret_ResumeThread(uint32_t esp) { (void)esp; return (uint32_t)-1; }
uint32_t aret_SuspendThread(uint32_t esp) { (void)esp; return (uint32_t)-1; }
uint32_t aret_ExitThread(uint32_t esp) { return aret_ExitProcess(esp); }
uint32_t aret_GetExitCodeThread(uint32_t esp) { uint32_t *o = (uint32_t *)WP(1); if (o) *o = 0; return 0; }
uint32_t aret_WaitForSingleObject(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_WaitForMultipleObjects(uint32_t esp) { (void)esp; return 0; }
#endif /* __wasm__ */

/* ------------------------------------------------------------------ */
/* Synchronisation / handles (single-process model)                   */
/* ------------------------------------------------------------------ */

/* CreateMutex{A,W}/OpenMutex{A,W}/ReleaseMutex + Semaphores = real waitable objects
 * (doc 80 incr. 4), defined with the fiber scheduler above. */
/* CreateEvent{A,W}/SetEvent/ResetEvent = real event objects (doc 80 incr. 3),
 * defined with the fiber scheduler above. */
uint32_t aret_SetConsoleCtrlHandler(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_FlushFileBuffers(uint32_t esp)    { (void)esp; return 1; }
/* File byte-range locks (LockFile/LockFileEx and their Unlock counterparts):
 * grant unconditionally. We run a single translated process against its own
 * files, so there is no other locker to contend with; sqlite's Win32 VFS calls
 * LockFile before every read/write and reports "database is locked" if it
 * returns 0 (the weak stub's value), so a real success is required to write. */
uint32_t aret_LockFile(uint32_t esp)            { (void)esp; return 1; }
uint32_t aret_LockFileEx(uint32_t esp)          { (void)esp; return 1; }
uint32_t aret_UnlockFile(uint32_t esp)          { (void)esp; return 1; }
uint32_t aret_UnlockFileEx(uint32_t esp)        { (void)esp; return 1; }
uint32_t aret_SetHandleCount(uint32_t esp)      { return WU(0); }
/* GetFileType(handle): map the fd's kind to the Win32 constant so a CLI tool can
 * tell a TTY (CHAR) from a pipe/redirect (PIPE) from a file (DISK). */
uint32_t aret_GetFileType(uint32_t esp) {
    struct stat st;
    if (fstat((int)WU(0), &st) != 0) return 0; /* FILE_TYPE_UNKNOWN */
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return 3; /* FILE_TYPE_PIPE */
    if (S_ISCHR(st.st_mode)) return 2;                          /* FILE_TYPE_CHAR */
    return 1;                                                   /* FILE_TYPE_DISK */
}

/* Console probes. When stdout is a real terminal these would succeed; when it is
 * a pipe/file they fail on Windows too. Reporting failure (0) makes a console
 * program treat output as non-interactive — the correct, side-effect-free choice
 * (plain output, no cursor/colour control). aret_GetStdHandle returns the fd
 * itself (0/1/2) as the HANDLE, so a console HANDLE is just its fd. */
static int aret_handle_fd(uint32_t h) { return (h <= 2u) ? (int)h : -1; }
/* msvcrt's OS-handle <-> CRT-fd bridge. On Linux a handle and an fd are the same
 * integer, so both are the identity — but they must exist: the weak stub returned
 * -1, and BusyBox added a length to that, dereferenced ~0x1, and crashed. */
uint32_t aret_open_osfhandle(uint32_t esp) { return WU(0); }
uint32_t aret_get_osfhandle(uint32_t esp) { return WU(0); }
uint32_t aret_SetErrorMode(uint32_t esp)                 { (void)esp; return 0; }
/* Console code pages: a process with no console (output redirected to a pipe/
 * file, as under the differential harness) reports 0, like Windows/Wine; a real
 * console reports a code page (the US OEM default). */
uint32_t aret_GetConsoleCP(uint32_t esp) {
    (void)esp;
    return (isatty(0) || isatty(1) || isatty(2)) ? 437u : 0u;
}
uint32_t aret_GetConsoleOutputCP(uint32_t esp) { return aret_GetConsoleCP(esp); }
/* WriteConsoleW writes only to a real console; on a redirected handle it fails
 * (the caller then falls back to WriteFile) — match that so output isn't doubled. */
uint32_t aret_WriteConsoleW(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(1);
    uint32_t n = WU(2);
    char tmp[8192];
    uint32_t i = 0;
    for (; i < n && i < sizeof tmp; i++) tmp[i] = (char)(s[i] & 0xff);
    ssize_t w = write(fd, tmp, i);
    uint32_t *pw = (uint32_t *)(uintptr_t)WU(3);
    if (pw) *pw = w > 0 ? (uint32_t)w : 0;
    return w >= 0 ? 1 : 0;
}
uint32_t aret_GetConsoleMode(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console */
    if (WU(1)) *(uint32_t *)(uintptr_t)WU(1) = 0x3; /* ENABLE_PROCESSED|LINE_INPUT */
    return 1;
}
/* SetConsoleMode: on a real console Windows accepts the mode and returns TRUE; on
 * a redirected handle (pipe/file — every non-interactive run, incl. the winetest
 * harness and the differential harness) it fails and returns FALSE. We mirror
 * GetConsoleMode exactly (console iff isatty). We do not enforce the termios
 * changes a mode implies — consistent with GetConsoleMode reporting a fixed mode;
 * the measured/tested path is always the non-console FALSE branch. */
uint32_t aret_SetConsoleMode(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console -> FALSE */
    return 1;
}
uint32_t aret_GetConsoleScreenBufferInfo(uint32_t esp) {
    int fd = aret_handle_fd(WU(0));
    if (fd < 0 || !isatty(fd)) return 0;            /* not a console */
    int16_t *p = (int16_t *)(uintptr_t)WU(1);
    if (!p) return 0;
    p[0] = 80; p[1] = 25;                /* dwSize           */
    p[2] = 0;  p[3] = 0;                 /* dwCursorPosition */
    p[4] = 7;                            /* wAttributes      */
    p[5] = 0; p[6] = 0; p[7] = 79; p[8] = 24; /* srWindow    */
    p[9] = 80; p[10] = 25;              /* dwMaximumWindowSize */
    return 1;
}

/* ---- oleaut32 BSTR (contractual ABI, not an approximation) -----------------
 * A BSTR is a pointer to a NUL-terminated UTF-16 array preceded by a 4-byte
 * byte-length prefix: [u32 byteLen][wchar[len]][u16 0]. SysAllocString returns a
 * pointer PAST the prefix; SysStringLen/ByteLen read it back; SysFreeString frees
 * from the prefix. This layout is documented and fixed, so a thin native impl is
 * exact. */
uint32_t aret_SysAllocStringLen(uint32_t esp) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(0);
    uint32_t len = WU(1);                                   /* char count */
    uint32_t *base = (uint32_t *)malloc(4 + (size_t)(len + 1) * 2);
    if (!base) return 0;
    base[0] = len * 2;                                      /* byte length */
    uint16_t *s = (uint16_t *)(base + 1);
    for (uint32_t i = 0; i < len; i++) s[i] = src ? src[i] : 0;
    s[len] = 0;
    return (uint32_t)(uintptr_t)s;
}
uint32_t aret_SysAllocString(uint32_t esp) {
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(0);
    if (!src) return 0;
    uint32_t len = 0;
    while (src[len]) len++;
    uint32_t *base = (uint32_t *)malloc(4 + (size_t)(len + 1) * 2);
    if (!base) return 0;
    base[0] = len * 2;
    uint16_t *s = (uint16_t *)(base + 1);
    for (uint32_t i = 0; i < len; i++) s[i] = src[i];
    s[len] = 0;
    return (uint32_t)(uintptr_t)s;
}
uint32_t aret_SysFreeString(uint32_t esp) {
    uint32_t b = WU(0);
    if (b) free((void *)(uintptr_t)(b - 4));
    return 0;
}
uint32_t aret_SysStringLen(uint32_t esp) {
    uint32_t b = WU(0);
    return b ? *(uint32_t *)(uintptr_t)(b - 4) / 2 : 0;
}
uint32_t aret_SysStringByteLen(uint32_t esp) {
    uint32_t b = WU(0);
    return b ? *(uint32_t *)(uintptr_t)(b - 4) : 0;
}

/* ---- ole32 minimal (thin: init + task allocator, no object model) ----------
 * CoInitialize(Ex) returns S_OK on the first init of a thread and S_FALSE (1) on
 * a nested one — the contract callers check; CoUninitialize unwinds it. The task
 * allocator is malloc/free. Exact for code that only inits COM and (de)allocates
 * task memory. A real CoCreateInstance stays long-tail (needs an object model). */
static int aret_co_init_depth = 0;
uint32_t aret_CoInitialize(uint32_t esp)   { (void)esp; return aret_co_init_depth++ ? 1u : 0u; }
uint32_t aret_CoInitializeEx(uint32_t esp) { (void)esp; return aret_co_init_depth++ ? 1u : 0u; }
uint32_t aret_CoUninitialize(uint32_t esp) { (void)esp; if (aret_co_init_depth) aret_co_init_depth--; return 0; }
/* OleInitialize calls CoInitialize internally, so it shares the same depth: S_OK on
 * the first COM init of the thread, S_FALSE (1) nested (measured vs Wine — Ole then
 * Co returns 0 then 1). OleUninitialize unwinds like CoUninitialize. */
uint32_t aret_OleInitialize(uint32_t esp)   { (void)esp; return aret_co_init_depth++ ? 1u : 0u; }
uint32_t aret_OleUninitialize(uint32_t esp) { (void)esp; if (aret_co_init_depth) aret_co_init_depth--; return 0; }
/* COM task-allocator bookkeeping: pointer -> requested size, for IMalloc::GetSize
 * and IMalloc::DidAlloc (below). A side table rather than a size header, for two
 * reasons that both matter: a header would change the pointer CoTaskMemAlloc
 * returns (so a program that mixes it with plain free() would corrupt), and
 * DidAlloc must answer for a pointer we did NOT allocate — reading a header there
 * means dereferencing a foreign pointer, which can fault. The table only ever reads
 * its own memory, so a hostile argument gets a correct "no", never a crash.
 *
 * Open addressing, power-of-two capacity, grown at 70% — the count is the number of
 * LIVE COM blocks, so it tracks the program rather than growing forever. */
struct u32_com_ent { uintptr_t p; size_t n; };
static struct u32_com_ent *g_com_blk;
static size_t g_com_cap, g_com_cnt;

static size_t u32_com_slot(uintptr_t p, size_t cap) {
    size_t h = (size_t)((p >> 4) * 2654435761u) & (cap - 1);
    while (g_com_blk[h].p && g_com_blk[h].p != p) h = (h + 1) & (cap - 1);
    return h;
}
static void u32_com_grow(void) {
    size_t ncap = g_com_cap ? g_com_cap * 2 : 1024;
    void *nb = calloc(ncap, sizeof *g_com_blk);
    if (!nb) { aret_unmodelled("COM allocator bookkeeping: out of memory"); return; }
    struct u32_com_ent *old = g_com_blk;
    size_t ocap = g_com_cap;
    g_com_blk = nb; g_com_cap = ncap;
    for (size_t i = 0; i < ocap; i++)
        if (old[i].p) g_com_blk[u32_com_slot(old[i].p, ncap)] = old[i];
    free(old);
}
static void u32_com_track(void *p, size_t n) {
    if (!p) return;
    if ((g_com_cnt + 1) * 10 >= g_com_cap * 7) u32_com_grow();
    size_t h = u32_com_slot((uintptr_t)p, g_com_cap);
    if (!g_com_blk[h].p) g_com_cnt++;
    g_com_blk[h].p = (uintptr_t)p; g_com_blk[h].n = n;
}
/* Removal must re-insert the probe chain behind the hole, or a later lookup walks
 * past a live entry and reports "not mine" for memory we allocated. */
static void u32_com_untrack(void *p) {
    if (!p || !g_com_cap) return;
    size_t h = u32_com_slot((uintptr_t)p, g_com_cap);
    if (!g_com_blk[h].p) return;
    g_com_blk[h].p = 0; g_com_blk[h].n = 0; g_com_cnt--;
    for (size_t i = (h + 1) & (g_com_cap - 1); g_com_blk[i].p; i = (i + 1) & (g_com_cap - 1)) {
        struct u32_com_ent e = g_com_blk[i];
        g_com_blk[i].p = 0; g_com_blk[i].n = 0; g_com_cnt--;
        u32_com_track((void *)e.p, e.n);
    }
}
/* -1 (not one of ours / NULL) or the requested size. */
static size_t u32_com_size(void *p) {
    if (!p || !g_com_cap) return (size_t)-1;
    size_t h = u32_com_slot((uintptr_t)p, g_com_cap);
    return g_com_blk[h].p ? g_com_blk[h].n : (size_t)-1;
}

uint32_t aret_CoTaskMemAlloc(uint32_t esp) {
    size_t n = (size_t)WU(0);
    void *p = malloc(n);
    u32_com_track(p, n);
    return (uint32_t)(uintptr_t)p;
}
uint32_t aret_CoTaskMemRealloc(uint32_t esp) {
    void *old = (void *)(uintptr_t)WU(0);
    size_t n = (size_t)WU(1);
    void *p = realloc(old, n);
    if (p) { u32_com_untrack(old); u32_com_track(p, n); }
    return (uint32_t)(uintptr_t)p;
}
uint32_t aret_CoTaskMemFree(uint32_t esp) {
    void *p = (void *)(uintptr_t)WU(0);
    u32_com_untrack(p);
    free(p);
    return 0;
}

/* ---- DDE param packing (user32) -------------------------------------------
 * The two values a DDE message carries fit a LPARAM directly for most messages
 * (MAKELONG), but WM_DDE_ADVISE/ACK/DATA/POKE need both a handle and a status/flags,
 * so PackDDElParam ALLOCATES a holder and returns a handle; UnpackDDElParam reads it
 * back; FreeDDElParam frees it. Measured vs Wine: the round-trip is exact (the raw
 * handle is a pointer, non-deterministic, so only the round-trip + the MAKELONG form
 * are oracle-compared). Runtime is -m32, so a malloc pointer fits a uint32_t. */
static int u32_dde_alloc_msg(uint32_t msg) {
    return msg == 0x3E2u || msg == 0x3E4u || msg == 0x3E5u || msg == 0x3E7u; /* ADVISE/ACK/DATA/POKE */
}
uint32_t aret_PackDDElParam(uint32_t esp) {
    uint32_t msg = WU(0), lo = WU(1), hi = WU(2);
    if (u32_dde_alloc_msg(msg)) {
        uint32_t *p = (uint32_t *)malloc(2 * sizeof(uint32_t));
        if (!p) return 0;
        p[0] = lo; p[1] = hi;
        return (uint32_t)(uintptr_t)p;
    }
    return (lo & 0xFFFFu) | (hi << 16);                 /* MAKELONG(lo, hi) */
}
uint32_t aret_UnpackDDElParam(uint32_t esp) {
    uint32_t msg = WU(0), lp = WU(1);
    uint32_t *plo = (uint32_t *)WP(2), *phi = (uint32_t *)WP(3);
    uint32_t lo, hi;
    if (u32_dde_alloc_msg(msg) && lp) { const uint32_t *p = (const uint32_t *)(uintptr_t)lp; lo = p[0]; hi = p[1]; }
    else { lo = lp & 0xFFFFu; hi = (lp >> 16) & 0xFFFFu; }
    if (plo) *plo = lo;
    if (phi) *phi = hi;
    return 1;
}
uint32_t aret_FreeDDElParam(uint32_t esp) {
    if (u32_dde_alloc_msg(WU(0)) && WU(1)) free((void *)(uintptr_t)WU(1));
    return 1;
}

/* ---- advapi32 minimal: the legacy CryptoAPI RNG path -----------------------
 * BusyBox-w32 (and much MSVC/mingw code) seeds its PRNG at startup via the
 * classic CryptAcquireContext + CryptGenRandom + CryptReleaseContext triple.
 * We model exactly what those callers use — a non-NULL provider token and a
 * buffer filled with random bytes — not a real cryptographic provider (no key
 * containers, no algorithms; a program needing real crypto strength needs a
 * real provider, which stays long-tail). Without this, BusyBox's first
 * applet-dispatch aborts ("applet not found") because RNG seeding fails at
 * startup, so *every* applet is unreachable.
 *
 * The bytes come from a self-contained xorshift seeded from host entropy — good
 * enough for seeding a CLI PRNG (this is not a security boundary). */
static uint64_t aret_rng_next(void) {
    static uint64_t s = 0;
    if (!s) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        s = ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec)
          ^ ((uint64_t)(uintptr_t)&s << 16) ^ (uint64_t)getpid();
        if (!s) s = 0x9e3779b97f4a7c15ull;
    }
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

/* CryptAcquireContextA(phProv, container, provider, provtype, flags) -> BOOL.
 * Return a non-NULL pseudo-handle; the only contract callers check is success
 * plus a usable handle to pass on to CryptGenRandom/CryptReleaseContext. */
uint32_t aret_CryptAcquireContextA(uint32_t esp) {
    uint32_t *phProv = (uint32_t *)WP(0);
    if (phProv) *phProv = 0x43525950u; /* 'CRYP' — an opaque non-zero token */
    return 1;
}
/* CryptGenRandom(hProv, dwLen, pbBuffer) -> BOOL. Fill the buffer with bytes. */
uint32_t aret_CryptGenRandom(uint32_t esp) {
    uint32_t len = WU(1);
    unsigned char *buf = (unsigned char *)WP(2);
    if (!buf) return 0;
    for (uint32_t i = 0; i < len; i++) buf[i] = (unsigned char)(aret_rng_next() & 0xffu);
    return 1;
}
/* CryptReleaseContext(hProv, flags) -> BOOL. Nothing to release. */
uint32_t aret_CryptReleaseContext(uint32_t esp) { (void)esp; return 1; }

/* ================================================================== */
/* MessageBox — display-free sound fallback                            */
/* ================================================================== */
/* A modal message box needs a display to show and a user to dismiss. In the
 * display-free tier (no SDL yet) there is neither, so we return the same thing
 * the ground truth does with no display: Wine's MessageBoxA with DISPLAY unset
 * returns -1 (0xFFFFFFFF) immediately, without blocking — verified empirically.
 * That is the honest "no display available" answer (not a guessed button): a
 * program that ignores the result proceeds, one that checks it sees the same
 * failure as under headless Wine. A real dialog (SDL_ShowSimpleMessageBox) will
 * replace this when a visible display is available (G2b). */
uint32_t aret_MessageBoxA(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxW(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxExA(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }
uint32_t aret_MessageBoxExW(uint32_t esp) { (void)esp; return 0xFFFFFFFFu; }

/* ================================================================== */
/* PE resources (.rsrc) — Find/Load/Sizeof/Lock/Free + LoadString      */
/* ================================================================== */
/* The PE headers and the .rsrc section are mapped at their image VAs (the
 * Memory Layout Mapper), so we walk the real IMAGE_RESOURCE_DIRECTORY tree in
 * place — no data is fabricated. Display-free and portable: this is pure data
 * indexing (LoadString, custom RCDATA, dialog/menu templates for later GDI/dialog
 * work). Sound: an absent resource returns NULL/0 (exactly as Win32), never a
 * guess. RT_* IDs: RT_STRING=6, RT_RCDATA=10, RT_VERSION=16, RT_DIALOG=5, … */
extern uint32_t aret_image_lo, aret_image_hi;

/* Locate the resource directory root in mapped memory: parse the PE headers at
 * the image base (aret_image_lo). Returns the root IMAGE_RESOURCE_DIRECTORY and,
 * via *out_base, the image base (leaf data RVAs are relative to it). NULL if the
 * image has no resource directory (or headers are unavailable). */
static const uint8_t *u32_rsrc_root(uint32_t *out_base) {
    uint32_t lo = aret_image_lo;
    if (!lo) return NULL;
    const uint8_t *img = (const uint8_t *)(uintptr_t)lo;
    if (img[0] != 'M' || img[1] != 'Z') return NULL;             /* DOS header */
    uint32_t e_lfanew = *(const uint32_t *)(img + 0x3C);
    if ((uint64_t)lo + e_lfanew + 0x100 > aret_image_hi) return NULL;
    const uint8_t *nt = img + e_lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] || nt[3]) return NULL; /* "PE\0\0" */
    const uint8_t *opt = nt + 24;                                /* skip sig(4)+COFF(20) */
    if (*(const uint16_t *)opt != 0x010B) return NULL;           /* PE32 only (our target) */
    uint32_t rsrc_rva = *(const uint32_t *)(opt + 112);          /* DataDirectory[2].VirtualAddress */
    if (!rsrc_rva) return NULL;
    if (out_base) *out_base = lo;
    return img + rsrc_rva;
}

/* Case-insensitive compare of an ANSI name to a UTF-16 IMAGE_RESOURCE_DIR_STRING_U
 * (WORD length prefix + that many WCHARs, no NUL). Win32 resource names are ASCII
 * and matched case-insensitively. */
static int u32_rsrc_name_eq(const uint8_t *s, const char *name) {
    uint16_t slen = *(const uint16_t *)s;
    const uint8_t *ws = s + 2;
    uint16_t k = 0;
    for (; k < slen && name[k]; k++) {
        uint16_t wc = *(const uint16_t *)(ws + 2 * k);
        char a = name[k];
        char la = (a >= 'A' && a <= 'Z') ? a + 32 : a;
        char lb = ((char)wc >= 'A' && (char)wc <= 'Z') ? (char)wc + 32 : (char)wc;
        if (wc > 0xFF || la != lb) return 0;
    }
    return k == slen && name[k] == 0;
}

/* Find the entry in a resource directory `dir` (rsrc base `rb`) whose key matches
 * an integer id (name==NULL) or an ANSI name. Returns the entry's OffsetToData
 * field (high bit = subdirectory, else data-entry offset), or 0 if not found. */
static uint32_t u32_rsrc_entry(const uint8_t *rb, const uint8_t *dir, uint32_t id, const char *name) {
    uint16_t nnamed = *(const uint16_t *)(dir + 12);
    uint16_t nid = *(const uint16_t *)(dir + 14);
    const uint8_t *e = dir + 16;
    int total = (int)nnamed + (int)nid;
    for (int i = 0; i < total; i++, e += 8) {
        uint32_t nm = *(const uint32_t *)e;
        if (name) {
            if (!(nm & 0x80000000u)) continue;                   /* an ID, not a name */
            if (u32_rsrc_name_eq(rb + (nm & 0x7FFFFFFFu), name)) return *(const uint32_t *)(e + 4);
        } else {
            if (nm & 0x80000000u) continue;                      /* a name, not an ID */
            if (nm == id) return *(const uint32_t *)(e + 4);
        }
    }
    return 0;
}

/* A resource reference is either an ANSI string pointer or a MAKEINTRESOURCE id
 * (the whole value < 0x10000). Split it. */
static void u32_rsrc_ref(uint32_t ref, uint32_t *id, const char **name) {
    if (ref >= 0x10000u) { *name = (const char *)(uintptr_t)ref; *id = 0; }
    else                 { *name = NULL; *id = ref; }
}

/* Walk type -> name -> language(first) and return the IMAGE_RESOURCE_DATA_ENTRY,
 * or NULL. `type`/`name` are MAKEINTRESOURCE-or-string refs. */
static const uint8_t *u32_rsrc_data_entry(uint32_t type_ref, uint32_t name_ref) {
    uint32_t base;
    const uint8_t *rb = u32_rsrc_root(&base);
    if (!rb) return NULL;
    uint32_t tid, nid; const char *tname, *nname;
    u32_rsrc_ref(type_ref, &tid, &tname);
    u32_rsrc_ref(name_ref, &nid, &nname);
    uint32_t off = u32_rsrc_entry(rb, rb, tid, tname);           /* type level */
    if (!(off & 0x80000000u)) return NULL;
    off = u32_rsrc_entry(rb, rb + (off & 0x7FFFFFFFu), nid, nname); /* name level */
    if (!(off & 0x80000000u)) return NULL;
    const uint8_t *lang = rb + (off & 0x7FFFFFFFu);              /* language level */
    /* Take the first language entry (its OffsetToData points at the leaf). */
    uint16_t nnamed = *(const uint16_t *)(lang + 12), nidc = *(const uint16_t *)(lang + 14);
    if ((int)nnamed + (int)nidc < 1) return NULL;
    uint32_t leaf = *(const uint32_t *)(lang + 16 + 4);          /* first entry OffsetToData */
    if (leaf & 0x80000000u) return NULL;                        /* must be a data entry, not a dir */
    return rb + leaf;
}

/* EnumResourceLanguages{A,W}(hModule, lpType, lpName, lpEnumFunc, lParam) -> BOOL.
 * Walks type -> name in the PE resource tree and calls the LIFTED callback once per
 * language leaf. Contract MEASURED against Wine:
 *   - success: callback per language, returns TRUE, last error untouched;
 *   - the callback returning FALSE stops the walk and the FUNCTION returns FALSE —
 *     with NO error set: "the caller asked to stop" is not a failure;
 *   - type not found   -> FALSE + 1813 (ERROR_RESOURCE_TYPE_NOT_FOUND);
 *   - name not found   -> FALSE + 1814 (ERROR_RESOURCE_NAME_NOT_FOUND).
 * `lpType`/`lpName` are handed to the callback VERBATIM (measured: the callback sees
 * 0xa and 0x64 for RT_RCDATA/100, i.e. the caller's own MAKEINTRESOURCE values, not
 * anything re-derived from the tree).
 * The whole contract comes from the binary's own .rsrc, so it is deterministic — no
 * environmental component to separate out here, unlike the font enumeration. */
static uint32_t u32_enum_res_langs(uint32_t esp, int wide) {
    uint32_t hmod = WU(0), type_ref = WU(1), name_ref = WU(2), proc = WU(3), lparam = WU(4);
    uint32_t base;
    const uint8_t *rb = u32_rsrc_root(&base);
    if (!rb) { g_last_error = 1813; return 0; }
    uint32_t tid, nid; const char *tname, *nname;
    u32_rsrc_ref(type_ref, &tid, &tname);
    u32_rsrc_ref(name_ref, &nid, &nname);
    /* A STRING type/name under the W entry point would be UTF-16, while the tree
     * comparison here is the ANSI one. Rather than mismatch (and then report
     * "not found" for a resource that exists — a lie), stop loudly. Integer
     * MAKEINTRESOURCE refs, the overwhelmingly common case, are exact at both widths. */
    if (wide && (tname || nname))
        aret_unmodelled("EnumResourceLanguagesW: string (non-MAKEINTRESOURCE) type/name");
    uint32_t off = u32_rsrc_entry(rb, rb, tid, tname);
    if (!(off & 0x80000000u)) { g_last_error = 1813; return 0; }
    off = u32_rsrc_entry(rb, rb + (off & 0x7FFFFFFFu), nid, nname);
    if (!(off & 0x80000000u)) { g_last_error = 1814; return 0; }
    const uint8_t *lang = rb + (off & 0x7FFFFFFFu);
    uint16_t nnamed = *(const uint16_t *)(lang + 12), nidc = *(const uint16_t *)(lang + 14);
    int total = (int)nnamed + (int)nidc;
    if (total < 1) { g_last_error = 1815; return 0; }   /* ERROR_RESOURCE_LANG_NOT_FOUND */
    const uint8_t *e = lang + 16;
    for (int i = 0; i < total; i++, e += 8) {
        uint32_t key = *(const uint32_t *)e;
        if (key & 0x80000000u) continue;                /* a named language: skip */
        /* stdcall frame below the caller's esp: [0] return slot, then the 5 args. */
        uint32_t frame = (esp - 0x80) & ~15u;
        uint32_t *fr = (uint32_t *)(uintptr_t)frame;
        fr[0] = 0; fr[1] = hmod; fr[2] = type_ref; fr[3] = name_ref;
        fr[4] = key & 0xFFFFu; fr[5] = lparam;
        if (!(uint32_t)aret_call(proc, frame, 0, 0, 0, 0, 0, 0, 0)) return 0;
    }
    return 1;
}
uint32_t aret_EnumResourceLanguagesA(uint32_t esp) { return u32_enum_res_langs(esp, 0); }
uint32_t aret_EnumResourceLanguagesW(uint32_t esp) { return u32_enum_res_langs(esp, 1); }

/* FindResourceA(hModule, lpName, lpType) -> HRSRC. Returns the DATA_ENTRY pointer
 * as an opaque handle (LoadResource/SizeofResource take it back). */
uint32_t aret_FindResourceA(uint32_t esp) {
    const uint8_t *de = u32_rsrc_data_entry(WU(2) /* type */, WU(1) /* name */);
    return (uint32_t)(uintptr_t)de;
}
/* LoadResource(hModule, hResInfo) -> HGLOBAL. The bytes live at image_base+RVA. */
uint32_t aret_LoadResource(uint32_t esp) {
    const uint8_t *de = (const uint8_t *)(uintptr_t)WU(1);
    if (!de) return 0;
    return aret_image_lo + *(const uint32_t *)de;               /* OffsetToData is an image RVA */
}
/* LockResource(hResData) -> LPVOID. Already a pointer to the bytes. */
uint32_t aret_LockResource(uint32_t esp) { return WU(0); }
/* SizeofResource(hModule, hResInfo) -> DWORD (bytes). */
uint32_t aret_SizeofResource(uint32_t esp) {
    const uint8_t *de = (const uint8_t *)(uintptr_t)WU(1);
    return de ? *(const uint32_t *)(de + 4) : 0;                /* DATA_ENTRY.Size */
}
/* FreeResource(hResData) -> BOOL. A no-op in Win32 (returns FALSE = 0). */
uint32_t aret_FreeResource(uint32_t esp) { (void)esp; return 0; }

/* LoadStringA(hInstance, uID, lpBuffer, cchBufferMax) -> chars copied (excl NUL).
 * RT_STRING resources bundle 16 strings per block: block id = uID/16 + 1, index
 * = uID%16; each entry is a WORD length (WCHARs) + that many WCHARs (no NUL). */
uint32_t aret_LoadStringA(uint32_t esp) {
    uint32_t uID = WU(1);
    char *buf = (char *)WP(2);
    uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    const uint8_t *de = u32_rsrc_data_entry(6 /* RT_STRING */, uID / 16 + 1);
    if (!de) { buf[0] = 0; return 0; }
    const uint16_t *p = (const uint16_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
    uint32_t idx = uID % 16;
    for (uint32_t i = 0; i < idx; i++) p += 1 + *p;             /* skip to the target entry */
    uint16_t len = *p++;                                        /* WCHAR count */
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t i = 0; i < n; i++) buf[i] = (char)(p[i] & 0xFF);
    buf[n] = 0;
    return n;
}

/* ================================================================== */
/* USER32 — message-only window subsystem (no display, fully portable) */
/* ================================================================== */
/* A message-only window (HWND_MESSAGE) has no pixels: it is purely an internal
 * message sink bound to a WNDPROC. Tcl's notifier — and many console programs —
 * create one to drive an event loop. We model it soundly: a window-class
 * registry, a window table, a single (mono-thread) message FIFO, and timers.
 * Every dispatch is a real callback into the lifted WNDPROC via aret_call. No
 * X11, no GDI, no host threads: this stays standalone and WASM-portable. Real
 * *visible* multi-window GUI is a later milestone (SDL2-backed); this is the
 * message plumbing only. Mono-thread, matching the rest of the runtime model. */

#define U32_WM_QUIT   0x0012u
#define U32_WM_TIMER  0x0113u
#define U32_PM_REMOVE 0x0001u
#define U32_WM_PAINT         0x000Fu
#define U32_WM_ERASEBKGND    0x0014u
#define U32_WM_SETTEXT       0x000Cu
#define U32_WM_GETTEXT       0x000Du
#define U32_WM_GETTEXTLENGTH 0x000Eu

/* A pseudo-HWND for the desktop window (GetDesktopWindow). Kept well outside the
 * 1..U32_MAX_WIN handle range so it never aliases a real window. Its "rect" is the
 * virtual screen — a defined ARET desktop size (like the host-backed values of
 * GetDiskFreeSpace), not a guess: display-dependent raw values are tested by
 * invariant, never bit-compared to Wine (doc 72 §4.5). */
#define U32_DESKTOP  0x00010000u
#define U32_SCREEN_W 1024
#define U32_SCREEN_H 768

#define U32_MAX_CLASSES 64
/* A registered window class. The full WNDCLASS(EX) fields are kept so GetClassInfo
 * can return them verbatim (a program that registers then queries a class must read
 * back exactly what it set). */
static struct {
    uint16_t name[128];
    uint32_t wndproc, hbr_bg;
    uint32_t style, cls_extra, wnd_extra, hinstance, hicon, hcursor, menu_name, hicon_sm;
    int used;
} g_u32_class[U32_MAX_CLASSES];

#define U32_MAX_WIN 256
/* A window object. For message-only windows only wndproc/parent matter; a visible
 * (top-level) window also tracks its rect/style/text so the geometry & text APIs
 * (GetWindowRect/SetWindowPos/Set/GetWindowText/…) round-trip. The actual pixels
 * (an SDL_Window) come in G2b — this model layer is display-free and portable. */
static struct {
    uint32_t wndproc, parent;
    int used;
    int x, y, w, h;          /* window rect, screen coords */
    uint32_t style, exstyle;
    int visible, enabled;
    uint32_t userdata;       /* GWL_USERDATA */
    char title[256];         /* window text (ANSI) */
    int ctrl_id;             /* dialog control id (0 = not a control) */
    char classname[64];      /* registered class name (for GetClassName) */
    int needs_paint;         /* update region non-empty -> owes a WM_PAINT */
    int needs_erase;         /* update region marked for erase -> WM_ERASEBKGND */
    uint32_t bg_brush;       /* class hbrBackground (HBRUSH) for the default erase */
    int unicode;             /* created via a W API (IsWindowUnicode) */
    int check_state;         /* dialog button check state (BM_GETCHECK/CheckDlgButton) */
    uint32_t ctrl_font;      /* control font (WM_SETFONT) for a predefined control's paint */
    int extra_len;           /* cbWndExtra bytes (from the class) */
    uint8_t extra[64];       /* cbWndExtra storage: SetWindowLong at offset >=0 (control state ptr) */
    struct { int min, max, page, pos; } scroll[3];  /* SB_HORZ=0 / SB_VERT=1 / SB_CTL=2 */
    int du_x, du_y;          /* dialog base units (per-dialog, from its font); 0 = not a mapped dialog */
    int is_dialog;           /* created by u32_dialog_create -> composite its child controls for display */
    char **items; int item_count, item_cap, cur_sel;   /* LISTBOX/COMBOBOX item model */
    uint32_t dlg_font;       /* HFONT of the dialog font (DS_SETFONT), applied to its controls */
    int wnd_rgn_set, wnd_rgn_l, wnd_rgn_t, wnd_rgn_r, wnd_rgn_b, wnd_rgn_complex;  /* Set/GetWindowRgn */
#ifdef ARET_HAVE_SDL
    void *sdl_win, *sdl_ren, *sdl_tex;  /* SDL window/renderer/streaming texture */
    uint32_t client_bmp;     /* HBITMAP of the client-area framebuffer (GDI draws here) */
    int cw, ch;              /* client framebuffer size (pixels) */
#endif
} g_u32_win[U32_MAX_WIN];

#ifdef ARET_HAVE_SDL
/* G2b window-presentation helpers (defined after the GDI object model, which they
 * use for the client framebuffer). Show creates the real SDL window on first
 * visibility; present blits the client framebuffer; pump drains SDL input events
 * into WM_* messages. All are no-ops when there is no usable display. */
static void sdl_window_show(uint32_t esp, int i);
static void sdl_window_present(int i);
static void u32_dialog_composite(uint32_t esp, int di);       /* fwd: fill 3DFACE + compose child controls */
static void u32_composite_children(uint32_t esp, int di);     /* fwd: compose visible child controls over the client */
static void u32_present_toplevel(uint32_t esp, int wi);       /* fwd: compose children (dialog or plain) then present */
static void u32_free_child_bmp(int i);                        /* fwd: free a child control's client framebuffer */
static void sdl_window_destroy(int i);
static void sdl_pump(void);
static int  sdl_win_idx_from_id(uint32_t winid);
static int  sdl_any_window(void);
#endif

#define U32_MAX_MSG 8192
static struct { uint32_t hwnd, message, wParam, lParam, time, ptx, pty; } g_u32_q[U32_MAX_MSG];
static int g_u32_qh, g_u32_qt;          /* ring head/tail; empty when equal */

static int g_u32_quit;                  /* PostQuitMessage seen */
static int g_u32_quit_code;

#define U32_MAX_TIMER 64
static struct { uint32_t hwnd, id, elapse, proc; uint64_t due_ns; int used; } g_u32_timer[U32_MAX_TIMER];
static uint32_t g_u32_next_timer_id = 0xF000u; /* ids for hwnd==NULL SetTimer */

/* width-16 (Windows wchar_t) string helpers */
static int u32_weq(const uint16_t *a, const uint16_t *b) {
    for (int i = 0;; i++) { if (a[i] != b[i]) return 0; if (!a[i]) return 1; }
}
static void u32_wcpy(uint16_t *d, const uint16_t *s, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0;
}
/* Widen a narrow (ANSI) class/window name to 16-bit — class names are ASCII in
 * practice, so a byte→u16 widening is exact. Lets the ANSI (A) window APIs share
 * the one wide class registry with the W APIs (Windows shares the atom table). */
static void u32_a2w(const char *s, uint16_t *d, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = (uint16_t)(unsigned char)s[i]; d[i] = 0;
}
/* Narrow a 16-bit (Windows wchar_t) string to ANSI — the reverse of u32_a2w. The
 * window model stores text as ANSI; the W text APIs narrow on the way in and widen
 * on the way out (exact for ASCII, which is what the content oracle exercises). */
static void u32_w2n(const uint16_t *s, char *d, int cap) {
    int i = 0; if (s) for (; s[i] && i < cap - 1; i++) d[i] = (char)(s[i] & 0xFF); d[i] = 0;
}

/* DeleteFileA — AUTO-MARSHALLED from DeleteFileW by `gen_win32_sigs.py --marshal
 * DeleteFileA` (doc 82, layer 2). Widen the input path (u32_a2w), pass nothing else,
 * call the wide core; NULL propagates as NULL. This is the first generated marshalling
 * thunk WIRED into the HLE: it replaces a hand shim that did translate_path+unlink
 * directly, and is byte-exact-equivalent because u32_a2w/aret_w2n round-trip every byte
 * 0-255. Proven bit-identical Wine (win32_fileops/win32_file/win32_find/…). The generator
 * REFUSES any pair that differs beyond input strings (LOGFONTA≠LOGFONTW, OUT buffers). */
uint32_t aret_DeleteFileW(uint32_t esp);
uint32_t aret_DeleteFileA(uint32_t esp) {
    const char *s0 = (const char *)WP(0);
    int n0 = 0; if (s0) while (s0[n0]) n0++;
    uint16_t w0[n0 + 1]; u32_a2w(s0, w0, n0 + 1);
    uint32_t fr[1];
    fr[0] = s0 ? (uint32_t)(uintptr_t)w0 : 0;
    return aret_DeleteFileW((uint32_t)(uintptr_t)fr);
}
/* Register a class (shared A/W core): store the full WNDCLASS(EX) so GetClassInfo
 * round-trips it, and return the atom. */
static uint32_t u32_class_register(uint32_t style, uint32_t wndproc, uint32_t cls_extra,
                                   uint32_t wnd_extra, uint32_t hinstance, uint32_t hicon,
                                   uint32_t hcursor, uint32_t hbr_bg, uint32_t menu_name,
                                   uint32_t hicon_sm, const uint16_t *wname) {
    for (int i = 0; i < U32_MAX_CLASSES; i++) {
        if (!g_u32_class[i].used) {
            g_u32_class[i].used = 1;
            g_u32_class[i].style = style;
            g_u32_class[i].wndproc = wndproc;
            g_u32_class[i].cls_extra = cls_extra;
            g_u32_class[i].wnd_extra = wnd_extra;
            g_u32_class[i].hinstance = hinstance;
            g_u32_class[i].hicon = hicon;
            g_u32_class[i].hcursor = hcursor;
            g_u32_class[i].hbr_bg = hbr_bg;
            g_u32_class[i].menu_name = menu_name;
            g_u32_class[i].hicon_sm = hicon_sm;
            u32_wcpy(g_u32_class[i].name, wname, 128);
            return 0xC000u + (uint32_t)i;
        }
    }
    return 0;
}
/* Resolve a class reference (atom or wide-name pointer) to its registry index, or -1. */
static int u32_class_index(uint32_t cref) {
    if (cref == 0) return -1;
    if (cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) return (int)idx;
        return -1;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) return i;
    return -1;
}
/* Resolve a class reference (atom or name pointer) to its background brush, or 0. */
static uint32_t u32_class_brush(uint32_t cref) {
    if (cref == 0) return 0;
    if (cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) return g_u32_class[idx].hbr_bg;
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) return g_u32_class[i].hbr_bg;
    return 0;
}
/* CW_USEDEFAULT: the caller leaves placement to the OS. We pick a fixed default so
 * geometry is deterministic; explicit coords (the common, oracle-tested case) pass
 * through unchanged. */
static int u32_coord(uint32_t v, int dflt) {
    return (v == 0x80000000u) ? dflt : (int)(int32_t)v;
}
/* A predefined USER32 control class (BUTTON/EDIT/…) has no app WNDPROC — the system
 * provides it. We model these as data-only control windows (state tracked, no
 * pixels): enough for GetDlgItem / Get-SetDlgItemText / CheckDlgButton to work on
 * a CreateWindowEx-created control, which real dialogs rely on. */
static int u32_is_ctrl_class(uint32_t cref, int wide) {
    if (cref < 0x10000u) return 0;                 /* atom = a registered class */
    char n[64];
    if (wide) u32_w2n((const uint16_t *)(uintptr_t)cref, n, sizeof n);
    else { const char *s = (const char *)(uintptr_t)cref; int k = 0; for (; s[k] && k < 63; k++) n[k] = s[k]; n[k] = 0; }
    static const char *const ctrls[] = { "button", "edit", "static", "listbox", "combobox",
        "scrollbar", "mdiclient", "richedit", "richedit20a", "richedit20w", "syslistview32",
        "systreeview32", "msctls_statusbar32", "msctls_updown32", "toolbarwindow32", "tooltips_class32", 0 };
    for (int i = 0; ctrls[i]; i++) {
        int eq = 1;
        for (int j = 0;; j++) {
            char a = n[j], b = ctrls[i][j];
            char la = (a >= 'A' && a <= 'Z') ? a + 32 : a;
            if (la != b) { eq = 0; break; }
            if (!a) break;
        }
        if (eq) return 1;
    }
    return 0;
}
/* Create a window (shared A/W core): bind wndproc + capture rect/style/text.
 * `is_ctrl` permits a data-only control window with no WNDPROC (predefined class). */
static uint32_t u32_window_create(uint32_t wndproc, uint32_t exstyle, uint32_t style,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  uint32_t parent, const char *title, int is_ctrl) {
    if (!wndproc && !is_ctrl) return 0;
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) {
            g_u32_win[i].used = 1;
            memset(g_u32_win[i].scroll, 0, sizeof g_u32_win[i].scroll);   /* fresh scroll state */
            g_u32_win[i].ctrl_font = 0;                                   /* fresh control font */
            g_u32_win[i].wndproc = wndproc;
            g_u32_win[i].parent = parent;
            g_u32_win[i].exstyle = exstyle;
            g_u32_win[i].style = style;
            g_u32_win[i].x = u32_coord(x, 0);
            g_u32_win[i].y = u32_coord(y, 0);
            g_u32_win[i].w = u32_coord(w, 0);
            g_u32_win[i].h = u32_coord(h, 0);
            g_u32_win[i].visible = (style & 0x10000000u /* WS_VISIBLE */) ? 1 : 0;
            g_u32_win[i].enabled = (style & 0x08000000u /* WS_DISABLED */) ? 0 : 1;
            g_u32_win[i].userdata = 0;
            g_u32_win[i].ctrl_id = 0;
            g_u32_win[i].classname[0] = 0;
            g_u32_win[i].needs_paint = 0;
            g_u32_win[i].needs_erase = 0;
            g_u32_win[i].bg_brush = 0;
            g_u32_win[i].extra_len = 0;
            g_u32_win[i].items = NULL; g_u32_win[i].item_count = 0; g_u32_win[i].item_cap = 0; g_u32_win[i].cur_sel = -1;
            g_u32_win[i].is_dialog = 0; g_u32_win[i].du_x = 0; g_u32_win[i].du_y = 0; g_u32_win[i].dlg_font = 0;
            g_u32_win[i].wnd_rgn_set = 0;
            memset(g_u32_win[i].extra, 0, sizeof g_u32_win[i].extra);
            int k = 0; if (title) for (; title[k] && k < 255; k++) g_u32_win[i].title[k] = title[k];
            g_u32_win[i].title[k] = 0;
            /* A visible top-level window (no parent, not a child control) starts
             * with its whole client invalid -> owes a WM_PAINT (erase included),
             * and gets a real SDL window. Child/message-only windows never do. */
            if (g_u32_win[i].visible && parent == 0 && !(style & 0x40000000u /* WS_CHILD */)) {
                g_u32_win[i].needs_paint = 1;
                g_u32_win[i].needs_erase = 1;
#ifdef ARET_HAVE_SDL
                sdl_window_show(0, i);   /* create-time: no children yet, esp not threaded here */
#endif
            }
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}

/* Resolve a class reference (an ATOM from RegisterClass, or a wide-name pointer)
 * to its registered WNDPROC, or 0 if unknown. */
static uint32_t u32_class_wndproc(uint32_t cref) {
    if (cref == 0) return 0;
    if (cref < 0x10000u) {                 /* ATOM */
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) return g_u32_class[idx].wndproc;
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) return g_u32_class[i].wndproc;
    return 0;
}
/* The cbWndExtra byte count a class was registered with (0 if unknown). */
static uint32_t u32_class_wndextra(uint32_t cref) {
    int idx = u32_class_index(cref);
    return idx >= 0 ? g_u32_class[idx].wnd_extra : 0;
}

static uint32_t u32_win_wndproc(uint32_t hwnd) {
    if (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) return g_u32_win[hwnd - 1].wndproc;
    return 0;
}

/* Call a lifted WNDPROC(hwnd,msg,wParam,lParam) — a stdcall callback into guest
 * code. Lay the frame just below the live machine esp (reentrant: a WNDPROC may
 * itself SendMessage), exactly like a real call: [esp+0]=retaddr, [esp+4..]=args. */
static uint32_t u32_call_wndproc(uint32_t esp, uint32_t wndproc,
                                 uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    uint32_t frame = (esp - 64) & ~15u;
    uint32_t *f = (uint32_t *)(uintptr_t)frame;
    f[0] = 0; f[1] = hwnd; f[2] = msg; f[3] = wp; f[4] = lp;
    return (uint32_t)aret_call(wndproc, frame, 0, 0, 0, 0, 0, 0, 0);
}

/* Send WM_NCCREATE then WM_CREATE to a freshly created window's WNDPROC, with a
 * CREATESTRUCTA (a control allocates its state here and stores it via
 * SetWindowLong(0)). Returns 1 if creation is accepted, 0 if the proc rejected
 * it (WM_NCCREATE -> FALSE, or WM_CREATE -> -1) — Windows then fails the create.
 * The CREATESTRUCT is malloc'd (guest-accessible, reentrant-safe), freed after. */
static int u32_create_dispatch(uint32_t esp, int i, uint32_t hinstance, uint32_t hmenu_or_id) {
    uint32_t wp = g_u32_win[i].wndproc;
    if (!wp) return 1;                       /* no proc -> nothing to initialise */
    uint32_t *cs = (uint32_t *)malloc(48);   /* CREATESTRUCTA (32-bit layout) */
    if (!cs) return 1;
    cs[0] = 0;                    cs[1] = hinstance;         cs[2] = hmenu_or_id;
    cs[3] = g_u32_win[i].parent;  cs[4] = (uint32_t)g_u32_win[i].h; cs[5] = (uint32_t)g_u32_win[i].w;
    cs[6] = (uint32_t)g_u32_win[i].y; cs[7] = (uint32_t)g_u32_win[i].x;
    cs[8] = g_u32_win[i].style;   cs[9] = 0; cs[10] = 0;     cs[11] = g_u32_win[i].exstyle;
    uint32_t hwnd = (uint32_t)(i + 1), csp = (uint32_t)(uintptr_t)cs;
    int ok = 1;
    if (u32_call_wndproc(esp, wp, hwnd, 0x0081u /* WM_NCCREATE */, 0, csp) == 0) {
        ok = 0;
    } else if (u32_call_wndproc(esp, wp, hwnd, 0x0001u /* WM_CREATE */, 0, csp) == (uint32_t)-1) {
        ok = 0;
    }
    free(cs);
    return ok;
}

/* Windows hooks (SetWindowsHookEx): the proc per idHook. idHook runs WH_MSGFILTER(-1)
 * .. WH_MOUSE_LL(14); store at idHook+1. Only WH_CBT is delivered today (the one MFC
 * relies on to attach its CWnd); other hooks are stored but not fired — a sound gap,
 * exactly the old stub behaviour, never a wrong delivery. */
#define U32_WH_CBT 5
static uint32_t g_u32_hook[16];
static uint32_t u32_hook_set(int idHook, uint32_t proc) {
    int idx = idHook + 1;
    if (idx < 0 || idx >= 16) return 0x484F4F4Bu;   /* out of range: opaque handle, not fired */
    g_u32_hook[idx] = proc;
    return 0x48480000u | (uint32_t)(idx + 1);        /* HHOOK encoding the slot */
}
/* Fire the WH_CBT hook's HCBT_CREATEWND for a freshly-created window, BEFORE WM_NCCREATE
 * — the notification MFC uses to attach/subclass its CWnd (its filter reads the
 * CREATESTRUCT's class and may SetWindowLong(GWL_WNDPROC) to reroute the wndproc). No
 * WH_CBT installed -> nothing happens (the pre-hook default). The CBT_CREATEWND +
 * CREATESTRUCTA are guest-accessible (malloc, 32-bit app), freed after. */
static void u32_fire_cbt_createwnd(uint32_t esp, int i, uint32_t hinst, uint32_t hmenu,
                                   uint32_t class_ref, uint32_t name_ref) {
    uint32_t proc = g_u32_hook[U32_WH_CBT + 1];
    if (!proc) return;
    uint32_t *cs = (uint32_t *)malloc(48 + 8);   /* CREATESTRUCTA (48) + CBT_CREATEWND (8) */
    if (!cs) return;
    cs[0] = 0; cs[1] = hinst; cs[2] = hmenu; cs[3] = g_u32_win[i].parent;
    cs[4] = (uint32_t)g_u32_win[i].h;  cs[5] = (uint32_t)g_u32_win[i].w;
    cs[6] = (uint32_t)g_u32_win[i].y;  cs[7] = (uint32_t)g_u32_win[i].x;
    cs[8] = g_u32_win[i].style; cs[9] = name_ref; cs[10] = class_ref; cs[11] = g_u32_win[i].exstyle;
    uint32_t *cbt = cs + 12;                      /* { CREATESTRUCT* lpcs; HWND hwndInsertAfter; } */
    cbt[0] = (uint32_t)(uintptr_t)cs; cbt[1] = 0;
    /* HOOKPROC(code, wParam, lParam): code=HCBT_CREATEWND(3), wParam=hwnd, lParam=&cbt.
     * u32_call_wndproc lays args as f[1..4]=hwnd,msg,wp,lp — so pass code,hwnd,cbt,0. */
    u32_call_wndproc(esp, proc, 3u, (uint32_t)(i + 1), (uint32_t)(uintptr_t)cbt, 0u);
    free(cs);
}

static int  u32_q_empty(void) { return g_u32_qh == g_u32_qt; }
static int  u32_q_push(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int nt = (g_u32_qt + 1) % U32_MAX_MSG;
    if (nt == g_u32_qh) return 0;          /* full */
    g_u32_q[g_u32_qt].hwnd = hwnd; g_u32_q[g_u32_qt].message = msg;
    g_u32_q[g_u32_qt].wParam = wp; g_u32_q[g_u32_qt].lParam = lp;
    g_u32_q[g_u32_qt].time = (uint32_t)(mono_ns() / 1000000ull);
    g_u32_q[g_u32_qt].ptx = 0; g_u32_q[g_u32_qt].pty = 0;
    g_u32_qt = nt; return 1;
}
/* Copy the queue head into a guest MSG (7 dwords), optionally removing it. */
static void u32_q_peek_copy(uint32_t *m, int remove) {
    if (m) {
        m[0] = g_u32_q[g_u32_qh].hwnd;   m[1] = g_u32_q[g_u32_qh].message;
        m[2] = g_u32_q[g_u32_qh].wParam; m[3] = g_u32_q[g_u32_qh].lParam;
        m[4] = g_u32_q[g_u32_qh].time;   m[5] = g_u32_q[g_u32_qh].ptx; m[6] = g_u32_q[g_u32_qh].pty;
    }
    if (remove) g_u32_qh = (g_u32_qh + 1) % U32_MAX_MSG;
}
static void u32_fill_quit(uint32_t *m) {
    if (!m) return;
    m[0] = 0; m[1] = U32_WM_QUIT; m[2] = (uint32_t)g_u32_quit_code; m[3] = 0;
    m[4] = (uint32_t)(mono_ns() / 1000000ull); m[5] = 0; m[6] = 0;
}
static void u32_fill_paint(uint32_t *m, int i) {
    if (!m) return;
    m[0] = (uint32_t)(i + 1); m[1] = U32_WM_PAINT; m[2] = 0; m[3] = 0;
    m[4] = (uint32_t)(mono_ns() / 1000000ull); m[5] = 0; m[6] = 0;
}
static int u32_any_timer(void) {
    for (int i = 0; i < U32_MAX_TIMER; i++) if (g_u32_timer[i].used) return 1;
    return 0;
}
/* Post a WM_TIMER for every timer whose due time has passed, and reschedule it.
 * Uses the real monotonic clock; a batch program that never loops never fires
 * one, so this stays deterministic in practice. */
static void u32_pump_timers(void) {
    uint64_t now = mono_ns();
    for (int i = 0; i < U32_MAX_TIMER; i++) {
        if (g_u32_timer[i].used && now >= g_u32_timer[i].due_ns) {
            u32_q_push(g_u32_timer[i].hwnd, U32_WM_TIMER, g_u32_timer[i].id, g_u32_timer[i].proc);
            g_u32_timer[i].due_ns = now + (uint64_t)g_u32_timer[i].elapse * 1000000ull;
        }
    }
}

/* --- Paint model (WM_PAINT / invalidation) ---------------------------------
 * A visible top-level window with a non-empty update region "owes" a WM_PAINT.
 * WM_PAINT is not queued: it is generated on demand (lowest priority, after the
 * posted queue and WM_QUIT) so a program's paint handler runs and draws — this is
 * what makes a visible window actually show content. Display-free & sound: the
 * message flow is correct Win32 behaviour whether or not a real screen (SDL) is
 * present. Coalesced to a whole-client dirty flag (WM_PAINT unions regions). */
static int u32_win_paints(int i) {
    return i >= 0 && i < U32_MAX_WIN && g_u32_win[i].used
        && g_u32_win[i].visible && g_u32_win[i].parent == 0;   /* excl. child/message-only */
}
/* The next visible window owing a WM_PAINT, or -1. */
static int u32_next_paint(void) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].needs_paint && u32_win_paints(i)) return i;
    return -1;
}
/* Fill a DC's whole surface with a brush colour (the default WM_ERASEBKGND). No
 * surface (display-free / null brush) -> sound no-op. Defined after the GDI model. */
static void u32_fill_dc_brush(uint32_t hdc, uint32_t brush);
static void u32_erase_window_client(int wi);

/* RegisterClassW(const WNDCLASSW*) -> ATOM. Fields (32-bit): lpfnWndProc @+4,
 * hbrBackground @+28, lpszClassName @+36. Returns a non-zero atom; 0 on failure. */
uint32_t aret_RegisterClassW(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const uint16_t *name = (const uint16_t *)(uintptr_t)wc[9]; /* +36 lpszClassName */
    if (!name) return 0;
    /* WNDCLASS: style@0 wndproc@4 clsx@8 wndx@12 hInst@16 hIcon@20 hCursor@24 hbr@28 menu@32 */
    return u32_class_register(wc[0], wc[1], wc[2], wc[3], wc[4], wc[5], wc[6], wc[7], wc[8], 0, name);
}
/* RegisterClassA(const WNDCLASSA*) — same 40-byte layout as WNDCLASSW but a narrow
 * class name; widen it and share the one registry. */
uint32_t aret_RegisterClassA(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const char *name = (const char *)(uintptr_t)wc[9];
    if (!name) return 0;
    uint16_t wname[128];
    u32_a2w(name, wname, 128);
    return u32_class_register(wc[0], wc[1], wc[2], wc[3], wc[4], wc[5], wc[6], wc[7], wc[8], 0, wname);
}
/* RegisterClassExW(const WNDCLASSEXW*) -> ATOM. WNDCLASSEX (32-bit) shifts every
 * field +4 vs WNDCLASS (cbSize @0): lpfnWndProc @+8, hbrBackground @+32,
 * lpszClassName @+40. Modern apps use this form. */
uint32_t aret_RegisterClassExW(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const uint16_t *name = (const uint16_t *)(uintptr_t)wc[10]; /* +40 lpszClassName */
    if (!name) return 0;
    /* WNDCLASSEX: cbSize@0 style@4 wndproc@8 clsx@12 wndx@16 hInst@20 hIcon@24 hCursor@28
     * hbr@32 menu@36 name@40 hIconSm@44 */
    return u32_class_register(wc[1], wc[2], wc[3], wc[4], wc[5], wc[6], wc[7], wc[8], wc[9], wc[11], name);
}
/* RegisterClassExA(const WNDCLASSEXA*) — narrow class name; widen and share. */
uint32_t aret_RegisterClassExA(uint32_t esp) {
    const uint32_t *wc = (const uint32_t *)WP(0);
    if (!wc) return 0;
    const char *name = (const char *)(uintptr_t)wc[10];
    if (!name) return 0;
    uint16_t wname[128];
    u32_a2w(name, wname, 128);
    return u32_class_register(wc[1], wc[2], wc[3], wc[4], wc[5], wc[6], wc[7], wc[8], wc[9], wc[11], wname);
}

/* GetClassInfo(Ex)A/W(hInstance, lpClassName, lpWndClass) — fill the caller's
 * WNDCLASS(EX) from a registered class and return the class atom (non-zero); return
 * 0 if the class is not registered. Fields are returned verbatim as registered, so a
 * register→query round-trip is exact (matching Wine). `lpClassName` may be an atom or
 * a string pointer; the A form widens the string first. The WNDCLASS layout is shared
 * A/W (only the string encoding of names differs, and names are pointers we pass
 * through). `_ex` writes the WNDCLASSEX layout (every field shifted +4 past cbSize,
 * plus hIconSm). */
static uint32_t u32_get_class_info(uint32_t cref, uint32_t out, uint32_t clsname_ref, int is_ex) {
    int i = u32_class_index(cref);
    if (i < 0 || out == 0) return 0;
    uint32_t *o = (uint32_t *)(uintptr_t)out;
    if (is_ex) {
        /* cbSize (o[0]) is caller-supplied — GetClassInfoEx leaves it untouched (Wine). */
        o[1] = g_u32_class[i].style;
        o[2] = g_u32_class[i].wndproc;
        o[3] = g_u32_class[i].cls_extra;
        o[4] = g_u32_class[i].wnd_extra;
        o[5] = g_u32_class[i].hinstance;
        o[6] = g_u32_class[i].hicon;
        o[7] = g_u32_class[i].hcursor;
        o[8] = g_u32_class[i].hbr_bg;
        o[9] = g_u32_class[i].menu_name;
        o[10] = clsname_ref;
        o[11] = g_u32_class[i].hicon_sm;
    } else {
        o[0] = g_u32_class[i].style;
        o[1] = g_u32_class[i].wndproc;
        o[2] = g_u32_class[i].cls_extra;
        o[3] = g_u32_class[i].wnd_extra;
        o[4] = g_u32_class[i].hinstance;
        o[5] = g_u32_class[i].hicon;
        o[6] = g_u32_class[i].hcursor;
        o[7] = g_u32_class[i].hbr_bg;
        o[8] = g_u32_class[i].menu_name;
        o[9] = clsname_ref;              /* lpszClassName = the queried name */
    }
    return 0xC000u + (uint32_t)i;        /* the class atom (non-zero) */
}
/* The A form widens a string class name into a temp so the shared wide registry
 * lookup applies; an atom (< 0x10000) passes through unchanged. */
static uint32_t u32_get_class_info_a(uint32_t esp, int is_ex) {
    uint32_t cref = WP(1), out = WP(2);
    uint16_t wtmp[128];
    if (cref >= 0x10000u) {
        u32_a2w((const char *)(uintptr_t)cref, wtmp, 128);
        cref = (uint32_t)(uintptr_t)wtmp;
    }
    return u32_get_class_info(cref, out, WP(1), is_ex);
}
uint32_t aret_GetClassInfoA(uint32_t esp)   { return u32_get_class_info_a(esp, 0); }
uint32_t aret_GetClassInfoExA(uint32_t esp) { return u32_get_class_info_a(esp, 1); }
uint32_t aret_GetClassInfoW(uint32_t esp)   { return u32_get_class_info(WP(1), WP(2), WP(1), 0); }
uint32_t aret_GetClassInfoExW(uint32_t esp) { return u32_get_class_info(WP(1), WP(2), WP(1), 1); }
/* UnregisterClassW(lpClassName, hInstance) -> BOOL. */
uint32_t aret_UnregisterClassW(uint32_t esp) {
    uint32_t cref = WU(0);
    if (cref && cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) { g_u32_class[idx].used = 0; return 1; }
        return 0;
    }
    const uint16_t *name = (const uint16_t *)(uintptr_t)cref;
    if (!name) return 0;
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, name)) { g_u32_class[i].used = 0; return 1; }
    return 0;
}
/* CreateWindowExW(exStyle@0, className@1, winName@2, style@3, x@4,y@5,w@6,h@7,
 * parent@8, menu@9, inst@10, param@11) -> HWND. Binds the class WNDPROC and
 * captures geometry/style/text; visible windows get real pixels in G2b. */
/* Resolve a class reference (atom or name pointer) to its registered class name. */
static void u32_class_name(uint32_t cref, int wide, char *out, int cap) {
    out[0] = 0;
    if (cref == 0) return;
    if (cref < 0x10000u) {                           /* ATOM -> registry wide name */
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) u32_w2n(g_u32_class[idx].name, out, cap);
        return;
    }
    if (wide) u32_w2n((const uint16_t *)(uintptr_t)cref, out, cap);
    else { const char *s = (const char *)(uintptr_t)cref; int k = 0; for (; s[k] && k < cap - 1; k++) out[k] = s[k]; out[k] = 0; }
}
uint32_t aret_CreateWindowExW(uint32_t esp) {
    char title[256]; u32_w2n((const uint16_t *)WP(2), title, sizeof title);
    uint32_t h = u32_window_create(u32_class_wndproc(WU(1)), WU(0), WU(3),
                                   WU(4), WU(5), WU(6), WU(7), WU(8), title, u32_is_ctrl_class(WU(1), 1));
    if (h) { u32_class_name(WU(1), 1, g_u32_win[h - 1].classname, sizeof g_u32_win[h - 1].classname);
             g_u32_win[h - 1].bg_brush = u32_class_brush(WU(1));
             g_u32_win[h - 1].unicode = 1;
             uint32_t we = u32_class_wndextra(WU(1));
             g_u32_win[h - 1].extra_len = we > 64 ? 64 : (int)we;
             if (WU(3) & 0x40000000u) g_u32_win[h - 1].ctrl_id = (int)WU(9);  /* WS_CHILD: hMenu=id */
             u32_fire_cbt_createwnd(esp, (int)h - 1, WU(8), WU(9), WU(1), WU(2));  /* MFC CWnd attach */
             if (!u32_create_dispatch(esp, (int)h - 1, WU(8), WU(9))) { g_u32_win[h - 1].used = 0; return 0; } }
    return h;
}
/* CreateWindowExA — className@1 is a narrow name (or an atom). Widen a name to
 * look it up in the shared registry; atoms (<0x10000) pass through unchanged. The
 * window name @2 is already narrow. */
uint32_t aret_CreateWindowExA(uint32_t esp) {
    uint32_t cref = WU(1);
    uint16_t wbuf[128];
    if (cref >= 0x10000u) {
        u32_a2w((const char *)(uintptr_t)cref, wbuf, 128);
        cref = (uint32_t)(uintptr_t)wbuf;
    }
    uint32_t h = u32_window_create(u32_class_wndproc(cref), WU(0), WU(3),
                                   WU(4), WU(5), WU(6), WU(7), WU(8), WCS(2), u32_is_ctrl_class(WU(1), 0));
    if (h) { u32_class_name(WU(1), 0, g_u32_win[h - 1].classname, sizeof g_u32_win[h - 1].classname);
             g_u32_win[h - 1].bg_brush = u32_class_brush(cref);
             uint32_t we = u32_class_wndextra(cref);
             g_u32_win[h - 1].extra_len = we > 64 ? 64 : (int)we;
             if (WU(3) & 0x40000000u) g_u32_win[h - 1].ctrl_id = (int)WU(9);  /* WS_CHILD: hMenu=id */
             u32_fire_cbt_createwnd(esp, (int)h - 1, WU(8), WU(9), WU(1), WU(2));  /* MFC CWnd attach */
             if (!u32_create_dispatch(esp, (int)h - 1, WU(8), WU(9))) { g_u32_win[h - 1].used = 0; return 0; } }
    return h;
}
/* DestroyWindow(HWND) -> BOOL. */
uint32_t aret_DestroyWindow(uint32_t esp) {
    uint32_t h = WU(0);
    if (h >= 1 && h <= U32_MAX_WIN && g_u32_win[h - 1].used) {
#ifdef ARET_HAVE_SDL
        sdl_window_destroy((int)h - 1);
#endif
        g_u32_win[h - 1].used = 0;
    }
    return 1;
}
/* Default handling for the window-text messages (WM_SETTEXT/GETTEXT/GETTEXTLENGTH),
 * shared by DefWindowProcA (narrow) and DefWindowProcW (wide). Returns 1 and sets
 * *out if it handled the message, 0 to fall through. This is the real Windows path:
 * Set/GetWindowText send these messages, and DefWindowProc is what stores/reports
 * the text — so a subclass that intercepts them behaves exactly as under Wine. */
static int u32_defproc_text(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp,
                            int wide, uint32_t *out) {
    int i = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    switch (msg) {
    case U32_WM_SETTEXT:
        if (i < 0) { *out = 0; return 1; }
        if (wide) u32_w2n((const uint16_t *)(uintptr_t)lp, g_u32_win[i].title, sizeof g_u32_win[i].title);
        else { const char *s = (const char *)(uintptr_t)lp; int k = 0;
               if (s) for (; s[k] && k < 255; k++) g_u32_win[i].title[k] = s[k];
               g_u32_win[i].title[k] = 0; }
        *out = 1; return 1;                 /* TRUE */
    case U32_WM_GETTEXTLENGTH:
        *out = (i < 0) ? 0 : (uint32_t)strlen(g_u32_win[i].title); return 1;
    case U32_WM_GETTEXT: {
        uint32_t n = wp;                    /* buffer capacity in chars, incl. NUL */
        if (i < 0 || n == 0 || lp == 0) { *out = 0; return 1; }
        const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
        uint32_t cc = len < n - 1 ? len : n - 1;
        if (wide) { uint16_t *d = (uint16_t *)(uintptr_t)lp;
                    for (uint32_t j = 0; j < cc; j++) d[j] = (uint16_t)(unsigned char)t[j]; d[cc] = 0; }
        else      { char *d = (char *)(uintptr_t)lp;
                    for (uint32_t j = 0; j < cc; j++) d[j] = t[j]; d[cc] = 0; }
        *out = cc; return 1;
    }
    default: return 0;
    }
}
/* DefWindowProc handling common to A/W, independent of text width: WM_PAINT does
 * a default paint (validates the update region so a program that delegates
 * painting doesn't loop forever on WM_PAINT); WM_CLOSE destroys the window after
 * a WM_DESTROY, so a real close (the SDL window's X button) actually closes. Both
 * fire only on generated/real events, never in the deterministic headless oracle.
 * Returns 1 if handled, with the result in *out. */
static int u32_defproc_common(uint32_t esp, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t *out) {
    int i = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    if (msg == 0x0081u /* WM_NCCREATE */) { *out = 1; return 1; }  /* accept creation */
    if (msg == 0x0001u /* WM_CREATE */)   { *out = 0; return 1; }  /* 0 = success (not -1) */
    if (msg == U32_WM_PAINT) {
        /* Default paint: real DefWindowProc runs BeginPaint/EndPaint, which erases the
         * class background (WM_ERASEBKGND). A window with no WM_PAINT handler must still
         * show its background on screen, so perform that erase here before validating. */
        if (i >= 0) {
            if (g_u32_win[i].needs_erase) { u32_erase_window_client(i); g_u32_win[i].needs_erase = 0; }
            g_u32_win[i].needs_paint = 0;
        }
        *out = 0; return 1;
    }
    if (msg == U32_WM_ERASEBKGND) {   /* default erase: fill client with class brush */
        if (i >= 0 && g_u32_win[i].bg_brush) u32_fill_dc_brush(wp /* HDC */, g_u32_win[i].bg_brush);
        *out = 1; return 1;           /* TRUE = background erased */
    }
    if (msg == 0x0010u /* WM_CLOSE */) {
        if (i >= 0) {
            uint32_t wp = g_u32_win[i].wndproc;
            if (wp) u32_call_wndproc(esp, wp, hwnd, 0x0002u /* WM_DESTROY */, 0, 0);
#ifdef ARET_HAVE_SDL
            sdl_window_destroy(i);
#endif
            g_u32_win[i].used = 0;
        }
        *out = 0; return 1;
    }
    return 0;
}
/* DefWindowProcW(HWND,UINT,WPARAM,LPARAM) -> LRESULT. Handles the text messages
 * (wide) + the common paint/close defaults; everything else defaults to 0. */
/* DefWindowProc: for a window that IS a predefined control (its implicit "system
 * proc" is u32_control_proc — our controls carry wndproc==0), also answer the control's
 * class messages here. A subclasser (MFC) whose saved original proc is NULL chains
 * unhandled messages to ::DefWindowProc (not CallWindowProc), so CB_ADDSTRING/CB_SETCURSEL/
 * BM_SETCHECK on a subclassed combo/button arrive here — without this they were dropped
 * (items/selection/check lost). u32_control_proc returns 0 for non-control windows, so
 * ordinary windows are unaffected. */
static int u32_control_proc(uint32_t esp, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t *out);
uint32_t aret_DefWindowProcW(uint32_t esp) {
    uint32_t r;
    if (u32_control_proc(esp, WU(0), WU(1), WU(2), WU(3), &r)) return r;
    if (u32_defproc_common(esp, WU(0), WU(1), WU(2), &r)) return r;
    if (u32_defproc_text(WU(0), WU(1), WU(2), WU(3), 1, &r)) return r;
    return 0;
}
/* PostMessageW(HWND,UINT,WPARAM,LPARAM) -> BOOL. Enqueue. */
uint32_t aret_PostMessageW(uint32_t esp) { return (uint32_t)u32_q_push(WU(0), WU(1), WU(2), WU(3)); }
/* A predefined control (BUTTON/…) has no app WNDPROC; its messages route to the
 * built-in control proc (paint, font). Fwd-declared here, defined after the GDI
 * primitives it uses (u32_drawedge/u32_drawtext). */
static int u32_control_proc(uint32_t esp, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t *out);
/* Full dispatch for a predefined control with no application WNDPROC: first its class
 * behaviour (fonts, button check/click, list/combo item model, WM_PRINTCLIENT —
 * u32_control_proc), then the common text messages (WM_SETTEXT/WM_GETTEXT/
 * WM_GETTEXTLENGTH — u32_defproc_text). A dialog populates its controls through exactly
 * these two families (CB_ADDSTRING/CB_SETCURSEL, BM_SETCHECK, WM_SETTEXT, …); routing
 * SendMessage and SendDlgItemMessage through the same helper keeps a system control's
 * response identical whichever way the app addresses it. `wide` selects ANSI/UTF-16
 * for the text messages. Returns 1 if handled, with the result in *out. */
static int u32_sys_control_msg(uint32_t esp, uint32_t hwnd, uint32_t msg,
                               uint32_t wp, uint32_t lp, int wide, uint32_t *out) {
    if (u32_control_proc(esp, hwnd, msg, wp, lp, out)) return 1;
    return u32_defproc_text(hwnd, msg, wp, lp, wide, out);
}
/* Hit-test a dialog's controls at client (x,y) and deliver a click to the one under the
 * point (defined with the control paint code). Used by the real-input path (a mouse
 * release on a dialog) so a click reaches the child control, as Wine's per-window input
 * would. */
static void u32_dialog_hittest_click(uint32_t esp, int di, int x, int y);
static void u32_ctrl_recomposite(uint32_t esp, int ci);   /* fwd: recompose a control's parent dialog (no-op without SDL) */
static uint32_t g_u32_focus;   /* fwd: focused window/control (keyboard target); defined below */
/* SendMessage(HWND,UINT,WPARAM,LPARAM) -> LRESULT. Synchronous: call the WNDPROC now; a
 * predefined control with no WNDPROC is served by the system-control dispatch. */
static uint32_t u32_send_message(uint32_t esp, int wide) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) { uint32_t r = 0; if (u32_sys_control_msg(esp, WU(0), WU(1), WU(2), WU(3), wide, &r)) return r; return 0; }
    return u32_call_wndproc(esp, wndproc, WU(0), WU(1), WU(2), WU(3));
}
uint32_t aret_SendMessageW(uint32_t esp) { return u32_send_message(esp, 1); }
/* DispatchMessageW(const MSG*) -> LRESULT. Route to the window's WNDPROC (or the
 * TIMERPROC in lParam for a WM_TIMER carrying one). */
uint32_t aret_DispatchMessageW(uint32_t esp) {
    const uint32_t *m = (const uint32_t *)WP(0);
    if (!m) return 0;
    uint32_t hwnd = m[0], msg = m[1], wp = m[2], lp = m[3];
    if (msg == U32_WM_TIMER && lp)          /* TIMERPROC callback */
        return u32_call_wndproc(esp, lp, hwnd, msg, wp, (uint32_t)(mono_ns() / 1000000ull));
    uint32_t wndproc = u32_win_wndproc(hwnd);
    if (!wndproc) return 0;
    uint32_t ret = u32_call_wndproc(esp, wndproc, hwnd, msg, wp, lp);
    /* After the DLGPROC sees a left-button release on a dialog (it usually ignores it),
     * route the click to the child control under the cursor — our controls are not
     * separate input windows, so the dialog does the hit-test that Wine's per-window
     * input would. Works for real SDL clicks and for a posted WM_LBUTTONUP. */
    int di = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    if (di >= 0 && g_u32_win[di].is_dialog && msg == 0x0202u /*WM_LBUTTONUP*/)
        u32_dialog_hittest_click(esp, di, (int)(int16_t)(lp & 0xFFFF), (int)(int16_t)((lp >> 16) & 0xFFFF));
    return ret;
}
/* TranslateMessage(const MSG*) -> BOOL. No keyboard input in this model -> no
 * WM_CHAR synthesis; returns 0 (nothing translated), which is correct here. */
uint32_t aret_TranslateMessage(uint32_t esp) { (void)esp; return 0; }
/* PeekMessageW(lpMsg, hwnd, min, max, wRemoveMsg) -> BOOL. Non-blocking. */
uint32_t aret_PeekMessageW(uint32_t esp) {
    uint32_t *m = (uint32_t *)WP(0);
    uint32_t remove = WU(4);
#ifdef ARET_HAVE_SDL
    sdl_pump();          /* drain real keyboard/mouse/close events into the queue */
#endif
    u32_pump_timers();
    if (!u32_q_empty()) { u32_q_peek_copy(m, (remove & U32_PM_REMOVE) != 0); return 1; }
    if (g_u32_quit)    { u32_fill_quit(m); return 1; }
    /* WM_PAINT is generated on demand (never queued), lowest priority. Peeking
     * does not clear the update region — BeginPaint/ValidateRect do. */
    { int pi = u32_next_paint(); if (pi >= 0) { u32_fill_paint(m, pi); return 1; } }
    return 0;
}
/* GetMessageW(lpMsg, hwnd, min, max) -> BOOL (0 = WM_QUIT, else 1). Blocks until a
 * message is available. In the mono-thread model the only wakers are the queue
 * (already posted) and timers, so if the queue is empty with no quit and no timer
 * nothing can ever arrive -> we abort loudly rather than hang or fake a quit. */
uint32_t aret_GetMessageW(uint32_t esp) {
    uint32_t *m = (uint32_t *)WP(0);
    for (;;) {
#ifdef ARET_HAVE_SDL
        sdl_pump();      /* drain real keyboard/mouse/close events into the queue */
#endif
        u32_pump_timers();
        if (!u32_q_empty()) { u32_q_peek_copy(m, 1); return 1; }
        if (g_u32_quit)    { u32_fill_quit(m); return 0; }
        /* WM_PAINT: generated on demand, after the posted queue and WM_QUIT. */
        { int pi = u32_next_paint(); if (pi >= 0) { u32_fill_paint(m, pi); return 1; } }
#ifdef ARET_HAVE_SDL
        /* A real visible window is a message source (its close button, input): the
         * queue can wake even with no timer, so block on SDL events instead of
         * aborting. When no window is shown either, fall through to the honest
         * mono-thread abort below. */
        if (sdl_any_window()) { usleep(2000); continue; }
#endif
        if (!u32_any_timer())
            aret_partial("GetMessageW: empty queue, no WM_QUIT, no timer (would block forever in mono-thread model)");
        usleep(1000);                        /* wait for the next timer to come due */
    }
}
/* SetTimer(hwnd, nIDEvent, uElapse_ms, TIMERPROC) -> UINT_PTR. */
uint32_t aret_SetTimer(uint32_t esp) {
    uint32_t hwnd = WU(0), id = WU(1), elapse = WU(2), proc = WU(3);
    if (hwnd == 0 && id == 0) id = g_u32_next_timer_id++;   /* window-less timer gets a fresh id */
    int slot = -1;
    for (int i = 0; i < U32_MAX_TIMER; i++) {               /* reuse an existing (hwnd,id) */
        if (g_u32_timer[i].used && g_u32_timer[i].hwnd == hwnd && g_u32_timer[i].id == id) { slot = i; break; }
        if (slot < 0 && !g_u32_timer[i].used) slot = i;
    }
    if (slot < 0) return 0;
    g_u32_timer[slot].used = 1; g_u32_timer[slot].hwnd = hwnd; g_u32_timer[slot].id = id;
    g_u32_timer[slot].elapse = elapse; g_u32_timer[slot].proc = proc;
    g_u32_timer[slot].due_ns = mono_ns() + (uint64_t)elapse * 1000000ull;
    return id;
}
/* KillTimer(hwnd, id) -> BOOL. */
uint32_t aret_KillTimer(uint32_t esp) {
    uint32_t hwnd = WU(0), id = WU(1);
    for (int i = 0; i < U32_MAX_TIMER; i++)
        if (g_u32_timer[i].used && g_u32_timer[i].hwnd == hwnd && g_u32_timer[i].id == id) { g_u32_timer[i].used = 0; return 1; }
    return 0;
}
/* PostQuitMessage(nExitCode) -> void. */
uint32_t aret_PostQuitMessage(uint32_t esp) { g_u32_quit = 1; g_u32_quit_code = WI(0); return 0; }
/* MsgWaitForMultipleObjectsEx(nCount, pHandles, dwMs, dwWakeMask, dwFlags) -> DWORD.
 * A message available -> WAIT_OBJECT_0 + nCount. We do not model signaled guest
 * handles here, so otherwise report WAIT_TIMEOUT (0x102). */
uint32_t aret_MsgWaitForMultipleObjectsEx(uint32_t esp) {
    uint32_t nCount = WU(0);
    u32_pump_timers();
    if (!u32_q_empty() || g_u32_quit) return nCount;   /* WAIT_OBJECT_0 (=0) + nCount */
    return 0x00000102u;                                /* WAIT_TIMEOUT */
}

/* --- ANSI (A) twins. The message ops carry no text at this layer (message-only
 * windows have no keyboard/WM_CHAR translation), so A is byte-identical to W;
 * only the class-name-bearing RegisterClassA/CreateWindowExA and the text-bearing
 * DefWindowProcA/Set-GetWindowTextA differ (ANSI vs wide payloads). --- */
uint32_t aret_DefWindowProcA(uint32_t esp) {
    uint32_t r;
    if (u32_control_proc(esp, WU(0), WU(1), WU(2), WU(3), &r)) return r;   /* subclassed control class msgs */
    if (u32_defproc_common(esp, WU(0), WU(1), WU(2), &r)) return r;
    if (u32_defproc_text(WU(0), WU(1), WU(2), WU(3), 0, &r)) return r;  /* narrow */
    return 0;
}
uint32_t aret_GetMessageA(uint32_t esp)      { return aret_GetMessageW(esp); }
uint32_t aret_PeekMessageA(uint32_t esp)     { return aret_PeekMessageW(esp); }
uint32_t aret_DispatchMessageA(uint32_t esp) { return aret_DispatchMessageW(esp); }
uint32_t aret_PostMessageA(uint32_t esp)     { return aret_PostMessageW(esp); }
uint32_t aret_SendMessageA(uint32_t esp)     { return u32_send_message(esp, 0); }
/* UnregisterClassA(lpClassName, hInstance) — widen a narrow name, else atom. */
uint32_t aret_UnregisterClassA(uint32_t esp) {
    uint32_t cref = WU(0);
    if (cref && cref < 0x10000u) {
        uint32_t idx = cref - 0xC000u;
        if (idx < U32_MAX_CLASSES && g_u32_class[idx].used) { g_u32_class[idx].used = 0; return 1; }
        return 0;
    }
    if (!cref) return 0;
    uint16_t wname[128];
    u32_a2w((const char *)(uintptr_t)cref, wname, 128);
    for (int i = 0; i < U32_MAX_CLASSES; i++)
        if (g_u32_class[i].used && u32_weq(g_u32_class[i].name, wname)) { g_u32_class[i].used = 0; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window geometry / state / text (G2a, display-free)                 */
/*                                                                    */
/* This models the window manager's *state* (rect, style, text, show/ */
/* enable) so the geometry & text APIs round-trip against Wine without */
/* a real screen. Actual pixels (SDL_Window) are G2b. Geometry APIs   */
/* read/write the window struct directly (as Windows' manager does);  */
/* text APIs go through WM_SETTEXT/GETTEXT so subclasses see them.     */
/* ------------------------------------------------------------------ */

/* Valid live-window index for hwnd, or -1. */
static int u32_win_idx(uint32_t hwnd) {
    return (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
}

/* GetWindowRect(HWND, RECT*) -> BOOL. Screen coords {left,top,right,bottom} =
 * {x, y, x+w, y+h} (Wine returns exactly the CreateWindow geometry). The desktop
 * pseudo-window reports the virtual screen. */
uint32_t aret_GetWindowRect(uint32_t esp) {
    uint32_t hwnd = WU(0); int32_t *r = (int32_t *)WP(1);
    if (!r) return 0;
    if (hwnd == U32_DESKTOP) { r[0] = 0; r[1] = 0; r[2] = U32_SCREEN_W; r[3] = U32_SCREEN_H; return 1; }
    int i = u32_win_idx(hwnd);
    if (i < 0) return 0;
    r[0] = g_u32_win[i].x; r[1] = g_u32_win[i].y;
    r[2] = g_u32_win[i].x + g_u32_win[i].w; r[3] = g_u32_win[i].y + g_u32_win[i].h;
    return 1;
}
/* SetWindowPos(HWND, hwndInsertAfter, X, Y, cx, cy, uFlags) -> BOOL. Honours
 * SWP_NOMOVE/NOSIZE and SWP_SHOW/HIDEWINDOW; Z-order is a no-op in this model. */
uint32_t aret_SetWindowPos(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    uint32_t f = WU(6);
    if (!(f & 0x0002u)) { g_u32_win[i].x = WI(2); g_u32_win[i].y = WI(3); }  /* !SWP_NOMOVE */
    if (!(f & 0x0001u)) { g_u32_win[i].w = WI(4); g_u32_win[i].h = WI(5); }  /* !SWP_NOSIZE */
    if (f & 0x0040u) g_u32_win[i].visible = 1;                               /* SWP_SHOWWINDOW */
    if (f & 0x0080u) g_u32_win[i].visible = 0;                               /* SWP_HIDEWINDOW */
    return 1;
}
/* MoveWindow(HWND, X, Y, nWidth, nHeight, bRepaint) -> BOOL. */
uint32_t aret_MoveWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    g_u32_win[i].x = WI(1); g_u32_win[i].y = WI(2);
    g_u32_win[i].w = WI(3); g_u32_win[i].h = WI(4);
    return 1;
}
/* ShowWindow(HWND, nCmdShow) -> BOOL (nonzero = was previously visible). SW_HIDE=0
 * hides; any other command shows (minimise/maximise are still "visible" here). */
uint32_t aret_ShowWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    int was = g_u32_win[i].visible;
    g_u32_win[i].visible = (WU(1) == 0 /* SW_HIDE */) ? 0 : 1;
    /* Becoming visible invalidates the whole window -> owes a WM_PAINT (erase). */
    if (g_u32_win[i].visible && !was && g_u32_win[i].parent == 0
        && !(g_u32_win[i].style & 0x40000000u /* WS_CHILD */)) {
        g_u32_win[i].needs_paint = 1;
        g_u32_win[i].needs_erase = 1;
    }
#ifdef ARET_HAVE_SDL
    if (g_u32_win[i].visible && g_u32_win[i].parent == 0 && !(g_u32_win[i].style & 0x40000000u))
        sdl_window_show(esp, i);
    else if (!g_u32_win[i].visible && g_u32_win[i].sdl_win)
        SDL_HideWindow((SDL_Window *)g_u32_win[i].sdl_win);
#endif
    return (uint32_t)was;
}
/* UpdateWindow(HWND) -> BOOL. No invalid region is tracked yet (GDI paint is G6),
 * so there is nothing to repaint; report success for a valid window. */
uint32_t aret_UpdateWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    /* UpdateWindow paints immediately if the update region is non-empty: send
     * WM_PAINT to the WNDPROC now (it Begin/EndPaints, which draws & validates). */
    if (i >= 0 && g_u32_win[i].needs_paint && u32_win_paints(i)) {
        uint32_t wp = g_u32_win[i].wndproc;
        if (wp) u32_call_wndproc(esp, wp, (uint32_t)(i + 1), U32_WM_PAINT, 0, 0);
    }
#ifdef ARET_HAVE_SDL
    if (i >= 0) u32_present_toplevel(esp, i);   /* compose children (dialog or plain) then flush to screen */
#endif
    return i >= 0 ? 1u : 0u;
}
/* EnableWindow(HWND, bEnable) -> BOOL (nonzero = was previously DISABLED). */
uint32_t aret_EnableWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    int was_disabled = !g_u32_win[i].enabled;
    g_u32_win[i].enabled = WU(1) ? 1 : 0;
    return (uint32_t)was_disabled;
}
/* GetParent(HWND) -> HWND. Returns the stored parent/owner (0 for a top-level). */
uint32_t aret_GetParent(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return i < 0 ? 0 : g_u32_win[i].parent;
}

/* ---- Mouse capture (window-manager state) --------------------------------
 * A single window holds the capture. SetCapture returns the previous holder (0 if
 * none); GetCapture returns the current; ReleaseCapture clears it (TRUE). Measured vs
 * Wine. Headless there is no real mouse, but the state round-trips exactly. */
static uint32_t g_u32_capture = 0;
uint32_t aret_SetCapture(uint32_t esp) { uint32_t p = g_u32_capture; g_u32_capture = WU(0); return p; }
uint32_t aret_GetCapture(uint32_t esp) { (void)esp; return g_u32_capture; }
uint32_t aret_ReleaseCapture(uint32_t esp) { (void)esp; g_u32_capture = 0; return 1; }
/* GetCursorPos(POINT*): the mouse position. Headless there is no mouse, so this is the
 * screen-origin invariant (0,0) — like the screen-size metrics, an env-independent
 * model value, not a guess (returns TRUE, fills the POINT so the caller never reads
 * uninitialised memory). */
uint32_t aret_GetCursorPos(uint32_t esp) {
    int32_t *p = (int32_t *)WP(0);
    if (p) { p[0] = 0; p[1] = 0; }
    return 1;
}
/* GetProcessVersion(pid): the Windows version a process expects (major<<16|minor).
 * The modelled era is Windows 4.0 (Win95/NT4 subsystem) = 0x00040000, measured vs Wine
 * for the Win95-era corpus. */
uint32_t aret_GetProcessVersion(uint32_t esp) { (void)esp; return 0x00040000u; }
/* SetMessageQueue(cMessagesMax): obsolete Win16 API; on Win32 it is a no-op that
 * returns TRUE (measured vs Wine). */
uint32_t aret_SetMessageQueue(uint32_t esp) { (void)esp; return 1; }

/* ---- Scroll bars (per-window, per-bar state) -----------------------------
 * Each window keeps {min,max,page,pos} for SB_HORZ(0)/SB_VERT(1)/SB_CTL(2). SetScrollPos
 * clamps pos to [min,max] and returns the previous pos; SetScrollRange clamps pos into
 * the new range; SetScrollInfo applies the fMask fields and clamps pos to [min, max -
 * (page>0 ? page-1 : 0)] (measured vs Wine: max valid pos = nMax-nPage+1 with a page).
 * nTrackPos = the current pos (no live drag headless). Deterministic round-trip. */
static int u32_sb_idx(uint32_t bar) { return bar <= 2 ? (int)bar : -1; }
static void u32_sb_clamp(int i, int b, int with_page) {
    int lo = g_u32_win[i].scroll[b].min;
    int hi = g_u32_win[i].scroll[b].max - (with_page && g_u32_win[i].scroll[b].page > 0 ? g_u32_win[i].scroll[b].page - 1 : 0);
    if (hi < lo) hi = lo;
    if (g_u32_win[i].scroll[b].pos < lo) g_u32_win[i].scroll[b].pos = lo;
    if (g_u32_win[i].scroll[b].pos > hi) g_u32_win[i].scroll[b].pos = hi;
}
uint32_t aret_SetScrollRange(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1)); if (i < 0 || b < 0) return 0;
    g_u32_win[i].scroll[b].min = (int)WU(2); g_u32_win[i].scroll[b].max = (int)WU(3);
    u32_sb_clamp(i, b, 0);
    return 1;
}
uint32_t aret_GetScrollRange(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1));
    int *pmn = (int *)WP(2), *pmx = (int *)WP(3);
    if (i < 0 || b < 0) { if (pmn) *pmn = 0; if (pmx) *pmx = 0; return 0; }
    if (pmn) *pmn = g_u32_win[i].scroll[b].min;
    if (pmx) *pmx = g_u32_win[i].scroll[b].max;
    return 1;
}
uint32_t aret_SetScrollPos(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1)); if (i < 0 || b < 0) return 0;
    int prev = g_u32_win[i].scroll[b].pos;
    g_u32_win[i].scroll[b].pos = (int)WU(2);
    u32_sb_clamp(i, b, 0);
    return (uint32_t)prev;
}
uint32_t aret_GetScrollPos(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1));
    return (i < 0 || b < 0) ? 0 : (uint32_t)g_u32_win[i].scroll[b].pos;
}
/* SCROLLINFO: cbSize@0, fMask@4, nMin@8, nMax@12, nPage@16, nPos@20, nTrackPos@24. */
uint32_t aret_SetScrollInfo(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1)); if (i < 0 || b < 0) return 0;
    const uint8_t *si = (const uint8_t *)WP(2); if (!si) return 0;
    uint32_t mask = *(const uint32_t *)(si + 4);
    if (mask & 0x1u /*SIF_RANGE*/) { g_u32_win[i].scroll[b].min = *(const int *)(si + 8); g_u32_win[i].scroll[b].max = *(const int *)(si + 12); }
    if (mask & 0x2u /*SIF_PAGE*/)  g_u32_win[i].scroll[b].page = *(const int *)(si + 16);
    if (mask & 0x4u /*SIF_POS*/)   g_u32_win[i].scroll[b].pos = *(const int *)(si + 20);
    u32_sb_clamp(i, b, 1);
    return (uint32_t)g_u32_win[i].scroll[b].pos;
}
uint32_t aret_GetScrollInfo(uint32_t esp) {
    int i = u32_win_idx(WU(0)), b = u32_sb_idx(WU(1));
    uint8_t *si = (uint8_t *)WP(2); if (!si) return 0;
    uint32_t mask = *(const uint32_t *)(si + 4);
    if (i < 0 || b < 0) return 0;
    if (mask & 0x1u) { *(int *)(si + 8) = g_u32_win[i].scroll[b].min; *(int *)(si + 12) = g_u32_win[i].scroll[b].max; }
    if (mask & 0x2u) *(int *)(si + 16) = g_u32_win[i].scroll[b].page;
    if (mask & 0x4u) *(int *)(si + 20) = g_u32_win[i].scroll[b].pos;
    if (mask & 0x10u /*SIF_TRACKPOS*/) *(int *)(si + 24) = g_u32_win[i].scroll[b].pos;
    return 1;
}
/* GetWindow(hwnd, cmd) -> HWND: navigate the window hierarchy. Children/siblings share
 * a parent and are ordered by creation (the g_u32_win index), which matches Wine's
 * default child Z-order (measured: create c1,c2,c3 under a -> GW_CHILD=c1,
 * GW_HWNDNEXT(c1)=c2, GW_HWNDLAST=c3, GW_HWNDFIRST=c1). NULL/desktop groups the
 * top-levels (parent 0). */
#define U32_GW_HWNDFIRST 0u
#define U32_GW_HWNDLAST  1u
#define U32_GW_HWNDNEXT  2u
#define U32_GW_HWNDPREV  3u
#define U32_GW_OWNER     4u
#define U32_GW_CHILD     5u
static uint32_t u32_first_child(uint32_t parent) {
    for (int k = 0; k < U32_MAX_WIN; k++)
        if (g_u32_win[k].used && g_u32_win[k].parent == parent) return (uint32_t)(k + 1);
    return 0;
}
uint32_t aret_GetWindow(uint32_t esp) {
    uint32_t hwnd = WU(0), cmd = WU(1);
    if (cmd == U32_GW_CHILD)
        return u32_first_child((hwnd == U32_DESKTOP) ? 0u : hwnd);
    int i = u32_win_idx(hwnd);
    if (i < 0) return 0;
    if (cmd == U32_GW_OWNER)
        return (g_u32_win[i].style & 0x40000000u /*WS_CHILD*/) ? 0u : g_u32_win[i].parent;
    uint32_t p = g_u32_win[i].parent;                 /* the sibling group */
    if (cmd == U32_GW_HWNDNEXT) {
        for (int k = i + 1; k < U32_MAX_WIN; k++)
            if (g_u32_win[k].used && g_u32_win[k].parent == p) return (uint32_t)(k + 1);
    } else if (cmd == U32_GW_HWNDPREV) {
        for (int k = i - 1; k >= 0; k--)
            if (g_u32_win[k].used && g_u32_win[k].parent == p) return (uint32_t)(k + 1);
    } else if (cmd == U32_GW_HWNDFIRST) {
        return u32_first_child(p);
    } else if (cmd == U32_GW_HWNDLAST) {
        for (int k = U32_MAX_WIN - 1; k >= 0; k--)
            if (g_u32_win[k].used && g_u32_win[k].parent == p) return (uint32_t)(k + 1);
    }
    return 0;
}
/* GetTopWindow(hwnd) -> the topmost child of hwnd in Z-order (= GW_CHILD), NULL if
 * childless; NULL/desktop -> the first top-level. */
uint32_t aret_GetTopWindow(uint32_t esp) {
    uint32_t hwnd = WU(0);
    return u32_first_child((hwnd == 0 || hwnd == U32_DESKTOP) ? 0u : hwnd);
}
/* GetDesktopWindow() -> HWND: the desktop pseudo-window. */
uint32_t aret_GetDesktopWindow(uint32_t esp) { (void)esp; return U32_DESKTOP; }
/* IsWindow(HWND) -> BOOL. */
uint32_t aret_IsWindow(uint32_t esp) {
    uint32_t h = WU(0);
    return (h == U32_DESKTOP || u32_win_idx(h) >= 0) ? 1u : 0u;
}
/* IsWindowVisible(HWND) -> BOOL. */
uint32_t aret_IsWindowVisible(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].visible) ? 1u : 0u;
}
/* IsWindowEnabled(HWND) -> BOOL. */
uint32_t aret_IsWindowEnabled(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].enabled) ? 1u : 0u;
}
/* IsIconic(HWND) -> BOOL. Windows are never minimised in this model. */
uint32_t aret_IsIconic(uint32_t esp) { (void)esp; return 0; }
/* GetWindowLongA(HWND, nIndex) -> LONG. Models the fields we store; other indices
 * (GWL_HINSTANCE/ID we never set) are 0, which is their value for these windows. */
uint32_t aret_GetWindowLongA(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    switch (WI(1)) {
    case -16: return g_u32_win[i].style;     /* GWL_STYLE */
    case -20: return g_u32_win[i].exstyle;   /* GWL_EXSTYLE */
    case -21: return g_u32_win[i].userdata;  /* GWL_USERDATA */
    case -4:  return g_u32_win[i].wndproc;   /* GWL_WNDPROC */
    case -8:  return g_u32_win[i].parent;    /* GWL_HWNDPARENT */
    default:  {                              /* cbWndExtra bytes (control state) */
        int off = WI(1);
        if (off >= 0 && off + 4 <= (int)sizeof g_u32_win[i].extra) {
            uint32_t v; memcpy(&v, g_u32_win[i].extra + off, 4); return v;
        }
        return 0;
    }
    }
}
uint32_t aret_GetWindowLongW(uint32_t esp) { return aret_GetWindowLongA(esp); }
/* SetWindowLongA(HWND, nIndex, dwNewLong) -> LONG (previous value). GWL_WNDPROC
 * assignment is real subclassing (later messages dispatch to the new proc). */
uint32_t aret_SetWindowLongA(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    if (i < 0) return 0;
    uint32_t v = WU(2), old;
    switch (WI(1)) {
    case -16: old = g_u32_win[i].style;    g_u32_win[i].style = v;    return old;
    case -20: old = g_u32_win[i].exstyle;  g_u32_win[i].exstyle = v;  return old;
    case -21: old = g_u32_win[i].userdata; g_u32_win[i].userdata = v; return old;
    case -4:  old = g_u32_win[i].wndproc;  g_u32_win[i].wndproc = v;  return old;
    default:  {                            /* cbWndExtra bytes (control state) */
        int off = WI(1);
        if (off >= 0 && off + 4 <= (int)sizeof g_u32_win[i].extra) {
            memcpy(&old, g_u32_win[i].extra + off, 4);
            memcpy(g_u32_win[i].extra + off, &v, 4);
            return old;
        }
        return 0;
    }
    }
}
uint32_t aret_SetWindowLongW(uint32_t esp) { return aret_SetWindowLongA(esp); }
/* IsWindowUnicode(HWND) -> BOOL. A window created through a W API is Unicode. */
uint32_t aret_IsWindowUnicode(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return (i >= 0 && g_u32_win[i].unicode) ? 1u : 0u;
}
/* RegisterWindowMessageA/W(lpString) -> UINT. A process-unique message id in the
 * [0xC000, 0xFFFF] range, identical for equal strings (like a global atom). */
#define U32_MAX_RWM 128
static struct { char name[128]; uint32_t id; int used; } g_u32_rwm[U32_MAX_RWM];
static uint32_t g_u32_rwm_next = 0xC000u;
static uint32_t u32_reg_win_msg(const char *s) {
    if (!s || !s[0]) return 0;
    for (int i = 0; i < U32_MAX_RWM; i++)
        if (g_u32_rwm[i].used && strncmp(g_u32_rwm[i].name, s, sizeof g_u32_rwm[i].name) == 0) return g_u32_rwm[i].id;
    for (int i = 0; i < U32_MAX_RWM; i++)
        if (!g_u32_rwm[i].used) {
            g_u32_rwm[i].used = 1;
            int k = 0; for (; s[k] && k < 127; k++) g_u32_rwm[i].name[k] = s[k]; g_u32_rwm[i].name[k] = 0;
            g_u32_rwm[i].id = g_u32_rwm_next++;
            return g_u32_rwm[i].id;
        }
    return 0;
}
uint32_t aret_RegisterWindowMessageA(uint32_t esp) { return u32_reg_win_msg(WCS(0)); }
uint32_t aret_RegisterWindowMessageW(uint32_t esp) {
    char buf[128]; u32_w2n((const uint16_t *)WP(0), buf, sizeof buf);
    return u32_reg_win_msg(buf);
}
/* RegisterClipboardFormatA/W(name) -> UINT. A process-unique clipboard-format id in
 * the global-atom range [0xC000, 0xFFFF], identical for equal names — the same
 * contract as RegisterWindowMessage, so share its allocator (Windows draws both from
 * the one global atom table). The weak stub returned 0 (= failure), which MFC treats
 * as a fatal init error. */
uint32_t aret_RegisterClipboardFormatA(uint32_t esp) { return u32_reg_win_msg(WCS(0)); }
uint32_t aret_RegisterClipboardFormatW(uint32_t esp) {
    char buf[128]; u32_w2n((const uint16_t *)WP(0), buf, sizeof buf);
    return u32_reg_win_msg(buf);
}
/* PathFindExtensionA/W(path) -> pointer to the last extension's '.' in path, or to
 * the terminating NUL if there is none. Reproduces Wine's shlwapi verbatim: scanning
 * forward, a '\\' or ' ' resets the candidate (an extension cannot cross a component
 * boundary or a space), a '.' records it. Returns a pointer INTO the caller's buffer. */
uint32_t aret_PathFindExtensionA(uint32_t esp) {
    uint32_t pv = WU(0);
    const char *p = (const char *)(uintptr_t)pv;
    if (!p) return pv;
    const char *last = NULL;
    for (; *p; p++) {
        if (*p == '\\' || *p == ' ') last = NULL;
        else if (*p == '.') last = p;
    }
    return (uint32_t)(uintptr_t)(last ? last : p);   /* p is at the terminating NUL */
}
uint32_t aret_PathFindExtensionW(uint32_t esp) {
    uint32_t pv = WU(0);
    const uint16_t *p = (const uint16_t *)(uintptr_t)pv;
    if (!p) return pv;
    const uint16_t *last = NULL;
    for (; *p; p++) {
        if (*p == (uint16_t)'\\' || *p == (uint16_t)' ') last = NULL;
        else if (*p == (uint16_t)'.') last = p;
    }
    return (uint32_t)(uintptr_t)(last ? last : p);
}
/* PathFindFileNameA/W(path) -> pointer INTO path, at the file-name component.
 * MEASURED against Wine, and the rule the measurements imply is not the obvious one:
 * a separator (`\\`, `/` or `:`) only counts when the character AFTER it exists and is
 * not itself `\\` or `/`. That single condition is what produces every observed answer —
 *   "C:\\dir\\sub\\file.txt" -> "file.txt"      (last real separator)
 *   "C:\\dir\\"             -> "dir\\"          (the TRAILING separator does not count)
 *   "C:\\"                 -> "C:\\"           (whole string, not the empty tail)
 *   "\\\\srv\\share\\f.dat"  -> "f.dat"         (the UNC's leading `\\\\` does not count)
 *   "a/b/c.txt"          -> "c.txt"         (forward slashes count)
 *   "C:file.txt"         -> "file.txt"      (a bare `:` counts)
 * A naive "return after the last separator" gets the trailing-separator and bare-root
 * cases wrong and would answer "" for both. NULL in, NULL out. */
#define ARET_PFFN_BODY(TYPE)                                                        \
    uint32_t pv = WU(0);                                                            \
    const TYPE *p = (const TYPE *)(uintptr_t)pv;                                    \
    if (!p) return pv;                                                              \
    const TYPE *last = p;                                                           \
    for (; *p; p++)                                                                 \
        if ((*p == (TYPE)'\\' || *p == (TYPE)'/' || *p == (TYPE)':') &&              \
            p[1] && p[1] != (TYPE)'\\' && p[1] != (TYPE)'/')                         \
            last = p + 1;                                                           \
    return (uint32_t)(uintptr_t)last;

uint32_t aret_PathFindFileNameA(uint32_t esp) { ARET_PFFN_BODY(char) }
uint32_t aret_PathFindFileNameW(uint32_t esp) { ARET_PFFN_BODY(uint16_t) }
#undef ARET_PFFN_BODY

/* shlwapi, the ROOT-AWARE lexical path family (wave 1). Eight functions that all
 * turn on the same question — where does this path's root end — so they are derived
 * and gated together: `winecorpus/win32_pathroot.c` sweeps ONE grid of 25 paths
 * through all of them, in A and W, on a poisoned buffer with a raw dump. Every rule
 * below reproduces all 25 measured rows; none of it is reasoned from the API docs,
 * because several of the answers contradict the obvious reading:
 *
 *   - `/` is NOT a separator for this family. `PathIsUNC("//server/share")` is FALSE
 *     and `PathIsRelative` of it is TRUE — even though `PathFindFileName`, three
 *     lines above, *does* treat `/` as a separator. The inconsistency is real and
 *     measured; do not "harmonise" it.
 *   - `PathIsRoot("\\\\server\\share")` is TRUE while `PathSkipRoot` of the same string
 *     is NULL: a share with no trailing backslash *is* a root, but there is nothing
 *     past it to skip to.
 *   - There is NO special case for the `\\\\?\\` extended prefix. It falls out of the
 *     plain "skip two backslash-separated components" rule: in `\\\\?\\C:\\x` the
 *     components are `?` and `C:`, which is why the root ends at 7, and in
 *     `\\\\?\\UNC\\srv\\sh\\f` they are `?` and `UNC`, which is why it ends at 8. A
 *     hand-written `\\\\?\\` case would have got the second one wrong.
 *   - `PathAddBackslash` on a 259-character path DOES write, producing 260 characters
 *     plus a NUL — it overflows a MAX_PATH buffer rather than refusing. Measured, and
 *     reproduced: the caller's buffer contract is the caller's problem, and diverging
 *     "for safety" would change behaviour the program may depend on.
 */
#define ARET_PATH_ROOT_FAMILY(TAG, TYPE)                                              \
/* A root is: "\", a drive root "X:\" exactly, or a UNC prefix with at most ONE       \
 * further separator anywhere in it. */                                               \
static int u32_p_isroot_##TAG(const TYPE *p) {                                        \
    if (!p || !*p) return 0;                                                          \
    if (*p == (TYPE)'\\') {                                                           \
        if (!p[1]) return 1;                                                          \
        if (p[1] != (TYPE)'\\') return 0;                                             \
        int seen = 0;                                                                 \
        for (p += 2; *p; p++)                                                         \
            if (*p == (TYPE)'\\') { if (seen) return 0; seen = 1; }                   \
        return 1;                                                                     \
    }                                                                                 \
    return p[1] == (TYPE)':' && p[2] == (TYPE)'\\' && !p[3];                          \
}                                                                                     \
/* Past the root, or NULL. UNC = skip exactly two backslash-terminated components;    \
 * otherwise a drive needs its separator ("C:" and "C:/dir" both have no root). */    \
static const TYPE *u32_p_skiproot_##TAG(const TYPE *p) {                              \
    if (!p) return 0;                                                                 \
    if (p[0] == (TYPE)'\\' && p[1] == (TYPE)'\\') {                                   \
        p += 2;                                                                       \
        for (int c = 0; c < 2; c++) {                                                 \
            while (*p && *p != (TYPE)'\\') p++;                                       \
            if (*p != (TYPE)'\\') return 0;                                           \
            p++;                                                                      \
        }                                                                             \
        return p;                                                                     \
    }                                                                                 \
    if (p[0] && p[1] == (TYPE)':' && p[2] == (TYPE)'\\') return p + 3;                \
    return 0;                                                                         \
}                                                                                     \
/* First index a file-spec removal is allowed to truncate to: past a leading "\" or   \
 * "\\", and past the last ":" (plus one separator if it follows). Protects the root  \
 * — `PathRemoveFileSpec("\\\\server")` leaves "\\\\", not "\". */                        \
static int u32_p_base_##TAG(const TYPE *p) {                                          \
    int b = 0;                                                                        \
    if (p[0] == (TYPE)'\\') { b = 1; if (p[1] == (TYPE)'\\') b = 2; }                 \
    for (int i = 0; p[i]; i++)                                                        \
        if (p[i] == (TYPE)':') { b = i + 1; if (p[b] == (TYPE)'\\') b++; }            \
    return b;                                                                         \
}                                                                                     \
uint32_t aret_PathIsUNC##TAG(uint32_t esp) {                                          \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                   \
    return p && p[0] == (TYPE)'\\' && p[1] == (TYPE)'\\';                             \
}                                                                                     \
uint32_t aret_PathIsRoot##TAG(uint32_t esp) {                                         \
    return (uint32_t)u32_p_isroot_##TAG((const TYPE *)(uintptr_t)WU(0));              \
}                                                                                     \
uint32_t aret_PathIsRelative##TAG(uint32_t esp) {                                     \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                   \
    if (!p || !*p) return 1;                                                          \
    return !(p[0] == (TYPE)'\\' || p[1] == (TYPE)':');                                \
}                                                                                     \
uint32_t aret_PathSkipRoot##TAG(uint32_t esp) {                                       \
    return (uint32_t)(uintptr_t)u32_p_skiproot_##TAG((const TYPE *)(uintptr_t)WU(0)); \
}                                                                                     \
/* Returns the NUL. An empty path is left empty (no backslash appended) and a path    \
 * at or over MAX_PATH is refused with NULL — both measured. */                       \
uint32_t aret_PathAddBackslash##TAG(uint32_t esp) {                                   \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                               \
    if (!p) return 0;                                                                 \
    int n = 0;                                                                        \
    while (p[n]) n++;                                                                 \
    if (n >= 260) return 0;                                                           \
    if (n) { if (p[n - 1] != (TYPE)'\\') p[n++] = (TYPE)'\\'; p[n] = 0; }             \
    return (uint32_t)(uintptr_t)(p + n);                                              \
}                                                                                     \
/* Returns the LAST CHARACTER of the path as it was on entry (not the NUL), and drops \
 * a trailing backslash only when the path is not a root. */                          \
uint32_t aret_PathRemoveBackslash##TAG(uint32_t esp) {                                \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                               \
    if (!p) return 0;                                                                 \
    int n = 0;                                                                        \
    while (p[n]) n++;                                                                 \
    TYPE *last = n ? p + n - 1 : p;                                                   \
    if (!u32_p_isroot_##TAG(p) && *last == (TYPE)'\\') *last = 0;                     \
    return (uint32_t)(uintptr_t)last;                                                 \
}                                                                                     \
/* Move the file-name component (same rule as PathFindFileName) to the front. */      \
uint32_t aret_PathStripPath##TAG(uint32_t esp) {                                      \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                               \
    if (!p) return 0;                                                                 \
    TYPE *last = p;                                                                   \
    for (TYPE *q = p; *q; q++)                                                        \
        if ((*q == (TYPE)'\\' || *q == (TYPE)'/' || *q == (TYPE)':') &&               \
            q[1] && q[1] != (TYPE)'\\' && q[1] != (TYPE)'/')                          \
            last = q + 1;                                                             \
    if (last != p) { int i = 0; do { p[i] = last[i]; } while (last[i++]); }           \
    return 0;                                                                         \
}                                                                                     \
/* Truncate at the last backslash that lies at or after the protected base; if there  \
 * is none, truncate to the base itself. TRUE iff anything was cut. */                \
uint32_t aret_PathRemoveFileSpec##TAG(uint32_t esp) {                                 \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                               \
    if (!p) return 0;                                                                 \
    int b = u32_p_base_##TAG(p), cut = b, i;                                          \
    for (i = b; p[i]; i++)                                                            \
        if (p[i] == (TYPE)'\\') cut = i;                                              \
    if (!p[cut]) return 0;                                                            \
    p[cut] = 0;                                                                       \
    return 1;                                                                         \
}

ARET_PATH_ROOT_FAMILY(A, char)
ARET_PATH_ROOT_FAMILY(W, uint16_t)
#undef ARET_PATH_ROOT_FAMILY

/* StrFromTimeIntervalW/A [SHLWAPI] — ported from Wine's dlls/shlwapi/string.c.
 *
 * MEDIUM form of Wine reuse (doc 82): a whole function BODY with real algorithm,
 * not a data table. Formats a millisecond duration as " H hr M min S sec", writing
 * `iDigits` significant digits of the first non-zero class (hours/minutes/seconds)
 * and zeroing — not rounding — the rest; if digits remain, the next class follows.
 * Faithful transcription of the SHLWAPI_WriteReverseNum / _FormatSignificant /
 * _WriteTimeClass chain. The class unit strings are Wine's shlwapi resources
 * IDS_TIME_INTERVAL_{HOURS,MINUTES,SECONDS} = " hr"/" min"/" sec" (leading space —
 * "always writing a leading space before the time interval begins"). Verified
 * bit-identical Wine by winecorpus/str_time_interval. */

/* Write a decimal number backwards from `out` (which starts at the NUL slot); returns
 * a pointer to the slot BEFORE the first digit (mirrors SHLWAPI_WriteReverseNum). */
static uint16_t *u32_write_reverse_num(uint16_t *out, uint32_t num) {
    *out-- = 0;
    do {
        uint32_t d = num % 10;
        *out-- = (uint16_t)('0' + d);
        num = (num - d) / 10;
    } while (num > 0);
    return out;
}

/* Zero the non-significant digits, return the count of significant digits remaining
 * (mirrors SHLWAPI_FormatSignificant). */
static int u32_format_significant(uint16_t *num, int digits) {
    while (*num) {
        num++;
        if (--digits == 0) {
            while (*num) *num++ = '0';
            return 0;
        }
    }
    return digits;
}

/* Append " <digits> <unit>" for one time class onto `out`; returns remaining digits
 * (mirrors SHLWAPI_WriteTimeClass, with LoadStringW(unit) into buff+32). */
static int u32_write_time_class(uint16_t *out, uint32_t val, const char *unit, int digits) {
    uint16_t buff[64], *o = buff + 32;
    o = u32_write_reverse_num(o, val);
    digits = u32_format_significant(o + 1, digits);
    *o = ' ';
    uint16_t *u = buff + 32;                 /* right after the last digit (the old NUL) */
    while (*unit) *u++ = (uint16_t)(unsigned char)*unit++;
    *u = 0;
    uint16_t *e = out; while (*e) e++;        /* lstrcatW(out, o) */
    while (*o) *e++ = *o++;
    *e = 0;
    return digits;
}

static int u32_strfromtime_w(uint16_t *out, uint32_t cchMax, uint32_t dwMS, int iDigits) {
    int iRet = 0;
    if (out && cchMax) {
        uint16_t copy[128];
        uint32_t hrs, mins;
        if (!iDigits || cchMax == 1) { *out = 0; return 0; }
        dwMS = (dwMS + 500) / 1000;
        hrs = dwMS / 3600; dwMS -= hrs * 3600;
        mins = dwMS / 60;  dwMS -= mins * 60;
        copy[0] = 0;
        if (hrs)             iDigits = u32_write_time_class(copy, hrs,  " hr",  iDigits);
        if (mins && iDigits) iDigits = u32_write_time_class(copy, mins, " min", iDigits);
        if (iDigits)                   u32_write_time_class(copy, dwMS, " sec", iDigits);
        uint32_t i = 0;                           /* lstrcpynW(out, copy, cchMax) */
        for (; copy[i] && i + 1 < cchMax; i++) out[i] = copy[i];
        out[i] = 0;
        iRet = (int)i;                            /* lstrlenW(out) */
    }
    return iRet;
}

uint32_t aret_StrFromTimeIntervalW(uint32_t esp) {
    return (uint32_t)u32_strfromtime_w((uint16_t *)WP(0), WU(1), WU(2), WI(3));
}

/* Wine QUIRK preserved: StrFromTimeIntervalA never updates iRet, so it ALWAYS
 * returns 0 (it delegates to the W form, then narrows via WideCharToMultiByte). */
uint32_t aret_StrFromTimeIntervalA(uint32_t esp) {
    char *out = WS(0);
    uint32_t cchMax = WU(1);
    if (out && cchMax) {
        uint16_t buff[128];
        u32_strfromtime_w(buff, 128, WU(2), WI(3));
        /* WideCharToMultiByte(CP_ACP,0,buff,-1,out,cchMax,0,0) of ASCII: copies the
         * terminating NUL when it fits; on OVERFLOW writes exactly cchMax bytes and
         * does NOT force a NUL (measured against Wine — the tail stays uninitialised). */
        for (uint32_t i = 0; i < cchMax; i++) {
            out[i] = (char)buff[i];
            if (buff[i] == 0) break;
        }
    }
    return 0;
}

/* shlwapi path family, wave 3: EXTENSIONS, COMPONENTS, ROOT COMPARISON.
 *
 * Eleven functions taken as a BLOCK rather than one runtime wall at a time — the
 * measurement technique from waves 1-2 is established, so a whole family costs one
 * grid (`winecorpus/win32_pathparts.c`). Everything below reproduces a measured row.
 *
 * All the extension operations pivot on `PathFindExtension` (already implemented
 * above): scanning forward, a `\` or a SPACE resets the candidate and a `.` records
 * it. The space is the surprising half and it drives three answers — "x.exe arg1
 * arg2" has NO extension, so PathAddExtension appends to the whole command line.
 * Also measured: a LEADING dot counts (".hidden" already has an extension, so
 * PathAddExtension refuses it) and so does a TRAILING one ("file.").
 *
 * Two functions from this family are deliberately NOT here:
 *   - `PathIsUNCServer`: Wine's A entry point answers FALSE for every input while
 *     its W entry point answers correctly. Both cannot be right and this host cannot
 *     decide which matches Windows, so a call aborts instead of returning a coin flip.
 *   - `PathCommonPrefix` / `PathIsPrefix`: 11 measured pairs did not separate "the
 *     length includes the separator" from "the length is the root length". Not
 *     shipped on a rule the grid does not prove. */
#define ARET_PATH_PARTS_FAMILY(TAG, TYPE)                                              \
static const TYPE *u32_p_ext_##TAG(const TYPE *p) {                                    \
    const TYPE *last = 0;                                                              \
    for (; *p; p++) {                                                                  \
        if (*p == (TYPE)'\\' || *p == (TYPE)' ') last = 0;                             \
        else if (*p == (TYPE)'.') last = p;                                            \
    }                                                                                  \
    return last ? last : p;                                                            \
}                                                                                      \
/* First character of the next component: skip to a separator, then skip the run of   \
 * them — which is why "\\\\srv" answers 2 and not 1. NULL for an empty path. */         \
uint32_t aret_PathFindNextComponent##TAG(uint32_t esp) {                               \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p || !*p) return 0;                                                           \
    while (*p && *p != (TYPE)'\\' && *p != (TYPE)'/') p++;                             \
    while (*p == (TYPE)'\\' || *p == (TYPE)'/') p++;                                   \
    return (uint32_t)(uintptr_t)p;                                                     \
}                                                                                      \
/* Arguments start after the first space OUTSIDE quotes; the terminating NUL if none. \
 * The quote tracking is what makes "\"a b\" c" answer 6 rather than 2. */             \
uint32_t aret_PathGetArgs##TAG(uint32_t esp) {                                         \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p) return 0;                                                                  \
    int q = 0;                                                                         \
    for (; *p; p++) {                                                                  \
        if (*p == (TYPE)'"') q = !q;                                                   \
        else if (*p == (TYPE)' ' && !q) return (uint32_t)(uintptr_t)(p + 1);           \
    }                                                                                  \
    return (uint32_t)(uintptr_t)p;                                                     \
}                                                                                      \
uint32_t aret_PathGetDriveNumber##TAG(uint32_t esp) {                                  \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p || !*p || p[1] != (TYPE)':') return 0xFFFFFFFFu;                            \
    TYPE c = p[0];                                                                     \
    if (c >= (TYPE)'a' && c <= (TYPE)'z') return (uint32_t)(c - (TYPE)'a');            \
    if (c >= (TYPE)'A' && c <= (TYPE)'Z') return (uint32_t)(c - (TYPE)'A');            \
    return 0xFFFFFFFFu;                                                                \
}                                                                                      \
/* A bare file name: no separator and no colon. The empty string qualifies. */         \
uint32_t aret_PathIsFileSpec##TAG(uint32_t esp) {                                      \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p) return 0;                                                                  \
    for (; *p; p++)                                                                    \
        if (*p == (TYPE)'\\' || *p == (TYPE)'/' || *p == (TYPE)':') return 0;          \
    return 1;                                                                          \
}                                                                                      \
/* "\\\\server\\share" — a UNC prefix with EXACTLY one further separator. Measured: what  \
 * follows that separator is irrelevant, so "\\\\srv\\" qualifies and "\\\\srv\\sh\\" does not. */ \
uint32_t aret_PathIsUNCServerShare##TAG(uint32_t esp) {                                \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p || p[0] != (TYPE)'\\' || p[1] != (TYPE)'\\') return 0;                      \
    int seps = 0;                                                                      \
    for (p += 2; *p; p++) if (*p == (TYPE)'\\') seps++;                                \
    return seps == 1;                                                                  \
}                                                                                      \
uint32_t aret_PathAddExtension##TAG(uint32_t esp) {                                    \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                                \
    const TYPE *ext = (const TYPE *)(uintptr_t)WU(1);                                  \
    if (!p || !ext) return 0;                                                          \
    if (!*ext) return 1;                     /* empty extension: success, no change */ \
    if (*u32_p_ext_##TAG(p)) return 0;       /* already has one */                     \
    int n = 0; while (p[n]) n++;                                                       \
    int e = 0; while (ext[e]) e++;                                                     \
    if (n + e >= 260) return 0;                                                        \
    for (int i = 0; i <= e; i++) p[n + i] = ext[i];                                    \
    return 1;                                                                          \
}                                                                                      \
uint32_t aret_PathRemoveExtension##TAG(uint32_t esp) {                                 \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                                \
    if (p) { TYPE *e = (TYPE *)u32_p_ext_##TAG(p); *e = 0; }                           \
    return 0;                                                                          \
}                                                                                      \
uint32_t aret_PathRenameExtension##TAG(uint32_t esp) {                                 \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                                \
    const TYPE *ext = (const TYPE *)(uintptr_t)WU(1);                                  \
    if (!p || !ext) return 0;                                                          \
    TYPE *e = (TYPE *)u32_p_ext_##TAG(p);                                              \
    int keep = (int)(e - p), n = 0;                                                    \
    while (ext[n]) n++;                                                                \
    if (keep + n >= 260) return 0;                                                     \
    for (int i = 0; i <= n; i++) e[i] = ext[i];                                        \
    return 1;                                                                          \
}                                                                                      \
/* Strip to the root by repeatedly removing the file spec until the path IS a root —  \
 * literally Wine's loop, and it reuses the two wave-1 functions, so the three agree  \
 * by construction. FALSE (with the path emptied) when there is no root to reach. */  \
uint32_t aret_PathStripToRoot##TAG(uint32_t esp) {                                     \
    TYPE *p = (TYPE *)(uintptr_t)WU(0);                                                \
    if (!p) return 0;                                                                  \
    while (!u32_p_isroot_##TAG(p)) {                                                   \
        int b = u32_p_base_##TAG(p), cut = b, i;                                       \
        for (i = b; p[i]; i++) if (p[i] == (TYPE)'\\') cut = i;                        \
        if (!p[cut]) return 0;                                                         \
        p[cut] = 0;                                                                    \
    }                                                                                  \
    return 1;                                                                          \
}                                                                                      \
/* Same root = both HAVE a root and the roots match, case-insensitively (measured:    \
 * "C:\\a" and "c:\\b" are the same root). Compares only the root, so "C:\\dir" and      \
 * "C:\\dirx\\f" are the same. */                                                        \
/* "\\\\server" — a UNC prefix with NO further separator. Settled on a REAL Windows      \
 * runner, not on Wine: Wine's A entry point answers FALSE for every input while its   \
 * W entry point is correct, and Windows agrees with W (A ≡ W there). So Wine's A is a \
 * bug, and reproducing it — which a Wine-only gate would have pushed toward — would   \
 * have shipped a wrong answer to every caller. Both our entry points implement the    \
 * measured Windows rule. */                                                           \
uint32_t aret_PathIsUNCServer##TAG(uint32_t esp) {                                     \
    const TYPE *p = (const TYPE *)(uintptr_t)WU(0);                                    \
    if (!p || p[0] != (TYPE)'\\' || p[1] != (TYPE)'\\') return 0;                      \
    for (p += 2; *p; p++) if (*p == (TYPE)'\\') return 0;                              \
    return 1;                                                                          \
}                                                                                      \
/* Length of the common prefix, cut at a component boundary. The rule fits all eleven  \
 * Windows tie-breaker pairs and NONE of it is derivable from the Wine grid, which is  \
 * why this was left unimplemented until the runner answered:                          \
 *   - identical strings answer their FULL length ("C:\\a"/"C:\\a" -> 4, not 3)          \
 *   - otherwise the answer is the index of the last separator inside the common part  \
 *     ("C:\\a\\b"/"C:\\a\\c" -> 4, the separator itself excluded)                         \
 *   - EXCEPT when that separator is the drive root's, which is kept                   \
 *     ("C:\\aa\\b"/"C:\\ab\\b" -> 3, i.e. "C:\\", not 2)                                   \
 *   - a UNC gets no such exception ("\\\\s\\h"/"\\\\s\\i" -> 3 = "\\\\s")                       \
 * A NULL output buffer is allowed and only the length is returned. */                 \
static int u32_p_common_##TAG(const TYPE *a, const TYPE *b) {                          \
    if (!a || !b) return 0;                                                            \
    int n = 0;                                                                         \
    while (a[n] && b[n]) {                                                             \
        TYPE x = a[n], y = b[n];                                                       \
        if (x >= (TYPE)'A' && x <= (TYPE)'Z') x = (TYPE)(x - 'A' + 'a');               \
        if (y >= (TYPE)'A' && y <= (TYPE)'Z') y = (TYPE)(y - 'A' + 'a');               \
        if (x != y) break;                                                             \
        n++;                                                                           \
    }                                                                                  \
    if (!a[n] && !b[n]) return n;               /* identical: the whole length */      \
    int s = -1;                                                                        \
    for (int k = 0; k < n; k++) if (a[k] == (TYPE)'\\') s = k;                         \
    if (s < 0) return 0;                                                               \
    if (s == 2 && a[1] == (TYPE)':') return 3;  /* the drive root keeps its separator */\
    return s;                                                                          \
}                                                                                      \
uint32_t aret_PathCommonPrefix##TAG(uint32_t esp) {                                    \
    const TYPE *a = (const TYPE *)(uintptr_t)WU(0);                                    \
    const TYPE *b = (const TYPE *)(uintptr_t)WU(1);                                    \
    TYPE *out = (TYPE *)(uintptr_t)WU(2);                                              \
    int n = u32_p_common_##TAG(a, b);                                                  \
    if (out) { for (int i = 0; i < n; i++) out[i] = a[i]; out[n] = 0; }                \
    return (uint32_t)n;                                                                \
}                                                                                      \
/* TRUE when the whole of `prefix` is the common prefix — so "C:\\" IS a prefix of       \
 * "C:\\a" but "C:\\a\\" is NOT a prefix of "C:\\a\\b" (the common part stops at 4, the      \
 * prefix is 5 long). Measured, both directions. */                                    \
uint32_t aret_PathIsPrefix##TAG(uint32_t esp) {                                        \
    const TYPE *pre = (const TYPE *)(uintptr_t)WU(0);                                  \
    const TYPE *path = (const TYPE *)(uintptr_t)WU(1);                                 \
    if (!pre || !path) return 0;                                                       \
    int n = 0; while (pre[n]) n++;                                                     \
    return u32_p_common_##TAG(pre, path) == n;                                         \
}                                                                                      \
uint32_t aret_PathIsSameRoot##TAG(uint32_t esp) {                                      \
    const TYPE *a = (const TYPE *)(uintptr_t)WU(0);                                    \
    const TYPE *b = (const TYPE *)(uintptr_t)WU(1);                                    \
    if (!a || !b) return 0;                                                            \
    const TYPE *ra = u32_p_skiproot_##TAG(a), *rb = u32_p_skiproot_##TAG(b);           \
    if (!ra || !rb) return 0;                                                          \
    int la = (int)(ra - a), lb = (int)(rb - b);                                        \
    if (la != lb) return 0;                                                            \
    for (int i = 0; i < la; i++) {                                                     \
        TYPE x = a[i], y = b[i];                                                       \
        if (x >= (TYPE)'A' && x <= (TYPE)'Z') x = (TYPE)(x - 'A' + 'a');               \
        if (y >= (TYPE)'A' && y <= (TYPE)'Z') y = (TYPE)(y - 'A' + 'a');               \
        if (x != y) return 0;                                                          \
    }                                                                                  \
    return 1;                                                                          \
}

ARET_PATH_PARTS_FAMILY(A, char)
ARET_PATH_PARTS_FAMILY(W, uint16_t)
#undef ARET_PATH_PARTS_FAMILY

/* shlwapi path family, wave 2: PathCanonicalize / PathCombine / PathAppend.
 *
 * One implementation, because they are one function in disguise: PathAppend defers
 * to PathCombine, which canonicalizes. All three are gated by one grid of 38 rows in
 * `winecorpus/win32_pathcombine.c`, and every rule below reproduces the MEASURED
 * answer — several of which no amount of reading the documentation would give:
 *
 *   "C:\\a\\.\\b"   -> "C:\\a\\b"    but "C:\\a\\."  -> "C:\\a\\."   (a TRAILING dot stays)
 *   "C:\\a\\...\\b" -> "C:.\\b"      ("..." is ".." then ".", not a component)
 *   "C:"          -> "C:\\"        (a naked drive spec gains a backslash at the END)
 *   "C:a\\..\\b"   -> "\\b"         (a drive with NO separator is not a root: it is LOST)
 *   "C:\\..\\a"    -> "C:\\a"       (but a drive WITH one is protected)
 *   "\\\\srv\\..'   -> "\\\\srv"      (climbing into a UNC server name is refused —
 *                                 only the trailing separator goes)
 *   "\\\\..\\a"     -> "\\\\a"        (same refusal at the bare "\\\\")
 *   ""            -> "\\"
 *   PathCombine("C:\\dir","\\file")  -> "C:\\file"   (dir's ROOT, not dir)
 *   PathCombine("C:\\dir","C:rel")   -> "C:rel"     (drive-relative counts as absolute)
 *   PathCombine over MAX_PATH        -> NULL, and the destination is set to ""
 *
 * The `..` rule that fits every row: back up to the previous separator; if there is
 * none, fall back to just inside the root when the root ends in a separator and to
 * the very start when it does not; and on a UNC path refuse any back-up that would
 * land at or before index 1, dropping only the trailing separator instead. */
#define ARET_PATH_COMBINE_FAMILY(TAG, TYPE)                                            \
static int u32_p_canon_##TAG(TYPE *out, const TYPE *src) {                             \
    if (!out || !src) return 0;                                                        \
    if (!*src) { out[0] = (TYPE)'\\'; out[1] = 0; return 1; }                          \
    /* Built in scratch, then copied out as exactly one string. Canonicalization       \
     * writes components and then backs over them, so working directly in the          \
     * caller's buffer would leave stale bytes PAST the terminating NUL — which is     \
     * visible, and measurably not what Wine leaves there (its A entry point converts  \
     * through a wide buffer and writes only the final result). Also makes an aliased  \
     * out == src safe. */                                                             \
    TYPE dst[2 * 260 + 4];                                                             \
    TYPE *d = dst;                                                                     \
    const TYPE *s = src;                                                               \
    if (*s == (TYPE)'\\') { *d++ = *s++; }                                             \
    else if (*s && s[1] == (TYPE)':') {                                                \
        *d++ = *s++; *d++ = *s++;                                                      \
        if (*s == (TYPE)'\\') *d++ = *s++;                                             \
    }                                                                                  \
    int rootlen = (int)(d - dst);                                                      \
    while (*s) {                                                                       \
        /* "\.\" or a leading ".\" — drop it. Only at a component start, which is why  \
         * the middle dot of "..." does not qualify. */                                \
        if (*s == (TYPE)'.' && s[1] == (TYPE)'\\' &&                                   \
            (s == src || s[-1] == (TYPE)'\\' || s[-1] == (TYPE)':')) { s += 2; continue; } \
        if (*s == (TYPE)'.' && s[1] == (TYPE)'.' && d > dst && d[-1] == (TYPE)'\\') {  \
            int n = (int)(d - dst), j = -1;                                            \
            for (int k = n - 2; k >= 0; k--)                                           \
                if (dst[k] == (TYPE)'\\') { j = k; break; }                            \
            if (j >= 0) {                                                              \
                int unc = (n > 1 && dst[0] == (TYPE)'\\' && dst[1] == (TYPE)'\\');     \
                d = dst + ((unc && j <= 1) ? n - 1 : j);                               \
            } else {                                                                   \
                d = dst + ((rootlen > 0 && dst[rootlen - 1] == (TYPE)'\\')             \
                           ? rootlen - 1 : 0);                                         \
            }                                                                          \
            s += 2;                                                                    \
            continue;                                                                  \
        }                                                                              \
        *d++ = *s++;                                                                   \
    }                                                                                  \
    if (d - dst == 2 && d[-1] == (TYPE)':') *d++ = (TYPE)'\\';                         \
    *d = 0;                                                                            \
    for (int i = 0; i <= (int)(d - dst); i++) out[i] = dst[i];                         \
    return 1;                                                                          \
}                                                                                      \
static TYPE *u32_p_combine_##TAG(TYPE *dst, const TYPE *dir, const TYPE *file) {       \
    if (!dst) return 0;                                                                \
    if (!dir && !file) { *dst = 0; return 0; }                                         \
    /* Built in a scratch buffer, never in place: PathAppend calls this with           \
     * dst == dir, so reading dir after writing dst would read what we just wrote. */  \
    TYPE tmp[2 * 260 + 4];                                                             \
    int t = 0;                                                                         \
    const TYPE *only = 0;                                                              \
    if (!file || !*file) only = dir;                    /* nothing to append */        \
    else if (!dir || !*dir) only = file;                /* nothing to append to */     \
    else if ((file[0] == (TYPE)'\\' && file[1] == (TYPE)'\\') || file[1] == (TYPE)':') \
        only = file;                                    /* UNC or drive: absolute */   \
    if (only) {                                                                        \
        for (; only[t] && t < 260; t++) tmp[t] = only[t];                              \
    } else if (file[0] == (TYPE)'\\') {                                                \
        /* Rooted but not absolute: dir's ROOT plus the tail, separator dropped. */    \
        const TYPE *r = u32_p_skiproot_##TAG(dir);                                     \
        int rl = r ? (int)(r - dir) : 0;                                               \
        for (int i = 0; i < rl && t < 260; i++) tmp[t++] = dir[i];                     \
        for (const TYPE *f = file + 1; *f && t < 2 * 260; f++) tmp[t++] = *f;          \
    } else {                                                                           \
        for (int i = 0; dir[i] && t < 2 * 260; i++) tmp[t++] = dir[i];                 \
        if (t && tmp[t - 1] != (TYPE)'\\' && t < 2 * 260) tmp[t++] = (TYPE)'\\';       \
        for (int i = 0; file[i] && t < 2 * 260; i++) tmp[t++] = file[i];               \
    }                                                                                  \
    tmp[t] = 0;                                                                        \
    if (t >= 260) { *dst = 0; return 0; }  /* over MAX_PATH: measured NULL + "" */     \
    u32_p_canon_##TAG(dst, tmp);                                                       \
    return dst;                                                                        \
}                                                                                      \
uint32_t aret_PathCanonicalize##TAG(uint32_t esp) {                                    \
    return (uint32_t)u32_p_canon_##TAG((TYPE *)(uintptr_t)WU(0),                       \
                                       (const TYPE *)(uintptr_t)WU(1));                \
}                                                                                      \
uint32_t aret_PathCombine##TAG(uint32_t esp) {                                         \
    return (uint32_t)(uintptr_t)u32_p_combine_##TAG((TYPE *)(uintptr_t)WU(0),          \
                                                    (const TYPE *)(uintptr_t)WU(1),    \
                                                    (const TYPE *)(uintptr_t)WU(2));   \
}                                                                                      \
uint32_t aret_PathAppend##TAG(uint32_t esp) {                                          \
    TYPE *path = (TYPE *)(uintptr_t)WU(0);                                             \
    const TYPE *more = (const TYPE *)(uintptr_t)WU(1);                                 \
    if (!path || !more) return 0;                                                      \
    /* Leading separators are stripped — UNLESS the tail is itself UNC, which is what  \
     * makes PathAppend("C:\\dir","\\\\srv\\sh") answer "\\\\srv\\sh" rather than "C:\\dir\\srv\\sh". */ \
    if (!(more[0] == (TYPE)'\\' && more[1] == (TYPE)'\\'))                             \
        while (*more == (TYPE)'\\') more++;                                            \
    return u32_p_combine_##TAG(path, path, more) ? 1 : 0;                              \
}

ARET_PATH_COMBINE_FAMILY(A, char)
ARET_PATH_COMBINE_FAMILY(W, uint16_t)
#undef ARET_PATH_COMBINE_FAMILY

/* shlwapi `Str*` — wave 1: the SEARCH/SCAN group (StrChr/StrRChr/StrStr/StrRStrI/
 * StrSpn/StrCSpn/StrPBrk and their case-insensitive and counted variants).
 *
 * Shimmed rather than lifted, and that is a measured decision, not a preference:
 * shlwapi is a RELAY (198 `__wine_spec_imp_` thunks + 217 PE forwarders out of 362
 * exports, doc 70 §5.0), so lifting it moves the wall one module deeper instead of
 * removing it. A pure lexical family belongs in the HLE.
 *
 * These look like the C library and are NOT the C library. Every line below is a
 * measured contrast that the obvious implementation gets wrong:
 *   - An EMPTY needle returns NULL — `strstr` returns the haystack. True for
 *     StrStr/StrStrI/StrStrN/StrStrNI/StrRStrI and for StrPBrk with an empty set.
 *   - `StrCSpn(s, "")` is the FULL length (nothing in the set was found) while
 *     `StrSpn(s, "")` is 0. Symmetric-looking, opposite answers.
 *   - The `N` variants bound where a match may START, not where it may end:
 *     `StrStrNW("Hello, World!…", "World", 8)` finds the match at index 7 even
 *     though it runs to 11. Swept n = 0..22 to pin that down rather than reasoned.
 *   - NULL arguments are SAFE on every one of them (NULL / 0), not a fault.
 *   - ⚠️ `StrChrW(s, 0)` returns the TERMINATOR while `StrChrA(s,'\0')`,
 *     `StrChrIW(s,0)` and `StrRChrW(s,NULL,0)` all return NULL. One function out of
 *     four disagrees with its own siblings; that asymmetry is measured, reproduced
 *     verbatim, and QUEUED FOR THE WINDOWS ORACLE (bench/winoracle) because it looks
 *     far more like a Wine slip than a contract — same shape as PathIsUNCServerA.
 *     Do not "clean it up" without a Windows measurement saying so.
 * Case folding is ASCII, exactly as elsewhere in the HLE (ordinal, locale C);
 * non-ASCII case rules are a different subject and are not claimed here. */
#define ARET_STR_SEARCH_FAMILY(TAG, TYPE)                                              \
static TYPE u32_s_low_##TAG(TYPE c) {                                                  \
    return (c >= (TYPE)'A' && c <= (TYPE)'Z') ? (TYPE)(c + 32) : c;                    \
}                                                                                      \
/* Length of the run of leading characters that ARE in `set`. */                       \
uint32_t aret_StrSpn##TAG(uint32_t esp) {                                              \
    const TYPE *s = (const TYPE *)(uintptr_t)WU(0), *set = (const TYPE *)(uintptr_t)WU(1); \
    if (!s || !set) return 0;                                                          \
    uint32_t n = 0;                                                                    \
    for (; s[n]; n++) {                                                                \
        const TYPE *m = set;                                                           \
        while (*m && *m != s[n]) m++;                                                  \
        if (!*m) break;                                                                \
    }                                                                                  \
    return n;                                                                          \
}                                                                                      \
/* Length of the run of leading characters NOT in `set`; an empty set therefore       \
 * answers the whole length, where StrSpn answers 0. */                                \
static uint32_t u32_s_cspn_##TAG(const TYPE *s, const TYPE *set, int fold) {           \
    if (!s || !set) return 0;                                                          \
    uint32_t n = 0;                                                                    \
    for (; s[n]; n++) {                                                                \
        TYPE c = fold ? u32_s_low_##TAG(s[n]) : s[n];                                  \
        const TYPE *m = set;                                                           \
        while (*m && (fold ? u32_s_low_##TAG(*m) : *m) != c) m++;                      \
        if (*m) break;                                                                 \
    }                                                                                  \
    return n;                                                                          \
}                                                                                      \
uint32_t aret_StrCSpn##TAG(uint32_t esp)                                               \
    { return u32_s_cspn_##TAG((const TYPE *)(uintptr_t)WU(0), (const TYPE *)(uintptr_t)WU(1), 0); } \
uint32_t aret_StrCSpnI##TAG(uint32_t esp)                                              \
    { return u32_s_cspn_##TAG((const TYPE *)(uintptr_t)WU(0), (const TYPE *)(uintptr_t)WU(1), 1); } \
/* First character of `s` that appears in `set`; NULL when none — and an EMPTY set    \
 * finds nothing, so it yields NULL rather than `s`. */                                \
uint32_t aret_StrPBrk##TAG(uint32_t esp) {                                             \
    const TYPE *s = (const TYPE *)(uintptr_t)WU(0), *set = (const TYPE *)(uintptr_t)WU(1); \
    if (!s || !set) return 0;                                                          \
    for (; *s; s++) {                                                                  \
        const TYPE *m = set;                                                           \
        while (*m && *m != *s) m++;                                                    \
        if (*m) return (uint32_t)(uintptr_t)s;                                         \
    }                                                                                  \
    return 0;                                                                          \
}                                                                                      \
static const TYPE *u32_s_chr_##TAG(const TYPE *s, TYPE c, int fold, uint32_t maxn) {   \
    if (!s) return 0;                                                                  \
    if (fold) c = u32_s_low_##TAG(c);                                                  \
    for (uint32_t i = 0; s[i] && i < maxn; i++)                                        \
        if ((fold ? u32_s_low_##TAG(s[i]) : s[i]) == c) return s + i;                  \
    return 0;                                                                          \
}                                                                                      \
uint32_t aret_StrChrI##TAG(uint32_t esp)                                               \
    { return (uint32_t)(uintptr_t)u32_s_chr_##TAG((const TYPE *)(uintptr_t)WU(0), (TYPE)WU(1), 1, ~0u); } \
/* Last occurrence before `end` (NULL end = the whole string). */                      \
static const TYPE *u32_s_rchr_##TAG(const TYPE *s, const TYPE *end, TYPE c, int fold) { \
    if (!s) return 0;                                                                  \
    if (fold) c = u32_s_low_##TAG(c);                                                  \
    const TYPE *hit = 0;                                                               \
    for (const TYPE *p = s; *p && (!end || p < end); p++)                              \
        if ((fold ? u32_s_low_##TAG(*p) : *p) == c) hit = p;                           \
    return hit;                                                                        \
}                                                                                      \
uint32_t aret_StrRChr##TAG(uint32_t esp) {                                             \
    return (uint32_t)(uintptr_t)u32_s_rchr_##TAG((const TYPE *)(uintptr_t)WU(0),        \
        (const TYPE *)(uintptr_t)WU(1), (TYPE)WU(2), 0);                               \
}                                                                                      \
uint32_t aret_StrRChrI##TAG(uint32_t esp) {                                            \
    return (uint32_t)(uintptr_t)u32_s_rchr_##TAG((const TYPE *)(uintptr_t)WU(0),        \
        (const TYPE *)(uintptr_t)WU(1), (TYPE)WU(2), 1);                               \
}                                                                                      \
/* Substring search. `maxstart` is the number of positions allowed as a match START;  \
 * the match itself may run past it (measured by sweeping n). An empty needle finds   \
 * nothing, which is where this parts company with strstr. */                          \
static const TYPE *u32_s_str_##TAG(const TYPE *h, const TYPE *n, int fold, uint32_t maxstart) { \
    if (!h || !n || !*n) return 0;                                                     \
    for (uint32_t i = 0; h[i] && i < maxstart; i++) {                                  \
        uint32_t k = 0;                                                                \
        while (n[k] && h[i + k] &&                                                     \
               (fold ? u32_s_low_##TAG(h[i + k]) == u32_s_low_##TAG(n[k]) : h[i + k] == n[k])) k++; \
        if (!n[k]) return h + i;                                                       \
    }                                                                                  \
    return 0;                                                                          \
}                                                                                      \
uint32_t aret_StrStr##TAG(uint32_t esp)                                                \
    { return (uint32_t)(uintptr_t)u32_s_str_##TAG((const TYPE *)(uintptr_t)WU(0), (const TYPE *)(uintptr_t)WU(1), 0, ~0u); } \
uint32_t aret_StrStrI##TAG(uint32_t esp)                                               \
    { return (uint32_t)(uintptr_t)u32_s_str_##TAG((const TYPE *)(uintptr_t)WU(0), (const TYPE *)(uintptr_t)WU(1), 1, ~0u); } \
/* Last occurrence, case-insensitive, before `end` (NULL end = whole string). */       \
uint32_t aret_StrRStrI##TAG(uint32_t esp) {                                            \
    const TYPE *h = (const TYPE *)(uintptr_t)WU(0);                                    \
    const TYPE *end = (const TYPE *)(uintptr_t)WU(1);                                  \
    const TYPE *nd = (const TYPE *)(uintptr_t)WU(2);                                   \
    if (!h || !nd || !*nd) return 0;                                                   \
    const TYPE *hit = 0;                                                               \
    for (const TYPE *p = h; *p && (!end || p < end); p++) {                            \
        uint32_t k = 0;                                                                \
        while (nd[k] && p[k] && u32_s_low_##TAG(p[k]) == u32_s_low_##TAG(nd[k])) k++;  \
        if (!nd[k]) hit = p;                                                           \
    }                                                                                  \
    return (uint32_t)(uintptr_t)hit;                                                   \
}

ARET_STR_SEARCH_FAMILY(A, char)
ARET_STR_SEARCH_FAMILY(W, uint16_t)
#undef ARET_STR_SEARCH_FAMILY

/* StrChrA/W and the two counted W variants, written out rather than generated,
 * because A and W genuinely DISAGREE on the terminator (see the family comment) and
 * a macro would have quietly imposed one answer on both. */
uint32_t aret_StrChrA(uint32_t esp) {
    const char *s = (const char *)(uintptr_t)WU(0);
    char c = (char)WU(1);
    if (!s) return 0;
    for (; *s; s++) if (*s == c) return (uint32_t)(uintptr_t)s;
    return 0;                                   /* NUL is NOT matched: measured */
}
uint32_t aret_StrChrW(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(0);
    uint16_t c = (uint16_t)WU(1);
    if (!s) return 0;
    for (;; s++) {                              /* terminator INCLUDED: measured */
        if (*s == c) return (uint32_t)(uintptr_t)s;
        if (!*s) return 0;
    }
}
uint32_t aret_StrChrNW(uint32_t esp) {
    const uint16_t *s = (const uint16_t *)(uintptr_t)WU(0);
    uint16_t c = (uint16_t)WU(1);
    uint32_t n = WU(2);
    if (!s) return 0;
    for (uint32_t i = 0; s[i] && i < n; i++) if (s[i] == c) return (uint32_t)(uintptr_t)(s + i);
    return 0;
}
uint32_t aret_StrStrNW(uint32_t esp) {
    const uint16_t *h = (const uint16_t *)(uintptr_t)WU(0);
    const uint16_t *n = (const uint16_t *)(uintptr_t)WU(1);
    uint32_t maxstart = WU(2);
    if (!h || !n || !*n) return 0;
    for (uint32_t i = 0; h[i] && i < maxstart; i++) {
        uint32_t k = 0;
        while (n[k] && h[i + k] && h[i + k] == n[k]) k++;
        if (!n[k]) return (uint32_t)(uintptr_t)(h + i);
    }
    return 0;
}
/* shlwapi `Str*` — wave 2: COPY / CONCATENATE / TRIM / DUPLICATE.
 *
 * The counted variants all take the size of the WHOLE destination, NUL included —
 * not a number of characters to move. That one convention is the difference between
 * a correct shim and an off-by-one overflow, and it is measured, not assumed:
 *   - `StrCpyN(dst, "hello", 0)` writes **nothing at all** — not even a NUL. n=1
 *     writes only the NUL. Visible solely on a poisoned buffer.
 *   - `StrNCat(dst, "CDEFG", n)` appends exactly n-1 characters (n=2 appends one).
 *   - `StrCatBuff(dst, src, cch)` treats cch as the total buffer, so a cch that only
 *     covers what is already there appends nothing and leaves the string as it was.
 * Other measured facts that would have been guessed wrong:
 *   - `StrDup(NULL)` returns a valid EMPTY string, not NULL. So does `StrDup("")`.
 *     The block comes from the process heap because the caller frees it with
 *     `LocalFree`, which is `free()` here — the pair has to agree or every StrDup
 *     leaks or corrupts.
 *   - `StrTrim` trims BOTH ends in place and returns whether it changed anything;
 *     a NULL or empty trim set changes nothing and returns FALSE (not TRUE).
 *   - `StrCatChainW` writes at `ichAt` LITERALLY — it does not scan for the end, so
 *     a gap before it keeps whatever was there (proven: indices 1..4 kept the
 *     poison). `ichAt = -1` means "at the current length", cchDst = 0 writes
 *     nothing and answers 0, and the answer is always the new end index.
 * NOT modelled, deliberately: a NULL SOURCE to `StrNCat`/`StrCpyN`. Wine faults on
 * it (measured), so it is a caller bug rather than a contract, and a shim that
 * invented "does nothing" would be kinder than Windows — which is its own kind of
 * divergence. `StrCpyNX*` is also left out: it is undocumented and absent from the
 * mingw import library, so no fixture could bind it without a `.def`. */
#define ARET_STR_COPY_FAMILY(TAG, TYPE)                                                \
static uint32_t u32_s_len_##TAG(const TYPE *s) {                                       \
    uint32_t n = 0; while (s[n]) n++; return n;                                        \
}                                                                                      \
/* Append, bounded by the TOTAL destination size (NUL included). */                     \
uint32_t aret_StrCatBuff##TAG(uint32_t esp) {                                          \
    TYPE *dst = (TYPE *)(uintptr_t)WU(0);                                              \
    const TYPE *src = (const TYPE *)(uintptr_t)WU(1);                                  \
    uint32_t cch = WU(2);                                                              \
    if (!dst || !src) return (uint32_t)(uintptr_t)dst;                                 \
    uint32_t have = u32_s_len_##TAG(dst);                                              \
    if (cch > have + 1) {                                                              \
        uint32_t room = cch - have - 1, i = 0;                                         \
        for (; i < room && src[i]; i++) dst[have + i] = src[i];                         \
        dst[have + i] = 0;                                                             \
    }                                                                                  \
    return (uint32_t)(uintptr_t)dst;                                                   \
}                                                                                      \
/* Append at most cchMax-1 characters (cchMax counts the NUL). */                       \
uint32_t aret_StrNCat##TAG(uint32_t esp) {                                             \
    TYPE *dst = (TYPE *)(uintptr_t)WU(0);                                              \
    const TYPE *src = (const TYPE *)(uintptr_t)WU(1);                                  \
    uint32_t cch = WU(2);                                                              \
    if (!dst || !cch) return (uint32_t)(uintptr_t)dst;                                 \
    uint32_t have = u32_s_len_##TAG(dst), i = 0;                                       \
    for (; i + 1 < cch && src[i]; i++) dst[have + i] = src[i];                          \
    dst[have + i] = 0;                                                                 \
    return (uint32_t)(uintptr_t)dst;                                                   \
}                                                                                      \
/* Trim both ends in place; TRUE only if something actually moved. */                   \
uint32_t aret_StrTrim##TAG(uint32_t esp) {                                             \
    TYPE *s = (TYPE *)(uintptr_t)WU(0);                                                \
    const TYPE *set = (const TYPE *)(uintptr_t)WU(1);                                  \
    if (!s || !set || !*set) return 0;                                                 \
    uint32_t n = u32_s_len_##TAG(s), lo = 0, hi = n;                                   \
    while (lo < hi) { const TYPE *m = set; while (*m && *m != s[lo]) m++; if (!*m) break; lo++; } \
    while (hi > lo) { const TYPE *m = set; while (*m && *m != s[hi - 1]) m++; if (!*m) break; hi--; } \
    if (lo == 0 && hi == n) return 0;                                                   \
    uint32_t k = 0;                                                                    \
    for (; lo < hi; lo++, k++) s[k] = s[lo];                                           \
    s[k] = 0;                                                                          \
    return 1;                                                                          \
}                                                                                      \
/* Heap copy the caller frees with LocalFree (== free() here). NULL duplicates to    \
 * an EMPTY string, not to NULL — measured. */                                         \
uint32_t aret_StrDup##TAG(uint32_t esp) {                                              \
    const TYPE *s = (const TYPE *)(uintptr_t)WU(0);                                    \
    uint32_t n = s ? u32_s_len_##TAG(s) : 0;                                           \
    TYPE *p = (TYPE *)malloc((size_t)(n + 1) * sizeof(TYPE));                          \
    if (!p) return 0;                                                                  \
    for (uint32_t i = 0; i < n; i++) p[i] = s[i];                                      \
    p[n] = 0;                                                                          \
    return (uint32_t)(uintptr_t)p;                                                     \
}

ARET_STR_COPY_FAMILY(A, char)
ARET_STR_COPY_FAMILY(W, uint16_t)
#undef ARET_STR_COPY_FAMILY

/* W-only: shlwapi exports no StrCat/StrCpy/StrCpyN for ANSI (lstrcat/lstrcpy
 * already cover it), so these are not part of the macro above. */
uint32_t aret_StrCatW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(0);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(1);
    if (!dst || !src) return (uint32_t)(uintptr_t)dst;
    uint32_t n = 0; while (dst[n]) n++;
    uint32_t i = 0; for (; src[i]; i++) dst[n + i] = src[i];
    dst[n + i] = 0;
    return (uint32_t)(uintptr_t)dst;
}
uint32_t aret_StrCpyW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(0);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(1);
    if (!dst || !src) return (uint32_t)(uintptr_t)dst;
    uint32_t i = 0; for (; src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return (uint32_t)(uintptr_t)dst;
}
uint32_t aret_StrCpyNW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(0);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(1);
    uint32_t cch = WU(2);
    if (!dst || !cch) return (uint32_t)(uintptr_t)dst;   /* cch 0 writes NOTHING */
    uint32_t i = 0;
    for (; i + 1 < cch && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return (uint32_t)(uintptr_t)dst;
}
/* StrCatChainW(dst, cchDst, ichAt, src) -> the new end index. `ichAt` is a literal
 * write position (-1 = the current length); anything before it is left alone. */
uint32_t aret_StrCatChainW(uint32_t esp) {
    uint16_t *dst = (uint16_t *)(uintptr_t)WU(0);
    uint32_t cch = WU(1), at = WU(2);
    const uint16_t *src = (const uint16_t *)(uintptr_t)WU(3);
    if (!dst || !cch) return 0;
    if (at == 0xFFFFFFFFu) { at = 0; while (at < cch && dst[at]) at++; }
    if (at >= cch) return at;
    if (src) {
        uint32_t i = 0;
        while (at + i + 1 < cch && src[i]) { dst[at + i] = src[i]; i++; }
        at += i;
    }
    dst[at] = 0;
    return at;
}

/* Window station / desktop (user32) — the "which desktop am I on" family every
 * framework asks at startup (MFC/WinMerge's wall after the Str* family).
 *
 * Two process-wide singleton handles, distinct from each other, in their own range.
 * A desktop is not something ARET can meaningfully have several of headless, and
 * inventing a second one would be inventing state: the honest model is exactly one
 * of each, named as Windows names them ("Default" on "WinSta0"), which is also what
 * Wine reports. `GetThreadDesktop` succeeds for a thread id THIS process actually
 * has — the main thread (`GetCurrentThreadId`) or a live fiber (`0x1000 + index`,
 * what `CreateThread` hands back) — and answers NULL + ERROR_INVALID_PARAMETER for
 * anything else, rather than a handle for a thread that does not exist.
 *
 * `GetUserObjectInformation`, measured on a POISONED buffer, and every line of this
 * is a contrast the obvious implementation gets wrong:
 *   - UOI_FLAGS writes a 12-byte USEROBJECTFLAGS but only the THIRD dword:
 *     `fInherit` and `fReserved` are left **untouched**. A shim that zero-filled the
 *     struct would look correct in any test that reads only dwFlags.
 *   - UOI_TYPE lengths differ per object ("Desktop" 8, "WindowStation" 14), so the
 *     required size is not a constant.
 *   - Indices 4+ (UOI_USER_SID and beyond) fail with ERROR_INVALID_PARAMETER and
 *     leave `*lpnLengthNeeded` at **0**, where a too-small buffer sets it.
 *   - A bad handle is ERROR_INVALID_HANDLE (6), not INVALID_PARAMETER (87).
 *   - ⚠️ On the A path the FAILURE branch reports the WIDE byte count (16 for
 *     "Default") while the SUCCESS branch reports the narrow one (8). Reproduced
 *     because Wine is the gate, and QUEUED FOR THE WINDOWS ORACLE: reporting a
 *     different size depending on whether you succeeded is far more like Wine's A
 *     wrapper leaking its W call than a contract. */
#define U32_DESKTOP_H  0x76000001u        /* the one desktop */
#define U32_WINSTA_H   0x76000002u        /* the one window station */
uint32_t aret_GetThreadDesktop(uint32_t esp) {
    uint32_t tid = WU(0);
    if (tid == (uint32_t)getpid()) return U32_DESKTOP_H;      /* the main thread */
    if (tid >= 0x1000u && tid < 0x1000u + (uint32_t)g_nfiber) return U32_DESKTOP_H;
    g_last_error = 87u;                                       /* ERROR_INVALID_PARAMETER */
    return 0;
}
uint32_t aret_GetProcessWindowStation(uint32_t esp) { (void)esp; return U32_WINSTA_H; }
static uint32_t u32_userobj_info(uint32_t esp, int wide) {
    uint32_t h = WU(0), index = WU(1), pv = WU(2), len = WU(3), pneed = WU(4);
    if (h != U32_DESKTOP_H && h != U32_WINSTA_H) {
        if (pneed) *(uint32_t *)(uintptr_t)pneed = 0;
        g_last_error = 6u;                                    /* ERROR_INVALID_HANDLE */
        return 0;
    }
    if (index == 1) {                                         /* UOI_FLAGS */
        if (pneed) *(uint32_t *)(uintptr_t)pneed = 12;
        if (!pv || len < 12) { g_last_error = 122u; return 0; }
        /* Only dwFlags is written; fInherit/fReserved stay as the caller left
         * them — measured, and the reason this is not a memset. */
        ((uint32_t *)(uintptr_t)pv)[2] = (h == U32_WINSTA_H) ? 1u : 0u; /* WSF_VISIBLE */
        return 1;
    }
    const char *s;
    if (index == 2) s = (h == U32_WINSTA_H) ? "WinSta0" : "Default";        /* UOI_NAME */
    else if (index == 3) s = (h == U32_WINSTA_H) ? "WindowStation" : "Desktop"; /* UOI_TYPE */
    else {
        if (pneed) *(uint32_t *)(uintptr_t)pneed = 0;
        g_last_error = 87u;
        return 0;
    }
    uint32_t chars = (uint32_t)strlen(s) + 1;
    uint32_t bytes = wide ? chars * 2 : chars;
    if (!pv || len < bytes) {
        /* The failure branch reports the WIDE size even for A — measured, queued
         * for the Windows oracle. The buffer is left untouched. */
        if (pneed) *(uint32_t *)(uintptr_t)pneed = chars * 2;
        g_last_error = 122u;                                  /* INSUFFICIENT_BUFFER */
        return 0;
    }
    if (pneed) *(uint32_t *)(uintptr_t)pneed = bytes;
    if (wide) { uint16_t *o = (uint16_t *)(uintptr_t)pv; for (uint32_t i = 0; i < chars; i++) o[i] = (uint8_t)s[i]; }
    else memcpy((void *)(uintptr_t)pv, s, chars);
    return 1;
}
uint32_t aret_GetUserObjectInformationA(uint32_t esp) { return u32_userobj_info(esp, 0); }
uint32_t aret_GetUserObjectInformationW(uint32_t esp) { return u32_userobj_info(esp, 1); }

uint32_t aret_StrStrNIW(uint32_t esp) {
    const uint16_t *h = (const uint16_t *)(uintptr_t)WU(0);
    const uint16_t *n = (const uint16_t *)(uintptr_t)WU(1);
    uint32_t maxstart = WU(2);
    if (!h || !n || !*n) return 0;
    for (uint32_t i = 0; h[i] && i < maxstart; i++) {
        uint32_t k = 0;
        while (n[k] && h[i + k] &&
               ((h[i+k] >= 'A' && h[i+k] <= 'Z' ? h[i+k] + 32 : h[i+k]) ==
                (n[k]   >= 'A' && n[k]   <= 'Z' ? n[k]   + 32 : n[k]))) k++;
        if (!n[k]) return (uint32_t)(uintptr_t)(h + i);
    }
    return 0;
}

/* GetUserNameA/W(buf, pcbBuffer) -> BOOL (advapi32).
 *
 * The NAME comes from the same place Wine's does — the host's Unix account — so the
 * two engines read one source of truth and the fixture can compare it directly
 * instead of skipping it. Nothing is invented: if the host provides no account name
 * at all (no passwd entry, no $USER, no $LOGNAME) we abort rather than answer with a
 * plausible-looking placeholder, because a wrong user name is exactly the kind of
 * value a program would act on believing it.
 *
 * The SIZE contract is what a plausible implementation gets wrong, and it was
 * MEASURED (`winecorpus/win32_username.c`): `*pcb` is set to the required count
 * INCLUDING the terminating NUL, on success AND on failure; a buffer that is one
 * short fails with ERROR_INSUFFICIENT_BUFFER (122) and is left COMPLETELY UNTOUCHED
 * (proven on a poisoned buffer — no truncated name is written); a request of exactly
 * the reported count succeeds. The count is bytes for A and characters for W. */
static const char *u32_host_user_name(void) {
    /* The passwd database FIRST, because that is what Wine reads — and the order
     * matters, not just the set: a first version asked $USER/$LOGNAME/getlogin()
     * only, which works in an interactive shell and fails in the winediff child
     * (no environment, no controlling terminal). The abort caught it immediately
     * instead of it becoming a silent divergence, which is the whole point of
     * aborting rather than answering with a placeholder. */
    const char *n = 0;
#ifndef __wasm__
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_name && *pw->pw_name) n = pw->pw_name;
#endif
    if (!n || !*n) n = getenv("USER");
    if (!n || !*n) n = getenv("LOGNAME");
    if (!n || !*n)
        aret_unmodelled("GetUserName: the host provides no account name "
                        "(no passwd entry, no $USER, no $LOGNAME)");
    return n;
}
uint32_t aret_GetUserNameA(uint32_t esp) {
    char *buf = (char *)(uintptr_t)WU(0);
    uint32_t *pcb = (uint32_t *)(uintptr_t)WU(1);
    const char *name = u32_host_user_name();
    uint32_t need = (uint32_t)strlen(name) + 1;
    if (!pcb) return 0;
    if (*pcb < need || !buf) { *pcb = need; g_last_error = 122; return 0; }
    memcpy(buf, name, need);
    *pcb = need;
    return 1;
}
uint32_t aret_GetUserNameW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)(uintptr_t)WU(0);
    uint32_t *pcb = (uint32_t *)(uintptr_t)WU(1);
    const char *name = u32_host_user_name();
    uint32_t need = (uint32_t)strlen(name) + 1;
    if (!pcb) return 0;
    if (*pcb < need || !buf) { *pcb = need; g_last_error = 122; return 0; }
    u32_a2w(name, buf, (int)need);
    *pcb = need;
    return 1;
}

/* ExitWindowsEx(uFlags, dwReason) -> BOOL. We never log the user off / shut the
 * host down (sound: a transpiled app must not affect the real session); report
 * success so the app proceeds to its own teardown. Not oracle-compared (a real
 * Wine call would actually try to end the session). */
uint32_t aret_ExitWindowsEx(uint32_t esp) { (void)esp; return 1; }
/* MapWindowPoints(hwndFrom, hwndTo, lpPoints, cPoints) -> DWORD. Translate points
 * between two windows' client spaces. Client origin (no non-client frame modelled)
 * = the window's screen position; a NULL window is the screen (origin 0,0). */
uint32_t aret_MapWindowPoints(uint32_t esp) {
    int32_t *pt = (int32_t *)WP(2);
    uint32_t n = WU(3);
    int fi = u32_win_idx(WU(0)), ti = u32_win_idx(WU(1));
    int fx = fi >= 0 ? g_u32_win[fi].x : 0, fy = fi >= 0 ? g_u32_win[fi].y : 0;
    int tx = ti >= 0 ? g_u32_win[ti].x : 0, ty = ti >= 0 ? g_u32_win[ti].y : 0;
    int dx = fx - tx, dy = fy - ty;
    if (pt) for (uint32_t k = 0; k < n; k++) { pt[k * 2] += dx; pt[k * 2 + 1] += dy; }
    return ((uint32_t)(dy & 0xFFFF) << 16) | (uint32_t)(dx & 0xFFFF);
}
/* CheckDlgButton(hDlg, nIDButton, uCheck) -> BOOL; IsDlgButtonChecked(hDlg,id) ->
 * UINT. The check state lives on the child control window. */
static int u32_dlg_ctrl(uint32_t hdlg, int id) {
    int d = u32_win_idx(hdlg); if (d < 0) return -1;
    uint32_t dh = (uint32_t)(d + 1);
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].parent == dh && g_u32_win[i].ctrl_id == id) return i;
    return -1;
}
uint32_t aret_CheckDlgButton(uint32_t esp) {
    int c = u32_dlg_ctrl(WU(0), WI(1)); if (c < 0) return 0;
    g_u32_win[c].check_state = (int)WU(2);   /* BST_UNCHECKED/CHECKED/INDETERMINATE */
    u32_ctrl_recomposite(esp, c);
    return 1;
}
uint32_t aret_IsDlgButtonChecked(uint32_t esp) {
    int c = u32_dlg_ctrl(WU(0), WI(1)); return c < 0 ? 0 : (uint32_t)g_u32_win[c].check_state;
}
/* RedrawWindow(hwnd, lprcUpdate, hrgn, flags) -> BOOL. Fold into the paint model:
 * RDW_INVALIDATE marks a WM_PAINT owed, RDW_VALIDATE clears it, RDW_UPDATENOW
 * delivers it now. */
uint32_t aret_RedrawWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    uint32_t flags = WU(3);
    if ((flags & 0x0001u) && u32_win_paints(i)) {          /* RDW_INVALIDATE */
        g_u32_win[i].needs_paint = 1;
        if (flags & 0x0004u) g_u32_win[i].needs_erase = 1; /* RDW_ERASE */
    }
    if (flags & 0x0008u) g_u32_win[i].needs_paint = 0;     /* RDW_VALIDATE */
    if ((flags & 0x0100u) && g_u32_win[i].needs_paint && u32_win_paints(i)) { /* RDW_UPDATENOW */
        uint32_t wp = g_u32_win[i].wndproc;
        if (wp) u32_call_wndproc(esp, wp, (uint32_t)(i + 1), U32_WM_PAINT, 0, 0);
    }
    return 1;
}
/* Deferred window positioning. We apply each move immediately (the final window
 * state is identical to a batched apply; only repaint atomicity — cosmetic —
 * differs), and use the count as an opaque non-zero HDWP. */
/* Window property list: SetProp/GetProp/RemovePropA/W(hwnd, lpString, [hData]).
 * A per-(window,key) value store — used by subclassing/wrapping code to hang
 * per-window data off an HWND. */
#define U32_MAX_PROP 256
static struct { uint32_t hwnd; char key[64]; uint32_t val; int used; } g_u32_prop[U32_MAX_PROP];
static int u32_prop_find(uint32_t hwnd, const char *key) {
    for (int i = 0; i < U32_MAX_PROP; i++)
        if (g_u32_prop[i].used && g_u32_prop[i].hwnd == hwnd && strncmp(g_u32_prop[i].key, key, 64) == 0) return i;
    return -1;
}
static uint32_t u32_set_prop(uint32_t hwnd, const char *key, uint32_t val) {
    if (!key) return 0;
    int i = u32_prop_find(hwnd, key);
    if (i < 0) for (i = 0; i < U32_MAX_PROP; i++) if (!g_u32_prop[i].used) break;
    if (i >= U32_MAX_PROP) return 0;
    g_u32_prop[i].used = 1; g_u32_prop[i].hwnd = hwnd;
    int k = 0; for (; key[k] && k < 63; k++) g_u32_prop[i].key[k] = key[k]; g_u32_prop[i].key[k] = 0;
    g_u32_prop[i].val = val; return 1;
}
static uint32_t u32_get_prop(uint32_t hwnd, const char *key) {
    if (!key) return 0; int i = u32_prop_find(hwnd, key);
    return i < 0 ? 0 : g_u32_prop[i].val;
}
static uint32_t u32_rm_prop(uint32_t hwnd, const char *key) {
    if (!key) return 0; int i = u32_prop_find(hwnd, key);
    if (i < 0) return 0; uint32_t v = g_u32_prop[i].val; g_u32_prop[i].used = 0; return v;
}
uint32_t aret_SetPropA(uint32_t esp) { return u32_set_prop(WU(0), WCS(1), WU(2)); }
uint32_t aret_GetPropA(uint32_t esp) { return u32_get_prop(WU(0), WCS(1)); }
uint32_t aret_RemovePropA(uint32_t esp) { return u32_rm_prop(WU(0), WCS(1)); }
uint32_t aret_SetPropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_set_prop(WU(0), k, WU(2)); }
uint32_t aret_GetPropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_get_prop(WU(0), k); }
uint32_t aret_RemovePropW(uint32_t esp) { char k[64]; u32_w2n((const uint16_t *)WP(1), k, sizeof k); return u32_rm_prop(WU(0), k); }
/* GetOpenFileNameA/GetSaveFileNameA(OPENFILENAME*) -> BOOL. No file chooser is
 * shown (display-free): report cancellation (FALSE), the sound "user picked
 * nothing" outcome — never a guessed filename. */
uint32_t aret_GetOpenFileNameA(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_GetSaveFileNameA(uint32_t esp) { (void)esp; return 0; }
/* WinExec(lpCmdLine, uCmdShow) -> UINT. Launching a child process has no host
 * Windows to run it (same hard boundary as CreateProcess) -> ERROR_FILE_NOT_FOUND
 * (2), a value < 32 meaning failure. Sound: never pretends to have launched. */
uint32_t aret_WinExec(uint32_t esp) { (void)esp; return 2; }
/* GetVolumeInformationA(root, volNameBuf, volNameSz, serial, maxComp, fsFlags,
 * fsNameBuf, fsNameSz) -> BOOL. A defined ARET volume (invariant, like
 * GetDiskFreeSpace): env-dependent raw values are tested by invariant, not
 * bit-compared to Wine. */
uint32_t aret_GetVolumeInformationA(uint32_t esp) {
    char *vn = (char *)WP(1); uint32_t vns = WU(2);
    uint32_t *serial = (uint32_t *)WP(3), *maxc = (uint32_t *)WP(4), *flags = (uint32_t *)WP(5);
    char *fsn = (char *)WP(6); uint32_t fsns = WU(7);
    if (vn && vns) { const char *s = "ARET"; uint32_t n = 0; for (; s[n] && n < vns - 1; n++) vn[n] = s[n]; vn[n] = 0; }
    if (serial) *serial = 0x41524554u;
    if (maxc) *maxc = 255;
    if (flags) *flags = 0x00000002u;          /* FS_CASE_IS_PRESERVED */
    if (fsn && fsns) { const char *s = "NTFS"; uint32_t n = 0; for (; s[n] && n < fsns - 1; n++) fsn[n] = s[n]; fsn[n] = 0; }
    return 1;
}
uint32_t aret_BeginDeferWindowPos(uint32_t esp) { (void)esp; return 0x0DEF0001u; }
uint32_t aret_DeferWindowPos(uint32_t esp) {
    int i = u32_win_idx(WU(1));
    if (i >= 0) {
        uint32_t f = WU(7);
        if (!(f & 0x0002u)) { g_u32_win[i].x = WI(3); g_u32_win[i].y = WI(4); }  /* !SWP_NOMOVE */
        if (!(f & 0x0001u)) { g_u32_win[i].w = WI(5); g_u32_win[i].h = WI(6); }  /* !SWP_NOSIZE */
        if (f & 0x0040u) g_u32_win[i].visible = 1;
        if (f & 0x0080u) g_u32_win[i].visible = 0;
    }
    return WU(0);   /* return the (unchanged) HDWP */
}
uint32_t aret_EndDeferWindowPos(uint32_t esp) { (void)esp; return 1; }

/* Window text: routed through WM_SETTEXT/GETTEXT/GETTEXTLENGTH so a subclassing
 * WNDPROC observes them exactly as under Windows (DefWindowProc stores/reports).
 * A and W share the machinery; the ANSI/wide payload distinction is realised by
 * which DefWindowProc the guest's WNDPROC chains to. */
/* SetWindowTextA(HWND, lpString) -> BOOL. */
uint32_t aret_SetWindowTextA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    /* A predefined control has no app WNDPROC: store the text directly (DefWindowProc's
     * job), then repaint its parent dialog so the change shows (SetDlgItemText path). */
    if (!wndproc) { uint32_t o = 0; u32_defproc_text(WU(0), U32_WM_SETTEXT, 0, WU(1), 0, &o);
                    u32_ctrl_recomposite(esp, (int)WU(0) - 1); return o; }
    u32_call_wndproc(esp, wndproc, WU(0), U32_WM_SETTEXT, 0, WU(1));
    return 1;
}
uint32_t aret_SetWindowTextW(uint32_t esp) { return aret_SetWindowTextA(esp); }
/* GetWindowTextA(HWND, lpString, nMaxCount) -> int (chars copied, excl. NUL). */
uint32_t aret_GetWindowTextA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) { uint32_t o = 0; u32_defproc_text(WU(0), U32_WM_GETTEXT, WU(2), WU(1), 0, &o); return o; }
    return u32_call_wndproc(esp, wndproc, WU(0), U32_WM_GETTEXT, WU(2), WU(1));
}
uint32_t aret_GetWindowTextW(uint32_t esp) { return aret_GetWindowTextA(esp); }
/* GetWindowTextLengthA(HWND) -> int. */
uint32_t aret_GetWindowTextLengthA(uint32_t esp) {
    uint32_t wndproc = u32_win_wndproc(WU(0));
    if (!wndproc) { uint32_t o = 0; u32_defproc_text(WU(0), U32_WM_GETTEXTLENGTH, 0, 0, 0, &o); return o; }
    return u32_call_wndproc(esp, wndproc, WU(0), U32_WM_GETTEXTLENGTH, 0, 0);
}
uint32_t aret_GetWindowTextLengthW(uint32_t esp) { return aret_GetWindowTextLengthA(esp); }

/* GetSystemMetrics(nIndex) -> int. Display-independent metrics return their classic
 * fixed values; screen dimensions return ARET's defined virtual-desktop size. Raw
 * screen values are environment-dependent (doc 72 §4.5): tested by invariant, never
 * bit-compared to Wine. Unmodelled indices return 0. */
/* SystemParametersInfo(A/W)(uiAction, uiParam, pvParam, fWinIni) -> BOOL. Query
 * (SPI_GET*) or set (SPI_SET*) a system-wide parameter. Only the GET actions with a
 * display-independent, deterministic value (verified against Wine's headless default)
 * are modelled — they fill *pvParam and return TRUE. SET actions and unmodelled GETs
 * abort soundly: returning FALSE without filling pvParam would let a caller that
 * ignores the return read uninitialised memory (valid data under Wine) — a false
 * silent — and a no-op SET would diverge from a later GET. Values match GetSystemMetrics
 * (virtual 1024x768 screen invariant, doc 72 4.5). */
static int g_u32_screensave_active = 1;   /* SPI_{GET,SET}SCREENSAVEACTIVE, Wine default 1 */

/* SPI_GETNONCLIENTMETRICS (0x29) -> NONCLIENTMETRICS{A,W}. The non-client metrics and
 * the five shell fonts MFC (and every framework that themes its own UI) reads at
 * start-up. Values are the classic display-independent defaults, MEASURED from Wine
 * (`winecorpus/user32_ncm.c`) rather than derived: note they do NOT all match our
 * GetSystemMetrics (Wine reports SM_CYCAPTION 26 vs iCaptionHeight 25, SM_CYMENU 19 vs
 * iMenuHeight 18), so deriving them would have been a silent divergence.
 *
 * The A and W structures differ (LOGFONTA is 60 bytes, LOGFONTW 92), so the two shims
 * cannot share a filler — hence the `wide` parameter. The size comes from the caller's
 * `cbSize` FIELD, not from uiParam (measured: uiParam=0 still works). Both the modern
 * size (344/504, with iPaddedBorderWidth) and the pre-Vista one (340/500, without) are
 * accepted; with the latter iPaddedBorderWidth is left UNTOUCHED, as Wine does. Any
 * other size returns FALSE and writes nothing. */
static void u32_ncm_font(uint8_t *p, int wide, int32_t height) {
    /* LOGFONT{A,W}: lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight (LONG),
     * then 8 BYTEs, then lfFaceName[32] (chars or WCHARs). All zero but the three
     * fields Wine sets: height, weight 400 (FW_NORMAL) and charset 1 (DEFAULT_CHARSET).
     *
     * Face-name tail, MEASURED (the A and W paths genuinely differ in Wine): the W path
     * copies the whole array, so the bytes after the terminator are ZERO; the A path
     * converts the name and writes only up to the terminator, leaving the REST OF THE
     * CALLER'S BUFFER UNTOUCHED — except the LAST element of the array, which it forces
     * to NUL (the usual "guarantee termination" safety). Zero-filling the whole tail in
     * the A case would be a visible divergence (caught here by a poison-filled fixture
     * buffer), so reproduce both shapes exactly. */
    static const char face[] = "Tahoma";
    memset(p, 0, 28);                  /* the numeric and byte fields */
    *(int32_t *)(p + 0) = height;      /* lfHeight  */
    *(int32_t *)(p + 16) = 400;        /* lfWeight = FW_NORMAL */
    p[23] = 1;                         /* lfCharSet = DEFAULT_CHARSET */
    if (wide) memset(p + 28, 0, 64);   /* lfFaceName[32] WCHARs, tail zeroed */
    else p[28 + 31] = 0;               /* A: only the last element is forced to NUL */
    for (unsigned i = 0; i <= sizeof face - 1; i++) {  /* incl. the NUL */
        if (wide) *(uint16_t *)(p + 28 + 2 * i) = (uint16_t)(unsigned char)face[i];
        else p[28 + i] = (uint8_t)face[i];
    }
}
static uint32_t u32_get_ncm(uint32_t pv, int wide) {
    if (!pv) return 0;
    uint8_t *p = (uint8_t *)(uintptr_t)pv;
    const uint32_t lf = wide ? 92u : 60u;          /* sizeof LOGFONT{W,A}            */
    const uint32_t full = 4 + 20 + lf + 8 + lf + 8 + lf + lf + lf + 4; /* 344 / 504  */
    uint32_t cb = *(uint32_t *)p;                  /* the caller's cbSize field      */
    if (cb != full && cb != full - 4) return 0;    /* unknown layout -> FALSE, no write */
    /* Every field is written explicitly — deliberately NOT a blanket memset of the
     * caller's buffer: cbSize keeps the caller's value, and the A face-name tails must
     * stay untouched (see u32_ncm_font). Writing only what Wine writes keeps this
     * byte-identical to the oracle. */
    int32_t *iv = (int32_t *)p;
    iv[1] = 1;                                     /* iBorderWidth     */
    iv[2] = 17;                                    /* iScrollWidth     */
    iv[3] = 17;                                    /* iScrollHeight    */
    iv[4] = 18;                                    /* iCaptionWidth    */
    iv[5] = 25;                                    /* iCaptionHeight   */
    uint32_t o = 24;
    u32_ncm_font(p + o, wide, -13);                /* lfCaptionFont    */
    o += lf;
    *(int32_t *)(p + o) = 17; o += 4;              /* iSmCaptionWidth  */
    *(int32_t *)(p + o) = 17; o += 4;              /* iSmCaptionHeight */
    u32_ncm_font(p + o, wide, -11); o += lf;       /* lfSmCaptionFont  */
    *(int32_t *)(p + o) = 18; o += 4;              /* iMenuWidth       */
    *(int32_t *)(p + o) = 18; o += 4;              /* iMenuHeight      */
    u32_ncm_font(p + o, wide, -11); o += lf;       /* lfMenuFont       */
    u32_ncm_font(p + o, wide, -11); o += lf;       /* lfStatusFont     */
    u32_ncm_font(p + o, wide, -11); o += lf;       /* lfMessageFont    */
    if (cb == full) *(uint32_t *)(p + o) = 0;      /* iPaddedBorderWidth (modern size) */
    return 1;
}

static uint32_t u32_spi(uint32_t esp, int wide) {
    uint32_t action = w32_arg(esp, 0), ui = w32_arg(esp, 1), pv = w32_arg(esp, 2);
    switch (action) {
    case 0x0029: return u32_get_ncm(pv, wide);  /* SPI_GETNONCLIENTMETRICS */
    case 0x0030:  /* SPI_GETWORKAREA -> RECT{0,0,W,H} (no taskbar reserved) */
        if (pv) { int32_t *r = (int32_t *)(uintptr_t)pv;
                  r[0] = 0; r[1] = 0; r[2] = U32_SCREEN_W; r[3] = U32_SCREEN_H; }
        return 1;
    case 0x0001: if (pv) *(int32_t *)(uintptr_t)pv = 1; return 1; /* SPI_GETBEEP           */
    case 0x0005: if (pv) *(int32_t *)(uintptr_t)pv = 1; return 1; /* SPI_GETBORDER         */
    /* SCREENSAVEACTIVE is stateful: SET stores uiParam, GET reads it back (verified vs
     * Wine — SET(0) then GET returns 0). SET returns TRUE; no display side effect. */
    case 0x0010: if (pv) *(int32_t *)(uintptr_t)pv = g_u32_screensave_active; return 1;
    case 0x0011: g_u32_screensave_active = (int)ui; return 1; /* SPI_SETSCREENSAVEACTIVE */
    case 0x0026: if (pv) *(int32_t *)(uintptr_t)pv = 0; return 1; /* SPI_GETDRAGFULLWINDOWS */
    case 0x0068: if (pv) *(int32_t *)(uintptr_t)pv = 3; return 1; /* SPI_GETWHEELSCROLLLINES */
    /* The "UI effects" family (0x1000-0x1042): per-effect BOOLs every modern shell and
     * framework queries at startup — MFC/WinMerge stops on SPI_GETMENUANIMATION (0x1002).
     * Each writes exactly ONE 32-bit BOOL, proven by querying Wine through a poisoned
     * buffer: only 4 bytes change, the rest keeps the poison. The values are NOT uniform
     * and are not derivable from anything — they had to be measured one by one. */
    case 0x1000:                                                  /* ACTIVEWINDOWTRACKING  */
    case 0x1002:                                                  /* MENUANIMATION         */
    case 0x1004:                                                  /* COMBOBOXANIMATION     */
    case 0x1006:                                                  /* LISTBOXSMOOTHSCROLLING*/
    case 0x100c:                                                  /* ACTIVEWNDTRKZORDER    */
    case 0x100e:                                                  /* HOTTRACKING           */
    case 0x1012:                                                  /* MENUFADE              */
    case 0x1014:                                                  /* SELECTIONFADE         */
    case 0x1016:                                                  /* TOOLTIPANIMATION      */
    case 0x1018:                                                  /* TOOLTIPFADE           */
    case 0x101a:                                                  /* CURSORSHADOW          */
    case 0x1024: if (pv) *(int32_t *)(uintptr_t)pv = 0; return 1; /* DROPSHADOW            */
    case 0x1008:                                                  /* GRADIENTCAPTIONS      */
    case 0x100a:                                                  /* KEYBOARDCUES          */
    case 0x1022:                                                  /* FLATMENU              */
    case 0x1042: if (pv) *(int32_t *)(uintptr_t)pv = 1; return 1; /* CLIENTAREAANIMATION   */
    /* Measured as REJECTED by Wine: FALSE with pvParam left untouched (the poison
     * survives intact). Reporting "unavailable" is not a guessed value — the caller is
     * told nothing was written, exactly as Wine tells it.
     * The last-error behaviour is NOT uniform, and the difference is real: 0x102a and
     * 0x1082 set ERROR_INVALID_SPI_VALUE, while 0x0042 fails without touching it at all.
     * A first measurement missed this by not resetting the error before each query, so
     * one action's 1439 leaked into the readings that followed — the fixture, which does
     * reset, is what caught it. */
    case 0x0042: return 0;                                        /* SHOWSOUNDS            */
    case 0x102a:                                                  /* UIEFFECTS             */
    case 0x1082: g_last_error = 1439 /*ERROR_INVALID_SPI_VALUE*/; return 0; /* MOUSEVANISH */
    default: {
        char m[64];
        snprintf(m, sizeof m, "SystemParametersInfo%c: unmodelled action %#x",
                 wide ? 'W' : 'A', action);
        aret_unmodelled(m);
        return 0;
    }
    }
}
uint32_t aret_SystemParametersInfoA(uint32_t esp) { return u32_spi(esp, 0); }
uint32_t aret_SystemParametersInfoW(uint32_t esp) { return u32_spi(esp, 1); }

uint32_t aret_GetSystemMetrics(uint32_t esp) {
    switch (WI(0)) {
    case 0:  case 78: return U32_SCREEN_W;  /* SM_CXSCREEN / SM_CXVIRTUALSCREEN */
    case 1:  case 79: return U32_SCREEN_H;  /* SM_CYSCREEN / SM_CYVIRTUALSCREEN */
    case 16: return U32_SCREEN_W;           /* SM_CXFULLSCREEN (approx) */
    case 17: return U32_SCREEN_H;           /* SM_CYFULLSCREEN (approx) */
    case 2:  return 16;   /* SM_CXVSCROLL */
    case 3:  return 16;   /* SM_CYHSCROLL */
    case 4:  return 19;   /* SM_CYCAPTION */
    case 5:  return 1;    /* SM_CXBORDER */
    case 6:  return 1;    /* SM_CYBORDER */
    case 11: return 32;   /* SM_CXICON */
    case 12: return 32;   /* SM_CYICON */
    case 13: return 32;   /* SM_CXCURSOR */
    case 14: return 32;   /* SM_CYCURSOR */
    case 15: return 19;   /* SM_CYMENU */
    case 30: case 31: return 18; /* SM_CXSIZE / SM_CYSIZE */
    case 32: return 4;    /* SM_CXFRAME / SM_CXSIZEFRAME */
    case 33: return 4;    /* SM_CYFRAME / SM_CYSIZEFRAME */
    case 43: return 3;    /* SM_CMOUSEBUTTONS */
    case 45: return 2;    /* SM_CXEDGE */
    case 46: return 2;    /* SM_CYEDGE */
    case 49: return 16;   /* SM_CXSMICON */
    case 50: return 16;   /* SM_CYSMICON */
    case 54: case 55: return 18; /* SM_CXMENUSIZE / SM_CYMENUSIZE (measured vs Wine) */
    case 68: case 69: return 4;  /* SM_CXDRAG / SM_CYDRAG — drag threshold, fixed 4px
                                  * (measured vs Wine; surfaced by the relay diff on
                                  * WinMerge as returning 0 where Windows returns 4). */
    case 71: case 72: return 13; /* SM_CXMENUCHECK / SM_CYMENUCHECK (measured vs Wine) */
    default: return 0;
    }
}

/* ================================================================== */
/* Dialogs — DLGTEMPLATE parse -> controls + modal pump (display-free) */
/* ================================================================== */
/* A dialog is a window (wndproc = the app's DLGPROC) whose child controls are
 * windows too (each with its control id + template text; wndproc 0 = a system
 * control, its text served natively). DialogBoxParamA creates them, sends
 * WM_INITDIALOG, and runs a modal pump; a DLGPROC that EndDialog's during
 * WM_INITDIALOG (the scriptable/headless case) returns at once. If a dialog
 * instead waits for user input there is nothing to pump headless -> abort loudly
 * (never hang or fake a result). Reuses the G2a window table; still display-free
 * (real pixels are G2b). */
#define U32_WM_INITDIALOG 0x0110u

static uint32_t g_u32_modal_hwnd;   /* active modal dialog (0 = none) */
static int      g_u32_modal_ended;
static int      g_u32_modal_result;

/* Resolve a dialog template resource (RT_DIALOG=5) to its bytes, or NULL. */
static const uint8_t *u32_dlg_template(uint32_t name_ref) {
    const uint8_t *de = u32_rsrc_data_entry(5 /* RT_DIALOG */, name_ref);
    if (!de) return NULL;
    return (const uint8_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
}
/* Advance past a template "sz_Or_Ord" field (WORD 0 = empty; 0xFFFF + WORD ordinal;
 * else a NUL-terminated WCHAR string). If `title`, narrow a string form to ANSI. */
static const uint8_t *u32_dt_szord(const uint8_t *p, char *title, int cap) {
    if (title && cap > 0) title[0] = 0;
    uint16_t w = *(const uint16_t *)p;
    if (w == 0x0000) return p + 2;
    if (w == 0xFFFF) return p + 4;                       /* 0xFFFF + ordinal WORD */
    const uint16_t *s = (const uint16_t *)p;
    int i = 0;
    while (s[i]) { if (title && i < cap - 1) title[i] = (char)(s[i] & 0xFF); i++; }
    if (title && cap > 0) title[i < cap ? i : cap - 1] = 0;
    return (const uint8_t *)(s + i + 1);                 /* past the NUL */
}
/* DWORD-align a cursor relative to the template base. */
static const uint8_t *u32_dt_align(const uint8_t *base, const uint8_t *p) {
    size_t off = (size_t)(p - base);
    return base + ((off + 3) & ~(size_t)3);
}
/* Allocate a child-control window (system control: wndproc 0, text stored). Geometry
 * (x,y,w,h) is parent-relative pixels (already converted from dialog units); cls is the
 * control's window class ("Button"/"Edit"/"Static"/… — drives the built-in paint proc). */
static uint32_t u32_new_control(uint32_t parent, int ctrl_id, uint32_t style, const char *title,
                                int x, int y, int w, int h, const char *cls) {
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) {
            memset(&g_u32_win[i], 0, sizeof g_u32_win[i]);
            g_u32_win[i].used = 1;
            g_u32_win[i].parent = parent;
            g_u32_win[i].style = style;
            g_u32_win[i].enabled = (style & 0x08000000u) ? 0 : 1;  /* WS_DISABLED */
            g_u32_win[i].visible = (style & 0x10000000u) ? 1 : 0;  /* WS_VISIBLE */
            g_u32_win[i].ctrl_id = ctrl_id;
            g_u32_win[i].cur_sel = -1;                            /* LB/CB: no selection (LB_ERR) */
            g_u32_win[i].x = x; g_u32_win[i].y = y; g_u32_win[i].w = w; g_u32_win[i].h = h;
            if (cls) { size_t n = strnlen(cls, sizeof g_u32_win[i].classname - 1);
                       memcpy(g_u32_win[i].classname, cls, n); g_u32_win[i].classname[n] = 0; }
            int k = 0; if (title) for (; title[k] && k < 255; k++) g_u32_win[i].title[k] = title[k];
            g_u32_win[i].title[k] = 0;
            return (uint32_t)(i + 1);
        }
    }
    return 0;
}
/* Compute+store a dialog's base units from its font (defined after the FreeType
 * helpers). */
static void u32_dlg_base_units(int has_font, int point_size, int weight, int italic, const char *face, int *out_bx, int *out_by, uint32_t *out_font);
static int gdi_muldiv(long long a, long long b, long long c);   /* fwd: GDI round-to-nearest MulDiv */
/* Parse a DLGTEMPLATE(EX) -> create the dialog window (wndproc = dlgproc) and its
 * child controls. Returns the dialog HWND (0 on failure). Handles both templates. */
static uint32_t u32_dialog_create(uint32_t esp, const uint8_t *tpl, uint32_t dlgproc, uint32_t parent) {
    const uint8_t *p = tpl;
    int ex = 0;
    uint32_t style; uint16_t cdit; int16_t d_cx, d_cy;
    if (*(const uint16_t *)p == 1 && *(const uint16_t *)(p + 2) == 0xFFFF) {  /* DLGTEMPLATEEX */
        ex = 1;
        style = *(const uint32_t *)(p + 12);            /* after dlgVer,sig,helpID,exStyle */
        cdit  = *(const uint16_t *)(p + 16);
        d_cx  = *(const int16_t *)(p + 22); d_cy = *(const int16_t *)(p + 24);
        p += 26;                                        /* +cDlgItems(2)+x,y,cx,cy(8) -> menu */
    } else {                                            /* classic DLGTEMPLATE */
        style = *(const uint32_t *)p;
        cdit  = *(const uint16_t *)(p + 8);             /* style(4)+exStyle(4) -> cdit */
        d_cx  = *(const int16_t *)(p + 14); d_cy = *(const int16_t *)(p + 16);
        p += 18;                                        /* +cdit(2)+x,y,cx,cy(8) -> menu */
    }
    char dtitle[256];
    p = u32_dt_szord(p, NULL, 0);                        /* menu */
    p = u32_dt_szord(p, NULL, 0);                        /* window class */
    p = u32_dt_szord(p, dtitle, sizeof dtitle);         /* caption */
    int has_font = 0, f_pt = 0, f_weight = 0, f_ital = 0; char f_face[64]; f_face[0] = 0;
    if (style & 0x40u /* DS_SETFONT */) {
        has_font = 1;
        f_pt = *(const uint16_t *)p;
        if (ex) { f_weight = *(const uint16_t *)(p + 2); f_ital = *(const uint8_t *)(p + 4); p += 6; }
        else p += 2;                                    /* classic: point size only */
        p = u32_dt_szord(p, f_face, sizeof f_face);     /* typeface */
    }
    /* Base units first (from the font), so the dialog window is created at its real
     * pixel size — a visible dialog then gets a correctly-sized SDL window at once. */
    int bx = 0, by = 0; uint32_t dfont = 0;
    u32_dlg_base_units(has_font, f_pt, f_weight, f_ital, f_face, &bx, &by, &dfont);
    int dw = bx > 0 ? gdi_muldiv(d_cx, bx, 4) : 0;
    int dh = by > 0 ? gdi_muldiv(d_cy, by, 8) : 0;
    uint32_t hDlg = u32_window_create(dlgproc, 0, style, 0, 0, dw, dh, parent, dtitle, 0);
    if (!hDlg) return 0;
    int hi = (int)hDlg - 1;
    /* HCBT_CREATEWND for the dialog window, before WM_INITDIALOG: MFC's CBT filter uses
     * it to attach the CDialog CWnd (via its m_pWndInit thread-state) and subclass the
     * wndproc, which is what makes AfxDlgProc route WM_INITDIALOG to OnInitDialog. The
     * dialog class is the standard #32770 atom (0x8002 — an atom, not a string to deref). */
    u32_fire_cbt_createwnd(esp, hi, 0, 0, 0x8002u, 0);
    g_u32_win[hi].du_x = bx; g_u32_win[hi].du_y = by;   /* pixels per dialog unit */
    g_u32_win[hi].is_dialog = 1;                        /* composite its controls when shown */
    g_u32_win[hi].dlg_font = dfont;                     /* dialog font, applied to controls below */
    for (int c = 0; c < cdit; c++) {
        p = u32_dt_align(tpl, p);
        uint32_t cstyle, cid;
        int16_t ix, iy, icx, icy;
        if (ex) {
            cstyle = *(const uint32_t *)(p + 8);        /* helpID(4)+exStyle(4) -> style */
            ix = *(const int16_t *)(p + 12); iy = *(const int16_t *)(p + 14);
            icx = *(const int16_t *)(p + 16); icy = *(const int16_t *)(p + 18);
            p += 20; cid = *(const uint32_t *)p; p += 4; /* +x,y,cx,cy(8) -> id(DWORD) */
        } else {
            cstyle = *(const uint32_t *)p;
            ix = *(const int16_t *)(p + 8); iy = *(const int16_t *)(p + 10);
            icx = *(const int16_t *)(p + 12); icy = *(const int16_t *)(p + 14);
            p += 16; cid = *(const uint16_t *)p; p += 2; /* +exStyle(4)+x,y,cx,cy(8) -> id(WORD) */
        }
        /* Control class: predefined atom (0xFFFF + ordinal) or a name string. */
        char cclass[64]; cclass[0] = 0;
        if (*(const uint16_t *)p == 0xFFFF) {
            uint16_t ord = *(const uint16_t *)(p + 2);
            const char *nm = (ord == 0x80) ? "Button" : (ord == 0x81) ? "Edit" :
                             (ord == 0x82) ? "Static" : (ord == 0x83) ? "ListBox" :
                             (ord == 0x84) ? "ScrollBar" : (ord == 0x85) ? "ComboBox" : NULL;
            if (nm) { size_t L = strlen(nm); memcpy(cclass, nm, L); cclass[L] = 0; }
            p += 4;
        } else {
            p = u32_dt_szord(p, cclass, sizeof cclass);  /* class name string */
        }
        char ctitle[256];
        p = u32_dt_szord(p, ctitle, sizeof ctitle);     /* control caption */
        uint16_t extra = *(const uint16_t *)p; p += 2;  /* creation-data byte count */
        p += extra;
        /* Dialog units -> parent-relative pixels via the dialog's base units. */
        int px = bx > 0 ? gdi_muldiv(ix, bx, 4) : 0, py = by > 0 ? gdi_muldiv(iy, by, 8) : 0;
        int pw = bx > 0 ? gdi_muldiv(icx, bx, 4) : 0, ph = by > 0 ? gdi_muldiv(icy, by, 8) : 0;
        uint32_t hc = u32_new_control(hDlg, (int)cid, cstyle, ctitle, px, py, pw, ph, cclass);
        /* Windows sends WM_SETFONT(dialog font) to each control at dialog init; mirror
         * it so a control paints its caption in the dialog font (the app may override). */
        if (hc && dfont) g_u32_win[hc - 1].ctrl_font = dfont;
        /* Keyboard focus is set AFTER WM_INITDIALOG by u32_dialog_default_focus (first
         * tab-stop), the way the real dialog manager does it — not guessed at create time. */
    }
#ifdef ARET_HAVE_SDL
    /* If the template is WS_VISIBLE, the window auto-showed during u32_window_create —
     * before is_dialog was set and before the controls existed. Re-compose now that the
     * children are in place, and present. (A dialog shown later via ShowWindow composes
     * through sdl_window_show, which sees is_dialog set.) */
    if (g_u32_win[hi].visible && g_u32_win[hi].client_bmp) {
        u32_dialog_composite(0, hi);   /* create-time: DLGPROC/WM_INITDIALOG not run yet, esp not threaded here */
        sdl_window_present(hi);
    }
#endif
    return hDlg;
}
/* Destroy a dialog and all its child controls. */
static void u32_dialog_destroy(uint32_t hDlg) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && ((uint32_t)(i + 1) == hDlg || g_u32_win[i].parent == hDlg)) {
#ifdef ARET_HAVE_SDL
            /* Free a control's own client framebuffer (allocated when a lifted child
             * is composited); the dialog's own is freed by sdl_window_destroy. */
            if ((uint32_t)(i + 1) != hDlg) u32_free_child_bmp(i);
#endif
            g_u32_win[i].used = 0;
        }
}
/* Find a dialog control by id (0 if none). */
static uint32_t u32_dlg_item(uint32_t hDlg, int id) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].parent == hDlg && g_u32_win[i].ctrl_id == id)
            return (uint32_t)(i + 1);
    return 0;
}

/* After WM_INITDIALOG returns TRUE, the Win32 dialog manager gives the keyboard focus
 * to the first tab-stop control (GetNextDlgTabItem(hDlg, NULL, FALSE)) and fires
 * WM_SETFOCUS — matching Wine. A FALSE return means the DLGPROC set focus itself, so we
 * leave it. u32_set_focus / u32_next_dlg_item are defined later in the file. */
static uint32_t u32_set_focus(uint32_t esp, uint32_t neu);
static uint32_t u32_next_dlg_item(uint32_t hDlg, uint32_t cur, int prev, int tab);
static void u32_dialog_default_focus(uint32_t esp, uint32_t hDlg, uint32_t initret) {
    if (!initret) return;                                  /* DLGPROC set focus itself */
    uint32_t first = u32_next_dlg_item(hDlg, 0, 0, 1);     /* first visible+enabled tab-stop */
    if (first) u32_set_focus(esp, first);
}

/* Modal dialog core (shared by DialogBoxParam and DialogBoxIndirectParam — they
 * differ only in where the DLGTEMPLATE comes from): create, WM_INITDIALOG, pump until
 * EndDialog, return the EndDialog result. */
static uint32_t u32_dialog_modal(uint32_t esp, const uint8_t *tpl, uint32_t dlgproc,
                                 uint32_t parent, uint32_t param) {
    if (!tpl || !dlgproc) return (uint32_t)-1;
    if (g_u32_modal_hwnd) aret_partial("nested modal DialogBox (mono-thread model)");
    uint32_t hDlg = u32_dialog_create(esp, tpl, dlgproc, parent);
    if (!hDlg) return (uint32_t)-1;
    g_u32_modal_hwnd = hDlg; g_u32_modal_ended = 0; g_u32_modal_result = 0;
    uint32_t initret = u32_call_wndproc(esp, dlgproc, hDlg, U32_WM_INITDIALOG, 0, param);
    if (!g_u32_modal_ended) u32_dialog_default_focus(esp, hDlg, initret);
    while (!g_u32_modal_ended) {
        u32_pump_timers();
#ifdef ARET_HAVE_SDL
        sdl_pump();                       /* drain real mouse/keyboard/close into the queue */
#endif
        if (!u32_q_empty()) {
            uint32_t m[7]; u32_q_peek_copy(m, 1);
            uint32_t wp = u32_win_wndproc(m[0]);
            if (wp) u32_call_wndproc(esp, wp, m[0], m[1], m[2], m[3]);
            if (m[0] == hDlg && m[1] == 0x0202u /*WM_LBUTTONUP*/)   /* route click to a child */
                u32_dialog_hittest_click(esp, (int)hDlg - 1,
                                         (int)(int16_t)(m[3] & 0xFFFF), (int)(int16_t)((m[3] >> 16) & 0xFFFF));
        } else {
#ifdef ARET_HAVE_SDL
            if (g_u32_win[(int)hDlg - 1].sdl_win) { SDL_WaitEventTimeout(NULL, 20); continue; }
#endif
            aret_partial("modal DialogBox: DLGPROC did not EndDialog and no events (headless)");
        }
    }
    int result = g_u32_modal_result;
    u32_dialog_destroy(hDlg);
    g_u32_modal_hwnd = 0;
    return (uint32_t)result;
}
/* Modeless dialog core (CreateDialogParam / CreateDialogIndirectParam). */
static uint32_t u32_dialog_modeless(uint32_t esp, const uint8_t *tpl, uint32_t dlgproc,
                                    uint32_t parent, uint32_t param) {
    if (!tpl || !dlgproc) return 0;
    uint32_t hDlg = u32_dialog_create(esp, tpl, dlgproc, parent);
    if (!hDlg) return 0;
    uint32_t initret = u32_call_wndproc(esp, dlgproc, hDlg, U32_WM_INITDIALOG, 0, param);
    u32_dialog_default_focus(esp, hDlg, initret);
    return hDlg;
}
/* DialogBoxParamA(hInst, lpTemplate, hWndParent, lpDialogFunc, dwInitParam) -> INT_PTR.
 * lpTemplate is a resource name/id. */
uint32_t aret_DialogBoxParamA(uint32_t esp) {
    return u32_dialog_modal(esp, u32_dlg_template(WU(1)), WU(3), WU(2), WU(4));
}
uint32_t aret_DialogBoxParamW(uint32_t esp) { return aret_DialogBoxParamA(esp); }
/* DialogBoxIndirectParamA(hInst, lpTemplate, hWndParent, lpDialogFunc, dwInitParam):
 * lpTemplate is a DIRECT pointer to an in-memory DLGTEMPLATE (host memory in
 * shared-stack mode), not a resource id. Same modal semantics otherwise. */
uint32_t aret_DialogBoxIndirectParamA(uint32_t esp) {
    return u32_dialog_modal(esp, (const uint8_t *)(uintptr_t)WU(1), WU(3), WU(2), WU(4));
}
uint32_t aret_DialogBoxIndirectParamW(uint32_t esp) { return aret_DialogBoxIndirectParamA(esp); }
/* CreateDialogParamA(...) -> HWND. Modeless: create + WM_INITDIALOG, return HWND. */
uint32_t aret_CreateDialogParamA(uint32_t esp) {
    return u32_dialog_modeless(esp, u32_dlg_template(WU(1)), WU(3), WU(2), WU(4));
}
uint32_t aret_CreateDialogParamW(uint32_t esp) { return aret_CreateDialogParamA(esp); }
/* CreateDialogIndirectParamA(...) — modeless, in-memory template. */
uint32_t aret_CreateDialogIndirectParamA(uint32_t esp) {
    return u32_dialog_modeless(esp, (const uint8_t *)(uintptr_t)WU(1), WU(3), WU(2), WU(4));
}
uint32_t aret_CreateDialogIndirectParamW(uint32_t esp) { return aret_CreateDialogIndirectParamA(esp); }
/* EndDialog(hDlg, nResult) -> BOOL. Ends the active modal loop with a result. */
uint32_t aret_EndDialog(uint32_t esp) {
    g_u32_modal_ended = 1; g_u32_modal_result = WI(1);
    return 1;
}
/* GetDlgItem(hDlg, nIDDlgItem) -> HWND. */
uint32_t aret_GetDlgItem(uint32_t esp) { return u32_dlg_item(WU(0), WI(1)); }
/* GetDlgCtrlID(hWnd) -> int (the control's id). */
uint32_t aret_GetDlgCtrlID(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    return i < 0 ? 0 : (uint32_t)g_u32_win[i].ctrl_id;
}
/* SetDlgItemTextA(hDlg, id, lpString) -> BOOL. A system control stores its text. */
uint32_t aret_SetDlgItemTextA(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    const char *s = WCS(2); int k = 0;
    if (s) for (; s[k] && k < 255; k++) g_u32_win[i].title[k] = s[k];
    g_u32_win[i].title[k] = 0;
    u32_ctrl_recomposite(esp, i);
    return 1;
}
/* GetDlgItemTextA(hDlg, id, lpString, cchMax) -> chars copied (excl NUL). */
uint32_t aret_GetDlgItemTextA(uint32_t esp) {
    char *buf = (char *)WP(2); uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = t[j];
    buf[n] = 0;
    return n;
}
/* SetDlgItemTextW(hDlg, id, lpString) — wide string, narrowed into the control. */
uint32_t aret_SetDlgItemTextW(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    u32_w2n((const uint16_t *)WP(2), g_u32_win[i].title, sizeof g_u32_win[i].title);
    u32_ctrl_recomposite(esp, i);
    return 1;
}
/* GetDlgItemTextW(hDlg, id, lpString, cchMax) — widen the stored ANSI text. */
uint32_t aret_GetDlgItemTextW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(2); uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *t = g_u32_win[i].title; uint32_t len = (uint32_t)strlen(t);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)t[j];
    buf[n] = 0;
    return n;
}
/* SetDlgItemInt(hDlg, id, uValue, bSigned) -> BOOL. */
uint32_t aret_SetDlgItemInt(uint32_t esp) {
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) return 0;
    if (WU(3)) snprintf(g_u32_win[i].title, sizeof g_u32_win[i].title, "%d", WI(2));
    else       snprintf(g_u32_win[i].title, sizeof g_u32_win[i].title, "%u", WU(2));
    u32_ctrl_recomposite(esp, i);
    return 1;
}
/* GetDlgItemInt(hDlg, id, lpTranslated, bSigned) -> UINT. */
uint32_t aret_GetDlgItemInt(uint32_t esp) {
    uint32_t *ok = (uint32_t *)WP(2);
    int i = u32_win_idx(u32_dlg_item(WU(0), WI(1)));
    if (i < 0) { if (ok) *ok = 0; return 0; }
    char *end = NULL; const char *t = g_u32_win[i].title;
    long v = WU(3) ? strtol(t, &end, 10) : (long)strtoul(t, &end, 10);
    if (ok) *ok = (end && end != t && *end == 0) ? 1 : 0;
    return (uint32_t)v;
}
/* SendDlgItemMessageA(hDlg, id, msg, wParam, lParam) -> LRESULT. */
uint32_t aret_SendDlgItemMessageA(uint32_t esp) {
    uint32_t child = u32_dlg_item(WU(0), WI(1));
    if (!child) return 0;
    uint32_t wp = u32_win_wndproc(child);
    if (wp) return u32_call_wndproc(esp, wp, child, WU(2), WU(3), WU(4));
    uint32_t r;
    if (u32_sys_control_msg(esp, child, WU(2), WU(3), WU(4), 0, &r)) return r;  /* system control */
    return 0;
}
uint32_t aret_SendDlgItemMessageW(uint32_t esp) {
    uint32_t child = u32_dlg_item(WU(0), WI(1));
    if (!child) return 0;
    uint32_t wp = u32_win_wndproc(child);
    if (wp) return u32_call_wndproc(esp, wp, child, WU(2), WU(3), WU(4));
    uint32_t r;
    if (u32_sys_control_msg(esp, child, WU(2), WU(3), WU(4), 1, &r)) return r;
    return 0;
}

/* ================================================================== */
/* GDI — object/DC model + bit-exact DIB drawing (framebuffer oracle)  */
/* ================================================================== */
/* GDI objects (DCs, bitmaps, brushes, pens, fonts) live in one handle table;
 * handles are opaque (base 0x30000000, distinct from HWND/GDI-stock). Drawing
 * targets a memory DIB section — a 32-bit BGRA buffer we own — so SetPixel/
 * FillRect/PatBlt are exact memory writes that match Wine's DIB byte-for-byte
 * (verified: a 32bpp BI_RGB pixel is [B,G,R,0]). The oracle hashes that buffer.
 * Screen/window DCs have no surface (no display) -> their drawing is a sound
 * no-op; only the offscreen DIB path is pixel-verified. Text (font raster) and
 * pen-edged shapes are NOT here (can't match Wine's rasteriser bit-for-bit) —
 * abort sound until a binary needs a modellable subset. GDI is vast; we stop at
 * the measured, exactly-reproducible core (doc 72 §5). */
#define GDI_MAX  512
#define GDI_BASE 0x30000000u
enum { GDIT_DC = 1, GDIT_BITMAP, GDIT_BRUSH, GDIT_PEN, GDIT_FONT, GDIT_RGN, GDIT_PALETTE };
static struct gdi_obj {
    int type, used, stock, null_obj;
    uint32_t sel_bitmap, sel_brush, sel_pen, sel_font, sel_palette;   /* DC */
    uint8_t *pal; int pal_count;                          /* PALETTE: pal_count PALETTEENTRY (4 bytes each) */
    uint32_t text_color, bk_color; int bk_mode; uint32_t text_align;  /* DC */
    int w, h, topdown, bpp; uint8_t *bits; int owns_bits; /* BITMAP */
    uint32_t color;                                      /* BRUSH/PEN */
    uint32_t pat_bitmap;                                  /* BRUSH: pattern bitmap handle (CreatePatternBrush) */
    int pen_style, pen_width;                            /* PEN */
    int lf_height, lf_weight, lf_italic, lf_quality;     /* FONT (LOGFONT) */
    int lf_underline, lf_strikeout;                      /* FONT (LOGFONT) */
    char lf_face[64];                                    /* FONT face name */
    int mapmode, savetop; int cur_x, cur_y;              /* DC map mode + save-stack + current pos (LOGICAL) */
    int vp_ox, vp_oy, win_ox, win_oy;                    /* viewport/window origin (mapping-mode transform) */
    int vp_ex, vp_ey, win_ex, win_ey;                    /* viewport/window extent (default 1 = identity) */
    struct { uint32_t font, brush, pen, tc, bc; int bm, mm; } sstk[8];  /* SaveDC/RestoreDC */
    int rgn_l, rgn_t, rgn_r, rgn_b, rgn_complex;         /* REGION: bounding rect + complex flag */
    int clip_set, clip_l, clip_t, clip_r, clip_b, clip_complex;  /* DC: user clip region (rect bbox) */
} g_gdi[GDI_MAX];

static uint32_t gdi_handle(int i) { return GDI_BASE | (uint32_t)i; }
static int gdi_idx(uint32_t h) {
    if ((h & 0xFF000000u) != GDI_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < GDI_MAX && g_gdi[i].used) ? (int)i : -1;
}
static int gdi_alloc(int type) {
    for (int i = 1; i < GDI_MAX; i++)
        if (!g_gdi[i].used) { memset(&g_gdi[i], 0, sizeof g_gdi[i]); g_gdi[i].used = 1; g_gdi[i].type = type; return i; }
    return 0;
}
/* The DIB surface a DC currently draws to (NULL if none / screen DC). */
static struct gdi_obj *gdi_dc_surface(uint32_t hdc) {
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return NULL;
    int b = gdi_idx(g_gdi[d].sel_bitmap);
    if (b < 0 || g_gdi[b].type != GDIT_BITMAP || !g_gdi[b].bits) return NULL;
    return &g_gdi[b];
}
/* Address of pixel (x,y) in a DIB (honours top-down vs bottom-up), or NULL. */
static uint8_t *gdi_px(struct gdi_obj *bm, int x, int y) {
    if (x < 0 || y < 0 || x >= bm->w || y >= bm->h) return NULL;
    int row = bm->topdown ? y : (bm->h - 1 - y);
    return bm->bits + ((size_t)row * bm->w + x) * 4;
}
/* COLORREF (0x00BBGGRR) -> DIB bytes [B,G,R,0], and back. */
static void gdi_put(struct gdi_obj *bm, int x, int y, uint32_t c) {
    uint8_t *p = gdi_px(bm, x, y); if (!p) return;
    p[0] = (c >> 16) & 0xFF; p[1] = (c >> 8) & 0xFF; p[2] = c & 0xFF; p[3] = 0;
}
static uint32_t gdi_getpx(struct gdi_obj *bm, int x, int y) {
    uint8_t *p = gdi_px(bm, x, y); if (!p) return 0xFFFFFFFFu; /* CLR_INVALID */
    return (uint32_t)p[2] | ((uint32_t)p[1] << 8) | ((uint32_t)p[0] << 16);
}

/* ---- Mapping-mode coordinate transform (logical <-> device) ----------------
 * Defined here (before any drawing primitive) so every primitive can transform. */
/* MulDiv: round(a*b/c) to nearest, ties AWAY from zero — Windows' GDI rounding
 * (measured vs Wine: /3 gives 1->0, 2->1, 5->2, -2->-1, -8->-3). */
static int gdi_muldiv(long long a, long long b, long long c) {
    if (c == 0) return 0;
    long long num = a * b;
    if (c < 0) { num = -num; c = -c; }
    long long half = c / 2;
    return (int)(num >= 0 ? (num + half) / c : -((-num + half) / c));
}
/* Logical -> device: dev = (log - winOrg) * vpExt/winExt + vpOrg. Identity by
 * default (MM_TEXT, org 0, ext 1) so untransformed drawing is unchanged. */
static void dc_l2d(int d, int lx, int ly, int *dx, int *dy) {
    *dx = gdi_muldiv(lx - g_gdi[d].win_ox, g_gdi[d].vp_ex, g_gdi[d].win_ex) + g_gdi[d].vp_ox;
    *dy = gdi_muldiv(ly - g_gdi[d].win_oy, g_gdi[d].vp_ey, g_gdi[d].win_ey) + g_gdi[d].vp_oy;
}
/* Device -> logical (inverse), for DPtoLP. */
static void dc_d2l(int d, int dx, int dy, int *lx, int *ly) {
    *lx = gdi_muldiv(dx - g_gdi[d].vp_ox, g_gdi[d].win_ex, g_gdi[d].vp_ex) + g_gdi[d].win_ox;
    *ly = gdi_muldiv(dy - g_gdi[d].vp_oy, g_gdi[d].win_ey, g_gdi[d].vp_ey) + g_gdi[d].win_oy;
}
/* Is the DC's mapping the identity (no transform needed)? Guards primitives that
 * don't yet apply the transform: they abort soundly under a non-identity map. */
static int dc_map_identity(int d) {
    return g_gdi[d].vp_ox == 0 && g_gdi[d].vp_oy == 0 && g_gdi[d].win_ox == 0 &&
           g_gdi[d].win_oy == 0 && g_gdi[d].vp_ex == g_gdi[d].win_ex &&
           g_gdi[d].vp_ey == g_gdi[d].win_ey;
}
/* Soundness guard for a drawing primitive that does NOT yet apply the mapping
 * transform: under a non-identity map it would draw at the wrong pixels, so abort
 * loudly instead (never a silent-wrong). Transformed data-driven when measured. */
#define GDI_MAP_GUARD(hdc, ret) do { \
    int _gd = gdi_idx(hdc); \
    if (_gd >= 0 && !dc_map_identity(_gd)) { \
        aret_partial("GDI primitive under non-identity mapping mode pending"); return (ret); } \
} while (0)

/* ================================================================== */
/* G2b — SDL2 window presentation (doc 72). Compiled only when the      */
/* program creates a window (`-DARET_HAVE_SDL`). Each visible top-level  */
/* window gets: (1) a client-area DIB framebuffer that GetDC/BeginPaint  */
/* bind to, so the program's GDI draws land in it; (2) a real           */
/* SDL_Window it is blitted to on UpdateWindow/EndPaint; (3) SDL input   */
/* events pumped into WM_* messages. Everything degrades to a sound      */
/* display-free no-op when there is no usable display (SDL_Init fails).  */
/* ================================================================== */
#ifdef ARET_HAVE_SDL
static int g_sdl_ready = 0;   /* 0 = not tried, 1 = video up, -1 = unavailable */
static int sdl_ensure(void) {
    if (g_sdl_ready) return g_sdl_ready > 0;
    /* Video only; no audio/events subsystems we don't drive. A missing display
     * (no DISPLAY, no dummy driver) is not an error here — we fall back to the
     * display-free path, never abort. */
    g_sdl_ready = (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) ? 1 : -1;
    if (g_sdl_ready > 0) SDL_StartTextInput();   /* deliver typed characters as SDL_TEXTINPUT */
    return g_sdl_ready > 0;
}
/* Create the client framebuffer + real SDL window for window i (idempotent). */
static void sdl_window_show(uint32_t esp, int i) {
    if (i < 0 || i >= U32_MAX_WIN || !g_u32_win[i].used) return;
    if (g_u32_win[i].sdl_win) { SDL_ShowWindow((SDL_Window *)g_u32_win[i].sdl_win); return; }
    int w = g_u32_win[i].w, h = g_u32_win[i].h;
    if (w <= 0) w = 320; if (h <= 0) h = 240;        /* sane default if unsized */
    if (w > 8192) w = 8192; if (h > 8192) h = 8192;
    /* Client-area framebuffer (a top-down 32bpp DIB) is allocated even if the
     * real window can't be created (headless): GetDC still gives the program a
     * surface to draw into, and the drawing round-trips like Wine's. */
    int b = gdi_alloc(GDIT_BITMAP);
    if (b) {
        g_gdi[b].w = w; g_gdi[b].h = h; g_gdi[b].topdown = 1; g_gdi[b].bpp = 32;
        g_gdi[b].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[b].owns_bits = 1;
        if (!g_gdi[b].bits) { g_gdi[b].used = 0; b = 0; }
    }
    g_u32_win[i].client_bmp = b ? gdi_handle(b) : 0;
    g_u32_win[i].cw = w; g_u32_win[i].ch = h;
    if (g_u32_win[i].is_dialog) u32_dialog_composite(esp, i);   /* fill 3DFACE + paint child controls */
    if (!sdl_ensure()) return;                       /* no display: framebuffer only */
    int px = g_u32_win[i].x, py = g_u32_win[i].y;
    SDL_Window *win = SDL_CreateWindow(g_u32_win[i].title[0] ? g_u32_win[i].title : "",
                                       px > 0 ? px : (int)SDL_WINDOWPOS_CENTERED,
                                       py > 0 ? py : (int)SDL_WINDOWPOS_CENTERED,
                                       w, h, SDL_WINDOW_SHOWN);
    if (!win) return;                                /* creation failed: framebuffer only */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture  *tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888,
                                                SDL_TEXTUREACCESS_STREAMING, w, h) : NULL;
    g_u32_win[i].sdl_win = win; g_u32_win[i].sdl_ren = ren; g_u32_win[i].sdl_tex = tex;
    sdl_window_present(i);
}
/* Blit the client framebuffer to the SDL window (no-op if not shown). The DIB
 * bytes are [B,G,R,0] which is exactly SDL_PIXELFORMAT_RGB888 memory order. */
static void sdl_window_present(int i) {
    if (i < 0 || i >= U32_MAX_WIN) return;
    SDL_Renderer *ren = (SDL_Renderer *)g_u32_win[i].sdl_ren;
    SDL_Texture  *tex = (SDL_Texture *)g_u32_win[i].sdl_tex;
    if (!ren || !tex) return;
    int b = gdi_idx(g_u32_win[i].client_bmp);
    if (b < 0 || !g_gdi[b].bits) return;
    SDL_UpdateTexture(tex, NULL, g_gdi[b].bits, g_u32_win[i].cw * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}
static void sdl_window_destroy(int i) {
    if (i < 0 || i >= U32_MAX_WIN) return;
    if (g_u32_win[i].sdl_tex) SDL_DestroyTexture((SDL_Texture *)g_u32_win[i].sdl_tex);
    if (g_u32_win[i].sdl_ren) SDL_DestroyRenderer((SDL_Renderer *)g_u32_win[i].sdl_ren);
    if (g_u32_win[i].sdl_win) SDL_DestroyWindow((SDL_Window *)g_u32_win[i].sdl_win);
    g_u32_win[i].sdl_tex = g_u32_win[i].sdl_ren = g_u32_win[i].sdl_win = NULL;
    int b = gdi_idx(g_u32_win[i].client_bmp);
    if (b >= 0) { if (g_gdi[b].owns_bits) free(g_gdi[b].bits); g_gdi[b].used = 0; }
    g_u32_win[i].client_bmp = 0;
}
static int sdl_any_window(void) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].sdl_win) return 1;
    return 0;
}
static int sdl_win_idx_from_id(uint32_t winid) {
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].sdl_win &&
            SDL_GetWindowID((SDL_Window *)g_u32_win[i].sdl_win) == winid) return i;
    return -1;
}
/* Map an SDL key symbol to a Win32 virtual-key code (the subset a GUI app tends
 * to test). Letters/digits share the ASCII value with their VK code. */
static uint32_t sdl_vk(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (uint32_t)(k - 'a' + 'A');   /* VK_A..VK_Z */
    if (k >= '0' && k <= '9') return (uint32_t)k;                 /* VK_0..VK_9 */
    switch (k) {
    case SDLK_ESCAPE: return 0x1B; case SDLK_RETURN: return 0x0D;
    case SDLK_SPACE:  return 0x20; case SDLK_TAB:    return 0x09;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    default: return (uint32_t)(k & 0xFF);
    }
}
/* Drain SDL input into the Win32 message queue. Only real user input (close,
 * mouse, keyboard) becomes a message; window-manager noise (expose/focus) does
 * NOT synthesise WM_PAINT/WM_ACTIVATE — those stay driven by the Win32
 * invalidation model, so the deterministic message oracle is untouched. */
/* The focused control's index if it is an EDIT (else -1) — the target of typed text. */
static int u32_focused_edit(void) {
    int fi = (g_u32_focus >= 1 && g_u32_focus <= U32_MAX_WIN && g_u32_win[g_u32_focus - 1].used) ? (int)g_u32_focus - 1 : -1;
    if (fi < 0 || strcasecmp(g_u32_win[fi].classname, "edit") != 0) return -1;
    return fi;
}
/* Append typed ASCII to the focused EDIT (non-ASCII skipped — a sound subset), recompose. */
static void u32_edit_key_text(const char *utf8) {
    int fi = u32_focused_edit(); if (fi < 0) return;
    int n = (int)strlen(g_u32_win[fi].title);
    for (const char *p = utf8; *p && n < (int)sizeof g_u32_win[fi].title - 1; p++)
        if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F) g_u32_win[fi].title[n++] = *p;
    g_u32_win[fi].title[n] = 0;
    u32_ctrl_recomposite(0, fi);   /* SDL input path: system EDIT only, no lifted control to drive */
}
/* Backspace in the focused EDIT. */
static void u32_edit_key_back(void) {
    int fi = u32_focused_edit(); if (fi < 0) return;
    int n = (int)strlen(g_u32_win[fi].title);
    if (n > 0) { g_u32_win[fi].title[n - 1] = 0; u32_ctrl_recomposite(0, fi); }
}
static void sdl_pump(void) {
    if (g_sdl_ready <= 0) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int wi; uint32_t lp;
        switch (e.type) {
        case SDL_QUIT:
            for (int i = 0; i < U32_MAX_WIN; i++)
                if (g_u32_win[i].used && g_u32_win[i].sdl_win)
                    u32_q_push((uint32_t)(i + 1), 0x0010u /* WM_CLOSE */, 0, 0);
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                wi = sdl_win_idx_from_id(e.window.windowID);
                if (wi >= 0) u32_q_push((uint32_t)(wi + 1), 0x0010u /* WM_CLOSE */, 0, 0);
            }
            break;
        case SDL_MOUSEMOTION:
            wi = sdl_win_idx_from_id(e.motion.windowID);
            if (wi >= 0) { lp = ((uint32_t)(e.motion.x & 0xFFFF)) | ((uint32_t)(e.motion.y & 0xFFFF) << 16);
                           u32_q_push((uint32_t)(wi + 1), 0x0200u /* WM_MOUSEMOVE */, 0, lp); }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            wi = sdl_win_idx_from_id(e.button.windowID);
            if (wi < 0) break;
            lp = ((uint32_t)(e.button.x & 0xFFFF)) | ((uint32_t)(e.button.y & 0xFFFF) << 16);
            int down = (e.type == SDL_MOUSEBUTTONDOWN);
            uint32_t msg = (e.button.button == SDL_BUTTON_RIGHT)
                         ? (down ? 0x0204u : 0x0205u)   /* WM_RBUTTONDOWN/UP */
                         : (down ? 0x0201u : 0x0202u);  /* WM_LBUTTONDOWN/UP */
            u32_q_push((uint32_t)(wi + 1), msg, 0, lp);
            break; }
        case SDL_TEXTINPUT:
            /* Typed characters go to the focused EDIT (shown on screen); also delivered
             * as WM_CHAR to a top-level window's WNDPROC. */
            u32_edit_key_text(e.text.text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            wi = sdl_win_idx_from_id(e.key.windowID);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_BACKSPACE) u32_edit_key_back();
            if (wi >= 0)
                u32_q_push((uint32_t)(wi + 1),
                           e.type == SDL_KEYDOWN ? 0x0100u : 0x0101u /* WM_KEYDOWN/UP */,
                           sdl_vk(e.key.keysym.sym), 0);
            break;
        default: break;
        }
    }
}
/* The client-framebuffer HBITMAP a window DC should draw into, or 0. */
static uint32_t u32_win_client_bmp(uint32_t hwnd) {
    if (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used)
        return g_u32_win[hwnd - 1].client_bmp;
    return 0;
}
#endif  /* ARET_HAVE_SDL */

/* ---- DC lifecycle ---- */
static void u32_dc_defaults(int d);   /* selects the DC's default stock objects */
/* GetDC/GetWindowDC(hwnd) -> HDC. A screen/window DC (no offscreen surface: no
 * display, so its drawing is a sound no-op). */
uint32_t aret_GetDC(uint32_t esp) {
    int i = gdi_alloc(GDIT_DC); if (!i) return 0;
    u32_dc_defaults(i);
#ifdef ARET_HAVE_SDL
    /* A window DC draws into that window's client framebuffer (G2b), so GDI paints
     * land on the visible surface; a screen/NULL DC stays a sound no-op. */
    uint32_t cb = u32_win_client_bmp(WU(0));
    if (cb) g_gdi[i].sel_bitmap = cb;
#endif
    return gdi_handle(i);
}
uint32_t aret_GetWindowDC(uint32_t esp) { return aret_GetDC(esp); }
/* ReleaseDC(hwnd, hdc) -> int (1). Presents the window's framebuffer if a window
 * DC was released (a program often draws to GetDC then expects it on screen). */
uint32_t aret_ReleaseDC(uint32_t esp) {
    int i = gdi_idx(WU(1));
#ifdef ARET_HAVE_SDL
    { int wi = u32_win_idx(WU(0));
      if (wi >= 0) {
          if (g_u32_win[wi].parent == 0) u32_present_toplevel(esp, wi);   /* compose children over the drawn client */
          else sdl_window_present(wi);
          /* A window DC has the window's shared framebuffer "selected"; free the
           * DC object but never the framebuffer (owned by the window). */
          if (i >= 0 && g_gdi[i].sel_bitmap == g_u32_win[wi].client_bmp) { g_gdi[i].used = 0; return 1; }
      } }
#endif
    if (i >= 0 && !g_gdi[i].sel_bitmap) g_gdi[i].used = 0;
    return 1;
}
/* CreateCompatibleDC(hdc) -> HDC (a memory DC; select a bitmap before drawing). */
uint32_t aret_CreateCompatibleDC(uint32_t esp) { (void)esp; int i = gdi_alloc(GDIT_DC); if (i) u32_dc_defaults(i); return i ? gdi_handle(i) : 0; }
/* DeleteDC(hdc) -> BOOL. */
uint32_t aret_DeleteDC(uint32_t esp) { int i = gdi_idx(WU(0)); if (i < 0) return 0; g_gdi[i].used = 0; return 1; }
/* BeginPaint(hwnd, LPPAINTSTRUCT) -> HDC. Zero the PAINTSTRUCT, hand back a DC. */
uint32_t aret_BeginPaint(uint32_t esp) {
    uint8_t *ps = (uint8_t *)WP(1);
    int i = gdi_alloc(GDIT_DC); if (i) u32_dc_defaults(i);
    uint32_t hdc = i ? gdi_handle(i) : 0;
    int wi = u32_win_idx(WU(0));
#ifdef ARET_HAVE_SDL
    /* Paint into the window's client framebuffer, like GetDC. */
    if (i) { uint32_t cb = u32_win_client_bmp(WU(0)); if (cb) g_gdi[i].sel_bitmap = cb; }
#endif
    /* A pending erase sends WM_ERASEBKGND now (DefWindowProc fills the class
     * brush into the DC we just bound); then the update region is validated.
     * needs_erase is cleared *before* the callback so a re-entrant BeginPaint
     * cannot loop. */
    if (wi >= 0) {
        if (g_u32_win[wi].needs_erase) {
            g_u32_win[wi].needs_erase = 0;
            uint32_t wp = g_u32_win[wi].wndproc;
            if (wp) u32_call_wndproc(esp, wp, WU(0), U32_WM_ERASEBKGND, hdc, 0);
        }
        g_u32_win[wi].needs_paint = 0;
    }
    if (ps) { memset(ps, 0, 64); *(uint32_t *)ps = hdc; }   /* PAINTSTRUCT.hdc @0 */
    return hdc;
}
/* EndPaint(hwnd, const PAINTSTRUCT*) -> BOOL. Present the window, release the DC. */
uint32_t aret_EndPaint(uint32_t esp) {
    const uint8_t *ps = (const uint8_t *)WP(1);
#ifdef ARET_HAVE_SDL
    { int wi = u32_win_idx(WU(0));
      if (wi >= 0) { if (g_u32_win[wi].parent == 0) u32_present_toplevel(esp, wi);   /* compose children over the just-painted client */
                     else sdl_window_present(wi); } }   /* a child's own EndPaint: no children, plain present */
#endif
    if (ps) { int i = gdi_idx(*(const uint32_t *)ps); if (i >= 0) g_gdi[i].used = 0; }
    return 1;
}
uint32_t aret_GdiFlush(uint32_t esp) { (void)esp; return 1; }   /* writes are immediate */

/* ---- bitmaps ---- */
/* CreateDIBSection(hdc, pbmi, usage, ppvBits, hSection, offset) -> HBITMAP.
 * 32bpp BI_RGB only (the exactly-reproducible case); other depths abort sound. */
uint32_t aret_CreateDIBSection(uint32_t esp) {
    const uint8_t *bmi = (const uint8_t *)WP(1);
    uint32_t *ppv = (uint32_t *)WP(3);
    if (ppv) *ppv = 0;
    if (!bmi) return 0;
    int32_t bw = *(const int32_t *)(bmi + 4);
    int32_t bh = *(const int32_t *)(bmi + 8);
    uint16_t bpp = *(const uint16_t *)(bmi + 14);
    if (bpp != 32) { aret_partial("CreateDIBSection: only 32bpp BI_RGB modelled"); return 0; }
    int w = bw, h = bh < 0 ? -bh : bh, td = bh < 0;
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = td; g_gdi[i].bpp = 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    if (ppv) *ppv = (uint32_t)(uintptr_t)g_gdi[i].bits;
    return gdi_handle(i);
}
/* GetDIBits(hdc, hbmp, uStartScan, cScanLines, lpvBits, lpbi, uUsage): read a
 * bitmap's pixels into a DIB buffer, or (lpvBits NULL) describe it. Our bitmaps
 * are 32bpp [B,G,R,0] internally, so a 32bpp read is a per-scanline copy honouring
 * the output orientation (biHeight sign) against the source's top-down/bottom-up.
 * Measured vs Wine: 32bpp query reports BI_BITFIELDS(3) and returns the height;
 * copy returns the scanline count. Non-32bpp output -> sound abort. */
uint32_t aret_GetDIBits(uint32_t esp) {
    uint32_t hbmp = WU(1), start = WU(2), lines = WU(3);
    uint8_t *bits = (uint8_t *)WP(4);
    uint8_t *bmi = (uint8_t *)WP(5);
    int b = gdi_idx(hbmp);
    if (b < 0 || g_gdi[b].type != GDIT_BITMAP || !g_gdi[b].bits || !bmi) { g_last_error = 6u; return 0; }
    struct gdi_obj *bm = &g_gdi[b];
    int32_t *biW = (int32_t *)(bmi + 4);
    int32_t *biH = (int32_t *)(bmi + 8);
    uint16_t *biPlanes = (uint16_t *)(bmi + 12);
    uint16_t *biBpp = (uint16_t *)(bmi + 14);
    uint32_t *biComp = (uint32_t *)(bmi + 16);
    uint32_t *biSizeImg = (uint32_t *)(bmi + 20);
    if (!bits) {
        /* query mode (biBitCount 0 = "describe the bitmap"). */
        *biW = bm->w; *biH = bm->h; *biPlanes = 1; *biBpp = 32;
        *biComp = 3u /*BI_BITFIELDS — matches Wine for 32bpp*/;
        *biSizeImg = (uint32_t)(bm->w * bm->h * 4);
        return (uint32_t)bm->h;
    }
    if (*biBpp != 32) { aret_partial("GetDIBits: only 32bpp output modelled"); return 0; }
    int td_out = *biH < 0;                         /* negative height = top-down output */
    uint32_t copied = 0;
    for (uint32_t r = 0; r < lines; r++) {
        int scan = (int)(start + r);
        if (scan >= bm->h) break;
        int image_y = td_out ? scan : (bm->h - 1 - scan);      /* 0 = top row of the image */
        int mem_row = bm->topdown ? image_y : (bm->h - 1 - image_y);
        memcpy(bits + (size_t)r * bm->w * 4,
               bm->bits + (size_t)mem_row * bm->w * 4,
               (size_t)bm->w * 4);
        copied++;
    }
    return copied;
}
/* StretchDIBits(hdc, xD,yD,wD,hD, xS,yS,wS,hS, lpBits, lpbmi, usage, rop): draw
 * source DIB bits onto the DC surface. Modelled: 32bpp SRCCOPY, 1:1 (no stretch),
 * whole-image source — the ImageList blit path. Anything else (stretch, sub-rect,
 * other ROP/bpp) -> sound abort (never a silent partial draw). */
uint32_t aret_StretchDIBits(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    uint32_t hdc = WU(0);
    int xDest = WI(1), yDest = WI(2), wDest = WI(3), hDest = WI(4);
    int xSrc = WI(5), ySrc = WI(6), wSrc = WI(7), hSrc = WI(8);
    const uint8_t *src = (const uint8_t *)WP(9);
    const uint8_t *bmi = (const uint8_t *)WP(10);
    uint32_t rop = WU(12);
    struct gdi_obj *dst = gdi_dc_surface(hdc);
    if (!dst || !src || !bmi) return 0;
    int sw = *(const int32_t *)(bmi + 4);
    int sh_raw = *(const int32_t *)(bmi + 8);
    uint16_t sbpp = *(const uint16_t *)(bmi + 14);
    int sh = sh_raw < 0 ? -sh_raw : sh_raw, s_topdown = sh_raw < 0;
    if (sbpp != 32 || rop != 0x00CC0020u /*SRCCOPY*/ || wDest != wSrc || hDest != hSrc) {
        aret_partial("StretchDIBits: only 32bpp SRCCOPY 1:1 modelled"); return 0;
    }
    if (xSrc != 0 || ySrc != 0 || wSrc != sw || hSrc != sh) {
        aret_partial("StretchDIBits: only whole-image source modelled"); return 0;
    }
    for (int iy = 0; iy < sh; iy++) {                       /* iy = image row from the top */
        int srow = s_topdown ? iy : (sh - 1 - iy);
        const uint8_t *sp = src + (size_t)srow * sw * 4;
        for (int ix = 0; ix < sw; ix++) {
            const uint8_t *px = sp + ix * 4;
            uint32_t c = (uint32_t)px[2] | ((uint32_t)px[1] << 8) | ((uint32_t)px[0] << 16);
            gdi_put(dst, xDest + ix, yDest + iy, c);
        }
    }
    return (uint32_t)sh;
}
/* CreateCompatibleBitmap(hdc, w, h) -> HBITMAP (32bpp, not app-exposed). */
uint32_t aret_CreateCompatibleBitmap(uint32_t esp) {
    int w = WI(1), h = WI(2);
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = 1; g_gdi[i].bpp = 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    return gdi_handle(i);
}

/* ---- objects ---- */
uint32_t aret_CreateSolidBrush(uint32_t esp) {
    int i = gdi_alloc(GDIT_BRUSH); if (!i) return 0;
    g_gdi[i].color = WU(0) & 0x00FFFFFFu;
    return gdi_handle(i);
}
/* CreatePatternBrush(hbmp) -> HBRUSH holding the pattern bitmap. The pattern is
 * used by PatBlt/FillRect (pattern fills); here we retain the source bitmap so a
 * later blit can sample it. */
uint32_t aret_CreatePatternBrush(uint32_t esp) {
    int i = gdi_alloc(GDIT_BRUSH); if (!i) return 0;
    g_gdi[i].pat_bitmap = WU(0);
    return gdi_handle(i);
}
uint32_t aret_CreatePen(uint32_t esp) {
    int i = gdi_alloc(GDIT_PEN); if (!i) return 0;
    g_gdi[i].pen_style = (int)WU(0);                      /* (style, width, color) */
    g_gdi[i].pen_width = (int)WU(1);
    g_gdi[i].color = WU(2) & 0x00FFFFFFu;
    if ((int)WU(0) == 5) g_gdi[i].null_obj = 1;          /* PS_NULL */
    return gdi_handle(i);
}
/* GetStockObject(i) -> HGDIOBJ. Distinct, cached handle per stock id (opaque). */
static uint32_t g_gdi_stock[32];
static uint32_t u32_default_palette(void);   /* fwd: the 20-colour DEFAULT_PALETTE */
static uint32_t u32_stock(int id) {
    if (id < 0 || id >= 32) return 0;
    if (id == 15) return u32_default_palette();   /* DEFAULT_PALETTE */
    if (!g_gdi_stock[id]) {
        int type = (id <= 5) ? GDIT_BRUSH : (id <= 8) ? GDIT_PEN : GDIT_FONT;
        int i = gdi_alloc(type); if (!i) return 0;
        g_gdi[i].stock = 1;
        switch (id) {                                     /* stock brush colours */
        case 0: g_gdi[i].color = 0xFFFFFF; break;         /* WHITE_BRUSH */
        case 1: g_gdi[i].color = 0xC0C0C0; break;         /* LTGRAY_BRUSH */
        case 2: g_gdi[i].color = 0x808080; break;         /* GRAY_BRUSH */
        case 3: g_gdi[i].color = 0x404040; break;         /* DKGRAY_BRUSH */
        case 4: g_gdi[i].color = 0x000000; break;         /* BLACK_BRUSH */
        case 5: g_gdi[i].null_obj = 1; break;             /* NULL_BRUSH / HOLLOW_BRUSH */
        case 6: g_gdi[i].color = 0xFFFFFF; break;         /* WHITE_PEN */
        case 7: g_gdi[i].color = 0x000000; break;         /* BLACK_PEN */
        case 8: g_gdi[i].null_obj = 1; break;             /* NULL_PEN */
        /* Stock fonts: assign the LOGFONT Wine reports (measured via GetObject).
         * The sans ones resolve (via u32_face_subst) to Liberation Sans and render
         * bit-exactly like Wine; DEFAULT_GUI_FONT is by far the most used. Quality
         * DEFAULT (0) → subpixel. Fixed-pitch/OEM stocks keep no face (sound abort). */
        case 12: /* ANSI_VAR_FONT   */ strcpy(g_gdi[i].lf_face, "MS Sans Serif"); g_gdi[i].lf_height = 12; g_gdi[i].lf_weight = 400; break;
        case 17: /* DEFAULT_GUI_FONT*/ strcpy(g_gdi[i].lf_face, "MS Shell Dlg");  g_gdi[i].lf_height = -11; g_gdi[i].lf_weight = 400; break;
        /* SYSTEM_FONT/SYSTEM_FIXED/OEM/ANSI_FIXED render as real Windows BITMAP fonts in
         * Wine — measured: SYSTEM_FONT reports LOGFONT {'System', height 16, weight 700}
         * and GetTextFace 'Liberation Sans', but its glyphs are the compact System.fon
         * bitmap (extent 'System Fn' = 60px), NOT Liberation Sans Bold (which the same
         * FreeType path renders 72px wide). Matching them needs Wine's bitmap font, not
         * a TrueType resolve → keep no face, abort soundly rather than mis-render. */
        default: break;
        }
        g_gdi_stock[id] = gdi_handle(i);
    }
    return g_gdi_stock[id];
}
uint32_t aret_GetStockObject(uint32_t esp) { return u32_stock(WI(0)); }
/* A new DC selects the Windows default objects (so SelectObject round-trips a
 * default back like Wine): SYSTEM_FONT, WHITE_BRUSH, BLACK_PEN. */
static void u32_dc_defaults(int d) {
    if (d < 0) return;
    g_gdi[d].sel_font  = u32_stock(13);   /* SYSTEM_FONT */
    g_gdi[d].sel_brush = u32_stock(0);    /* WHITE_BRUSH */
    g_gdi[d].sel_pen   = u32_stock(7);    /* BLACK_PEN */
    g_gdi[d].mapmode   = 1;               /* MM_TEXT */
    g_gdi[d].vp_ox = g_gdi[d].vp_oy = g_gdi[d].win_ox = g_gdi[d].win_oy = 0;
    g_gdi[d].vp_ex = g_gdi[d].vp_ey = g_gdi[d].win_ex = g_gdi[d].win_ey = 1; /* identity */
}
/* SaveDC(hdc) -> level. RestoreDC(hdc, level) -> BOOL. Get/SetMapMode. GetClipBox. */
uint32_t aret_SaveDC(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC || g_gdi[d].savetop >= 8) return 0;
    int t = g_gdi[d].savetop;
    g_gdi[d].sstk[t].font = g_gdi[d].sel_font; g_gdi[d].sstk[t].brush = g_gdi[d].sel_brush;
    g_gdi[d].sstk[t].pen = g_gdi[d].sel_pen;   g_gdi[d].sstk[t].tc = g_gdi[d].text_color;
    g_gdi[d].sstk[t].bc = g_gdi[d].bk_color;   g_gdi[d].sstk[t].bm = g_gdi[d].bk_mode;
    g_gdi[d].sstk[t].mm = g_gdi[d].mapmode;
    return (uint32_t)(++g_gdi[d].savetop);
}
uint32_t aret_RestoreDC(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int level = WI(1);
    int idx = level < 0 ? g_gdi[d].savetop + level : level - 1;
    if (idx < 0 || idx >= g_gdi[d].savetop) return 0;
    g_gdi[d].sel_font = g_gdi[d].sstk[idx].font; g_gdi[d].sel_brush = g_gdi[d].sstk[idx].brush;
    g_gdi[d].sel_pen = g_gdi[d].sstk[idx].pen;   g_gdi[d].text_color = g_gdi[d].sstk[idx].tc;
    g_gdi[d].bk_color = g_gdi[d].sstk[idx].bc;   g_gdi[d].bk_mode = g_gdi[d].sstk[idx].bm;
    g_gdi[d].mapmode = g_gdi[d].sstk[idx].mm;
    g_gdi[d].savetop = idx;
    return 1;
}
uint32_t aret_SetMapMode(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    int p = g_gdi[d].mapmode, m = WI(1);
    if (m == 1) {                          /* MM_TEXT: 1:1 (y down) */
        g_gdi[d].vp_ex = g_gdi[d].vp_ey = g_gdi[d].win_ex = g_gdi[d].win_ey = 1;
    } else if (m != 8) {                   /* MM_ANISOTROPIC: app controls extents */
        aret_partial("SetMapMode: only MM_TEXT/MM_ANISOTROPIC modelled (metric/isotropic pending)");
        return 0;
    }
    g_gdi[d].mapmode = m;
    return (uint32_t)p;
}
uint32_t aret_GetMapMode(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0 : (uint32_t)g_gdi[d].mapmode; }
/* Mapping-mode transform state (device = (log-winOrg)*vpExt/winExt + vpOrg).
 * *OrgEx apply in every mode; *ExtEx only in MM_ISOTROPIC/ANISOTROPIC. The *Ex
 * out-params receive the previous value (POINT for org, SIZE for extent). */
static void u32_put_pt(uint32_t p, int x, int y) { if (p) { int32_t *o = (int32_t *)(uintptr_t)p; o[0] = x; o[1] = y; } }
static int u32_ext_mode(int d) { return g_gdi[d].mapmode == 7 || g_gdi[d].mapmode == 8; }
uint32_t aret_SetViewportOrgEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].vp_ox, g_gdi[d].vp_oy);
    g_gdi[d].vp_ox = WI(1); g_gdi[d].vp_oy = WI(2); return 1;
}
uint32_t aret_GetViewportOrgEx(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0; u32_put_pt(WU(1), g_gdi[d].vp_ox, g_gdi[d].vp_oy); return 1; }
uint32_t aret_SetWindowOrgEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].win_ox, g_gdi[d].win_oy);
    g_gdi[d].win_ox = WI(1); g_gdi[d].win_oy = WI(2); return 1;
}
uint32_t aret_GetWindowOrgEx(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0; u32_put_pt(WU(1), g_gdi[d].win_ox, g_gdi[d].win_oy); return 1; }
uint32_t aret_SetViewportExtEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].vp_ex, g_gdi[d].vp_ey);
    if (u32_ext_mode(d)) { g_gdi[d].vp_ex = WI(1); g_gdi[d].vp_ey = WI(2); } return 1;
}
uint32_t aret_GetViewportExtEx(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0; u32_put_pt(WU(1), g_gdi[d].vp_ex, g_gdi[d].vp_ey); return 1; }
uint32_t aret_SetWindowExtEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].win_ex, g_gdi[d].win_ey);
    if (u32_ext_mode(d)) { g_gdi[d].win_ex = WI(1); g_gdi[d].win_ey = WI(2); } return 1;
}
uint32_t aret_GetWindowExtEx(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0; u32_put_pt(WU(1), g_gdi[d].win_ex, g_gdi[d].win_ey); return 1; }
uint32_t aret_OffsetViewportOrgEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].vp_ox, g_gdi[d].vp_oy);
    g_gdi[d].vp_ox += WI(1); g_gdi[d].vp_oy += WI(2); return 1;
}
uint32_t aret_OffsetWindowOrgEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(3), g_gdi[d].win_ox, g_gdi[d].win_oy);
    g_gdi[d].win_ox += WI(1); g_gdi[d].win_oy += WI(2); return 1;
}
uint32_t aret_ScaleViewportExtEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(5), g_gdi[d].vp_ex, g_gdi[d].vp_ey);
    if (u32_ext_mode(d)) { g_gdi[d].vp_ex = gdi_muldiv(g_gdi[d].vp_ex, WI(1), WI(2));
                           g_gdi[d].vp_ey = gdi_muldiv(g_gdi[d].vp_ey, WI(3), WI(4)); } return 1;
}
uint32_t aret_ScaleWindowExtEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    u32_put_pt(WU(5), g_gdi[d].win_ex, g_gdi[d].win_ey);
    if (u32_ext_mode(d)) { g_gdi[d].win_ex = gdi_muldiv(g_gdi[d].win_ex, WI(1), WI(2));
                           g_gdi[d].win_ey = gdi_muldiv(g_gdi[d].win_ey, WI(3), WI(4)); } return 1;
}
uint32_t aret_LPtoDP(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    int32_t *p = (int32_t *)WP(1); int n = WI(2);
    for (int i = 0; i < n; i++) { int dx, dy; dc_l2d(d, p[2*i], p[2*i+1], &dx, &dy); p[2*i] = dx; p[2*i+1] = dy; }
    return 1;
}
uint32_t aret_DPtoLP(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    int32_t *p = (int32_t *)WP(1); int n = WI(2);
    for (int i = 0; i < n; i++) { int lx, ly; dc_d2l(d, p[2*i], p[2*i+1], &lx, &ly); p[2*i] = lx; p[2*i+1] = ly; }
    return 1;
}
uint32_t aret_GetClipBox(uint32_t esp) {
    int32_t *r = (int32_t *)WP(1); if (!r) return 0;   /* ERROR */
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (bm) { r[0] = 0; r[1] = 0; r[2] = bm->w; r[3] = bm->h; }
    else    { r[0] = 0; r[1] = 0; r[2] = U32_SCREEN_W; r[3] = U32_SCREEN_H; }
    return 2;   /* SIMPLEREGION */
}
/* SelectObject(hdc, hgdiobj) -> previous object of that kind. */
uint32_t aret_SelectObject(uint32_t esp) {
    int d = gdi_idx(WU(0)), o = gdi_idx(WU(1));
    if (d < 0 || g_gdi[d].type != GDIT_DC || o < 0) return 0;
    uint32_t h = WU(1), prev = 0;
    switch (g_gdi[o].type) {
    case GDIT_BITMAP: prev = g_gdi[d].sel_bitmap; g_gdi[d].sel_bitmap = h; break;
    case GDIT_BRUSH:  prev = g_gdi[d].sel_brush;  g_gdi[d].sel_brush = h;  break;
    case GDIT_PEN:    prev = g_gdi[d].sel_pen;    g_gdi[d].sel_pen = h;    break;
    case GDIT_FONT:   prev = g_gdi[d].sel_font;   g_gdi[d].sel_font = h;   break;
    default: return 0;
    }
    return prev;
}
/* DeleteObject(hgdiobj) -> BOOL. Frees owned bits; stock objects are never freed. */
uint32_t aret_DeleteObject(uint32_t esp) {
    int i = gdi_idx(WU(0));
    if (i < 0) return 0;
    if (g_gdi[i].stock) return 1;
    if (g_gdi[i].owns_bits && g_gdi[i].bits) { free(g_gdi[i].bits); g_gdi[i].bits = NULL; }
    if (g_gdi[i].pal) { free(g_gdi[i].pal); g_gdi[i].pal = NULL; }
    g_gdi[i].used = 0;
    return 1;
}

/* Resolve a FillRect "brush": a real brush handle, or a system-colour index+1
 * ((HBRUSH)(COLOR_x+1)). Returns 1 if a colour was produced, 0 for a null brush. */
static uint32_t u32_syscolor(int i);
static int gdi_brush_color(uint32_t hb, uint32_t *out) {
    int b = gdi_idx(hb);
    if (b >= 0 && g_gdi[b].type == GDIT_BRUSH) { if (g_gdi[b].null_obj) return 0; *out = g_gdi[b].color; return 1; }
    if (hb >= 1 && hb <= 31) { *out = u32_syscolor((int)hb - 1); return 1; }  /* COLOR_x + 1 */
    return 0;
}
/* Fill a DC's whole surface with a brush colour — the default WM_ERASEBKGND. */
static void u32_fill_dc_brush(uint32_t hdc, uint32_t brush) {
    struct gdi_obj *bm = gdi_dc_surface(hdc);
    uint32_t c;
    if (!bm || !gdi_brush_color(brush, &c)) return;      /* no surface / null brush */
    for (int y = 0; y < bm->h; y++)
        for (int x = 0; x < bm->w; x++) gdi_put(bm, x, y, c);
}
/* Fill a window's client framebuffer with its class background brush — the default
 * WM_ERASEBKGND, done directly on the bitmap (used by DefWindowProc's default paint). */
static void u32_erase_window_client(int wi) {
#ifdef ARET_HAVE_SDL
    if (wi < 0 || !g_u32_win[wi].bg_brush) return;
    int b = gdi_idx(g_u32_win[wi].client_bmp); if (b < 0) return;
    uint32_t c; if (!gdi_brush_color(g_u32_win[wi].bg_brush, &c)) return;
    struct gdi_obj *bm = &g_gdi[b];
    for (int y = 0; y < bm->h; y++)
        for (int x = 0; x < bm->w; x++) gdi_put(bm, x, y, c);
#else
    (void)wi;
#endif
}

/* ---- drawing (bit-exact on the offscreen DIB) ---- */
/* SetPixel(hdc, x, y, color) -> COLORREF set (or CLR_INVALID). */
uint32_t aret_SetPixel(uint32_t esp) {
    int d = gdi_idx(WU(0)); struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm || d < 0) return 0xFFFFFFFFu;
    int x, y; dc_l2d(d, WI(1), WI(2), &x, &y);
    uint32_t c = WU(3) & 0x00FFFFFFu;
    if (!gdi_px(bm, x, y)) return 0xFFFFFFFFu;
    gdi_put(bm, x, y, c);
    return c;
}
/* SetPixelV(hdc, x, y, color) -> BOOL. */
uint32_t aret_SetPixelV(uint32_t esp) {
    int d = gdi_idx(WU(0)); struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm || d < 0) return 0;
    int x, y; dc_l2d(d, WI(1), WI(2), &x, &y);
    if (!gdi_px(bm, x, y)) return 0;
    gdi_put(bm, x, y, WU(3) & 0x00FFFFFFu);
    return 1;
}
/* GetPixel(hdc, x, y) -> COLORREF (or CLR_INVALID). */
uint32_t aret_GetPixel(uint32_t esp) {
    int d = gdi_idx(WU(0)); struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm || d < 0) return 0xFFFFFFFFu;
    int x, y; dc_l2d(d, WI(1), WI(2), &x, &y);
    return gdi_getpx(bm, x, y);
}

/* The DC's currently-selected pen: fills *color (COLORREF) and returns 1 if it is
 * a solid width≤1 pen we can draw exactly (Bresenham). Returns 0 for NULL_PEN (no
 * draw) and aborts soundly for wide/styled pens (their exact rasterisation is a
 * follow-up). */
static int gdi_pen(int d, uint32_t *color) {
    int p = gdi_idx(g_gdi[d].sel_pen);
    if (p < 0) { if (color) *color = 0; return 1; }        /* default black */
    if (g_gdi[p].null_obj) return 0;                        /* NULL_PEN: draws nothing */
    if (!g_gdi[p].stock && (g_gdi[p].pen_style != 0 || g_gdi[p].pen_width > 1)) {
        aret_partial("GDI pen: only PS_SOLID width<=1 modelled (styled/wide pens pending)");
        return -1;
    }
    if (color) *color = g_gdi[p].color;
    return 1;
}
/* Bresenham line from (x0,y0) to (x1,y1), *excluding* the endpoint (GDI LineTo
 * semantics), in the pen colour. Matches Wine's integer line rasteriser (verified
 * against measured output). */
static void gdi_bres(struct gdi_obj *bm, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int x = x0, y = y0;
    if (adx >= ady) {                                       /* x-major */
        int err = 2 * ady - adx;
        for (int i = 0; i < adx; i++) {                     /* endpoint excluded */
            if (gdi_px(bm, x, y)) gdi_put(bm, x, y, c);
            if (err > 0) { y += sy; err -= 2 * adx; }
            err += 2 * ady; x += sx;
        }
    } else {                                                /* y-major */
        int err = 2 * adx - ady;
        for (int i = 0; i < ady; i++) {
            if (gdi_px(bm, x, y)) gdi_put(bm, x, y, c);
            if (err > 0) { x += sx; err -= 2 * ady; }
            err += 2 * adx; y += sy;
        }
    }
}
/* MoveToEx(hdc, x, y, LPPOINT old) -> BOOL. Sets the current position. */
uint32_t aret_MoveToEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    uint32_t po = WU(3);
    if (po) { int32_t *pt = (int32_t *)(uintptr_t)po; pt[0] = g_gdi[d].cur_x; pt[1] = g_gdi[d].cur_y; }
    g_gdi[d].cur_x = WI(1); g_gdi[d].cur_y = WI(2);
    return 1;
}
uint32_t aret_GetCurrentPositionEx(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0) return 0;
    uint32_t po = WU(1);
    if (po) { int32_t *pt = (int32_t *)(uintptr_t)po; pt[0] = g_gdi[d].cur_x; pt[1] = g_gdi[d].cur_y; }
    return 1;
}
/* LineTo(hdc, x, y) -> BOOL. Draws from the current position to (x,y) (endpoint
 * excluded) with the selected pen, then moves the current position to (x,y). */
uint32_t aret_LineTo(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    uint32_t c; int pr = gdi_pen(d, &c);
    if (pr < 0) return 0;                                   /* sound abort already raised */
    if (pr && bm && bm->bpp == 32) {
        int x0, y0, x1, y1;                                 /* transform logical -> device */
        dc_l2d(d, g_gdi[d].cur_x, g_gdi[d].cur_y, &x0, &y0);
        dc_l2d(d, WI(1), WI(2), &x1, &y1);
        gdi_bres(bm, x0, y0, x1, y1, c);
    }
    g_gdi[d].cur_x = WI(1); g_gdi[d].cur_y = WI(2);         /* current position stays LOGICAL */
    return 1;
}
/* The DC's selected brush colour; returns 0 for a NULL/HOLLOW brush (no fill). */
static int gdi_brush(int d, uint32_t *color) {
    int b = gdi_idx(g_gdi[d].sel_brush);
    if (b < 0 || g_gdi[b].null_obj) return 0;
    if (color) *color = g_gdi[b].color;
    return 1;
}
/* Polyline(hdc, POINT* pts, int count) -> BOOL. Draws count-1 connected Bresenham
 * segments with the pen (each endpoint excluded, so shared vertices are drawn once
 * by the next segment's start; the final endpoint is not drawn). Does not use or
 * update the current position. */
uint32_t aret_Polyline(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *pts = (const int32_t *)(uintptr_t)WU(1); int n = WI(2);
    uint32_t c; int pr = gdi_pen(d, &c);
    if (pr < 0) return 0;
    if (pr && bm && bm->bpp == 32 && pts && n >= 2)
        for (int i = 0; i < n - 1; i++) {
            int x0, y0, x1, y1;
            dc_l2d(d, pts[2 * i], pts[2 * i + 1], &x0, &y0);
            dc_l2d(d, pts[2 * i + 2], pts[2 * i + 3], &x1, &y1);
            gdi_bres(bm, x0, y0, x1, y1, c);
        }
    return 1;
}
/* Rectangle(hdc, l, t, r, b) -> BOOL. Interior [l+1,r-1)×[t+1,b-1) filled with the
 * brush; outline [l,r-1]×[t,b-1] drawn with the pen (measured Wine semantics). */
uint32_t aret_Rectangle(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    int l, t, r, b;                                        /* transform corners logical -> device */
    dc_l2d(d, WI(1), WI(2), &l, &t);
    dc_l2d(d, WI(3), WI(4), &r, &b);
    uint32_t pc; int pr = gdi_pen(d, &pc);
    if (pr < 0) return 0;
    uint32_t bc; int hf = gdi_brush(d, &bc);
    if (bm && bm->bpp == 32 && r > l && b > t) {
        if (hf)
            for (int y = t + 1; y < b - 1; y++)
                for (int x = l + 1; x < r - 1; x++)
                    if (gdi_px(bm, x, y)) gdi_put(bm, x, y, bc);
        if (pr) {
            int r1 = r - 1, b1 = b - 1;
            for (int x = l; x <= r1; x++) { if (gdi_px(bm, x, t))  gdi_put(bm, x, t,  pc);
                                            if (gdi_px(bm, x, b1)) gdi_put(bm, x, b1, pc); }
            for (int y = t; y <= b1; y++) { if (gdi_px(bm, l, y))  gdi_put(bm, l,  y, pc);
                                            if (gdi_px(bm, r1, y)) gdi_put(bm, r1, y, pc); }
        }
    }
    return 1;
}
/* FrameRect(hdc, const RECT*, hbrush) -> int. Draws a 1px border on the outline
 * [l,r-1]x[t,b-1] (same bounds as Rectangle's outline) with the *argument* brush's
 * colour (measured on Wine — it uses the brush passed in, not the selected one).
 * Null brush -> nothing. */
uint32_t aret_FrameRect(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    uint32_t c;
    if (!gdi_brush_color(WU(2), &c)) return 1;            /* null brush: nothing */
    int l = r[0], t = r[1], rr = r[2], b = r[3];
    if (bm->bpp == 32 && rr > l && b > t) {
        int r1 = rr - 1, b1 = b - 1;
        for (int x = l; x <= r1; x++) { if (gdi_px(bm, x, t))  gdi_put(bm, x, t,  c);
                                        if (gdi_px(bm, x, b1)) gdi_put(bm, x, b1, c); }
        for (int y = t; y <= b1; y++) { if (gdi_px(bm, l, y))  gdi_put(bm, l,  y, c);
                                        if (gdi_px(bm, r1, y)) gdi_put(bm, r1, y, c); }
    }
    return 1;
}
/* InvertRect(hdc, const RECT*) -> BOOL. XORs every pixel (all 32 bits, incl. the
 * unused alpha byte — measured identical to DSTINVERT on Wine) over [l,r)x[t,b). */
uint32_t aret_InvertRect(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    for (int y = r[1]; y < r[3]; y++)
        for (int x = r[0]; x < r[2]; x++) {
            uint8_t *p = gdi_px(bm, x, y); if (!p) continue;
            p[0] = ~p[0]; p[1] = ~p[1]; p[2] = ~p[2]; p[3] = ~p[3];
        }
    return 1;
}
/* DrawFocusRect(hdc, rect): a 1px DOTTED outline of the rect, XOR-inverted, where a
 * border pixel (x,y) is a dot iff (x+y) is odd (absolute-coord parity, so adjacent
 * focus rects tile seamlessly — measured vs Wine). Single pass over the outline so a
 * corner is never XOR'd twice. Theme-independent (pure XOR of the existing pixels). */
uint32_t aret_DrawFocusRect(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    int l = r[0], t = r[1], rt = r[2], b = r[3];
    if (rt <= l || b <= t) return 1;
#define FR_DOT(X, Y) do { if ((((X) + (Y)) & 1)) { uint8_t *p = gdi_px(bm, (X), (Y)); if (p) { p[0] = ~p[0]; p[1] = ~p[1]; p[2] = ~p[2]; } } } while (0)
    for (int x = l; x < rt; x++) { FR_DOT(x, t); FR_DOT(x, b - 1); }   /* top + bottom rows */
    for (int y = t + 1; y < b - 1; y++) { FR_DOT(l, y); FR_DOT(rt - 1, y); }  /* side cols (corners already done) */
#undef FR_DOT
    return 1;
}
/* DrawEdge(hdc, rect, edge, flags): the 3D bevel every button/group-box/edit-border
 * uses. Two rings; a pixel takes the "bottom-right" colour when it is on the right col
 * or bottom row of its ring, else the "top-left" colour (measured vs Wine — the dark
 * bottom/right lines win at the TR/BL corners). Colour INDICES (not raw RGB) match
 * Wine; the values come from GetSysColor, so ARET's classic scheme and Wine's theme
 * both render the same *structure* (verified index-wise, theme-independent). Only the
 * measured EDGE_RAISED/EDGE_SUNKEN with BF_RECT (+optional BF_MIDDLE fill) are modelled;
 * other edge/flag combos abort soundly. */
/* Shared 3D-edge painter. `edge` = EDGE_RAISED(0x5)/EDGE_SUNKEN(0xA); `flags` = BF_RECT
 * (+BF_MIDDLE fill, +BF_SOFT). BF_SOFT swaps the light-side indices (3DLIGHT<->BTNHIGHLIGHT)
 * — the "soft" bevel a push button uses. Returns 0 (with a sound abort) outside the
 * modelled subset. */
static int u32_drawedge(struct gdi_obj *bm, const int32_t *r, uint32_t edge, uint32_t flags) {
    if ((flags & ~0x180Fu) || (flags & 0xFu) != 0xFu) { aret_partial("DrawEdge: only BF_RECT(+BF_MIDDLE/BF_SOFT) modelled"); return 0; }
    int lo, bo, li, bi;                        /* sys-colour indices: outer LT/BR, inner LT/BR */
    if (edge == 0x5)      { lo = 22; bo = 21; li = 20; bi = 16; }   /* EDGE_RAISED */
    else if (edge == 0xA) { lo = 16; bo = 20; li = 21; bi = 22; }   /* EDGE_SUNKEN */
    else if (edge == 0x6) { lo = 16; bo = 20; li = 20; bi = 16; }   /* EDGE_ETCHED (group box) — measured */
    else { aret_partial("DrawEdge: only EDGE_RAISED/SUNKEN/ETCHED modelled"); return 0; }
    if ((flags & 0x1000u) && edge == 0x5) { int tmp = lo; lo = li; li = tmp; }   /* BF_SOFT (raised) */
    int l = r[0], t = r[1], rt = r[2], b = r[3];
    if (rt - l < 2 || b - t < 2) return 1;
    uint32_t clo = u32_syscolor(lo), cbo = u32_syscolor(bo), cli = u32_syscolor(li), cbi = u32_syscolor(bi);
    for (int y = t; y < b; y++)
        for (int x = l; x < rt; x++) {
            int ring;
            if (x == l || x == rt - 1 || y == t || y == b - 1) ring = 0;
            else if (x == l + 1 || x == rt - 2 || y == t + 1 || y == b - 2) ring = 1;
            else continue;
            int br = ring == 0 ? (x == rt - 1 || y == b - 1) : (x == rt - 2 || y == b - 2);
            gdi_put(bm, x, y, ring == 0 ? (br ? cbo : clo) : (br ? cbi : cli));
        }
    if (flags & 0x800u) {                      /* BF_MIDDLE: fill the interior with 3DFACE */
        uint32_t face = u32_syscolor(15);
        for (int y = t + 2; y < b - 2; y++) for (int x = l + 2; x < rt - 2; x++) gdi_put(bm, x, y, face);
    }
    return 1;
}
uint32_t aret_DrawEdge(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    return (uint32_t)u32_drawedge(bm, r, WU(2), WU(3));
}
/* DrawFrameControl(hdc, rect, type, state): the composite control frames. Modelled:
 * DFC_BUTTON + DFCS_BUTTONPUSH (a normal push button = a soft-raised bevel filled with
 * 3DFACE, measured vs Wine = DrawEdge(EDGE_RAISED, BF_SOFT|BF_RECT|BF_MIDDLE)). Pushed
 * buttons, check/radio boxes, caption/menu/scroll glyphs abort soundly (each a measured
 * follow-up — never a wrong frame). */
static void u32_draw_check_glyph(struct gdi_obj *bm, int x, int y, int checked);   /* fwd */
static void u32_draw_radio_glyph(struct gdi_obj *bm, int x, int y, int checked);   /* fwd */
uint32_t aret_DrawFrameControl(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    uint32_t type = WU(2), state = WU(3);
    if (!bm || !r) return 0;
    if (type == 4 /*DFC_BUTTON*/) {
        uint32_t bt = state & 0xFFu;
        if (bt == 0x10u /*DFCS_BUTTONPUSH*/ && !(state & 0x200u /*DFCS_PUSHED*/))
            return (uint32_t)u32_drawedge(bm, r, 0x5 /*EDGE_RAISED*/, 0xF | 0x800u | 0x1000u /*BF_RECT|BF_MIDDLE|BF_SOFT*/);
        if (bt == 0x00u /*DFCS_BUTTONCHECK*/ && (r[2] - r[0]) == 13 && (r[3] - r[1]) == 13) {
            u32_draw_check_glyph(bm, r[0], r[1], (state & 0x400u /*DFCS_CHECKED*/) ? 1 : 0);
            return 1;
        }
        if (bt == 0x04u /*DFCS_BUTTONRADIO*/ && (r[2] - r[0]) == 13 && (r[3] - r[1]) == 13) {
            u32_draw_radio_glyph(bm, r[0], r[1], (state & 0x400u /*DFCS_CHECKED*/) ? 1 : 0);
            return 1;
        }
    }
    aret_partial("DrawFrameControl: only DFC_BUTTON push / 13x13 check|radio modelled");
    return 0;
}

static uint32_t u32_ansi_cp(unsigned char b);   /* fwd: ANSI byte -> codepoint (CP1252) */
static uint32_t u32_drawtext(uint32_t hdc, const uint32_t *cps, int len, uint32_t prc, uint32_t fmt); /* fwd */
static int u32_drawedge(struct gdi_obj *bm, const int32_t *r, uint32_t edge, uint32_t flags);   /* fwd */
/* Draw a 13x13 check box glyph at (x,y): a sunken edge + white field, plus the Marlett
 * check mark (measured pixel-exact from Wine's DrawFrameControl) when `checked`. The box
 * itself is EDGE_SUNKEN|BF_RECT (bit-identical to Wine's UITOOLS check). */
static void u32_draw_check_glyph(struct gdi_obj *bm, int x, int y, int checked) {
    int32_t r[4] = { x, y, x + 13, y + 13 };
    u32_drawedge(bm, r, 0x0Au /*EDGE_SUNKEN*/, 0xFu /*BF_RECT*/);
    uint32_t win = u32_syscolor(5 /*COLOR_WINDOW*/);
    for (int yy = 2; yy < 11; yy++) for (int xx = 2; xx < 11; xx++) gdi_put(bm, x + xx, y + yy, win);
    if (checked) {
        /* Marlett tick (21 px), measured from Wine at 13x13, in COLOR_WINDOWTEXT. */
        static const signed char pts[][2] = {
            {9,3},{8,4},{9,4},{7,5},{8,5},{9,5},{3,6},{6,6},{7,6},{8,6},
            {3,7},{4,7},{5,7},{6,7},{7,7},{3,8},{4,8},{5,8},{6,8},{4,9},{5,9} };
        uint32_t tx = u32_syscolor(8 /*COLOR_WINDOWTEXT*/);
        for (unsigned i = 0; i < sizeof pts / sizeof pts[0]; i++) gdi_put(bm, x + pts[i][0], y + pts[i][1], tx);
    }
}
/* Draw a 13x13 radio-button glyph at (x,y): a small bevelled circle with a white field,
 * plus the centre dot when `checked`. The whole glyph is a FIXED bitmap (measured pixel-
 * exact from Wine's DrawFrameControl), so it is bit-exact despite being "curved". */
static void u32_draw_radio_glyph(struct gdi_obj *bm, int x, int y, int checked) {
    /* S=BTNSHADOW K=3DDKSHADOW H=BTNHIGHLIGHT(white field) L=3DLIGHT  .=leave (outside) */
    static const char *g[13] = {
        ".............", ".....SSSS....", "...SSKKKKSS..", "..SSKHHHHK.H.",
        "..SKHHHHHHLH.", ".SKHHHHHHHHLH", ".SKHHHHHHHHLH", ".SKHHHHHHHHLH",
        ".SKHHHHHHHHLH", "..SKHHHHHHLH.", "..HHLHHHHLHH.", "...HHLLLLHH..",
        ".....HHHH...." };
    for (int r = 0; r < 13; r++) for (int c = 0; c < 13; c++) {
        int idx;
        switch (g[r][c]) {
            case 'S': idx = 16; break; case 'K': idx = 21; break;
            case 'H': idx = 20; break; case 'L': idx = 22; break;
            default: continue;
        }
        gdi_put(bm, x + c, y + r, u32_syscolor(idx));
    }
    if (checked) {
        static const signed char dot[][2] = {
            {6,5},{7,5},{5,6},{6,6},{7,6},{8,6},{5,7},{6,7},{7,7},{8,7},{6,8},{7,8} };
        uint32_t tx = u32_syscolor(8 /*COLOR_WINDOWTEXT*/);
        for (unsigned i = 0; i < sizeof dot / sizeof dot[0]; i++) gdi_put(bm, x + dot[i][0], y + dot[i][1], tx);
    }
}
/* Paint a predefined BUTTON control into `hdc` (its own client area, origin 0,0): the
 * soft-raised push-button frame + its caption centred in the control's font (COLOR_BTNTEXT,
 * transparent). Measured vs Wine (WM_PRINTCLIENT). The exact caption pixels depend on the
 * resolved font face (same env caveat as gdi_uifont), so a fixture verifies the frame
 * structurally + that caption pixels exist. */
static void u32_button_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc);
    int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    int32_t rc[4] = { 0, 0, w, h };
    u32_drawedge(bm, rc, 0x5 /*EDGE_RAISED*/, 0xF | 0x800u | 0x1000u);   /* frame + 3DFACE fill */
    const char *cap = g_u32_win[wi].title;
    if (cap && cap[0] && g_u32_win[wi].ctrl_font) {
        uint32_t sf = g_gdi[d].sel_font, tc = g_gdi[d].text_color; int bk = g_gdi[d].bk_mode;
        g_gdi[d].sel_font = g_u32_win[wi].ctrl_font;
        g_gdi[d].text_color = u32_syscolor(18) /*COLOR_BTNTEXT*/;
        g_gdi[d].bk_mode = 1 /*TRANSPARENT*/;
        uint32_t cps[256]; int m = 0; for (; cap[m] && m < 255; m++) cps[m] = u32_ansi_cp((unsigned char)cap[m]);
        int32_t r2[4] = { 0, 0, w, h };
        u32_drawtext(hdc, cps, m, (uint32_t)(uintptr_t)r2, 0x1u | 0x4u | 0x20u /*DT_CENTER|DT_VCENTER|DT_SINGLELINE*/);
        g_gdi[d].sel_font = sf; g_gdi[d].text_color = tc; g_gdi[d].bk_mode = bk;
    }
}
/* Draw `str` in `font` into `rect`, COLOR `idx`, transparent, `fmt` = DrawText flags. */
static void u32_paint_text(uint32_t hdc, int d, uint32_t font, const char *str, const int32_t *rect, uint32_t fmt, int idx) {
    if (!str || !str[0] || !font) return;
    uint32_t sf = g_gdi[d].sel_font, tc = g_gdi[d].text_color; int bk = g_gdi[d].bk_mode;
    g_gdi[d].sel_font = font;
    g_gdi[d].text_color = u32_syscolor(idx);
    g_gdi[d].bk_mode = 1 /*TRANSPARENT*/;
    uint32_t cps[256]; int m = 0; for (; str[m] && m < 255; m++) cps[m] = u32_ansi_cp((unsigned char)str[m]);
    u32_drawtext(hdc, cps, m, (uint32_t)(uintptr_t)rect, fmt);
    g_gdi[d].sel_font = sf; g_gdi[d].text_color = tc; g_gdi[d].bk_mode = bk;
}
/* Draw a control's own caption (the window title) with the control font. Shared by
 * STATIC/EDIT/BUTTON caption painting. */
static void u32_ctrl_text(uint32_t hdc, int d, int wi, const int32_t *rect, uint32_t fmt, int idx) {
    u32_paint_text(hdc, d, g_u32_win[wi].ctrl_font, g_u32_win[wi].title, rect, fmt, idx);
}
/* LISTBOX/COMBOBOX item model (heap list of strings). */
static void u32_items_free(int i) {
    if (g_u32_win[i].items) { for (int k = 0; k < g_u32_win[i].item_count; k++) free(g_u32_win[i].items[k]); free(g_u32_win[i].items); }
    g_u32_win[i].items = NULL; g_u32_win[i].item_count = 0; g_u32_win[i].item_cap = 0; g_u32_win[i].cur_sel = -1;
}
static int u32_items_insert(int i, int pos, const char *s) {
    if (g_u32_win[i].item_count >= g_u32_win[i].item_cap) {
        int nc = g_u32_win[i].item_cap ? g_u32_win[i].item_cap * 2 : 8;
        char **na = (char **)realloc(g_u32_win[i].items, (size_t)nc * sizeof(char *));
        if (!na) return -1; g_u32_win[i].items = na; g_u32_win[i].item_cap = nc;
    }
    int n = g_u32_win[i].item_count;
    if (pos < 0 || pos > n) pos = n;
    for (int k = n; k > pos; k--) g_u32_win[i].items[k] = g_u32_win[i].items[k - 1];
    size_t L = s ? strlen(s) : 0; char *dup = (char *)malloc(L + 1);
    if (!dup) return -1; if (s) memcpy(dup, s, L); dup[L] = 0;
    g_u32_win[i].items[pos] = dup; g_u32_win[i].item_count = n + 1;
    return pos;
}
static const char *u32_items_get(int i, int idx) {
    return (idx >= 0 && idx < g_u32_win[i].item_count) ? g_u32_win[i].items[idx] : NULL;
}
/* Fill a control's whole client with a system colour. */
static void u32_ctrl_fill(struct gdi_obj *bm, int w, int h, uint32_t c) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) gdi_put(bm, x, y, c);
}
/* STATIC (SS_LEFT text label): fill COLOR_3DFACE (the dialog bg, measured vs Wine) then
 * the text left/top in COLOR_WINDOWTEXT (matches Wine's WM_PRINTCLIENT). */
static void u32_static_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(15 /*COLOR_3DFACE*/));
    int32_t r2[4] = { 0, 0, w, h };
    u32_ctrl_text(hdc, d, wi, r2, 0x20u /*DT_SINGLELINE (left/top)*/, 8 /*COLOR_WINDOWTEXT*/);
}
/* EDIT client (matches WM_PRINTCLIENT — the 3D border is non-client, added by the
 * composite): fill COLOR_WINDOW then the text left/vcentre in COLOR_WINDOWTEXT.
 * `inset` shifts the text in from a composited border. */
static void u32_edit_paint(uint32_t hdc, int wi, int inset) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(5 /*COLOR_WINDOW*/));
    int32_t r2[4] = { inset, 0, w - inset, h };
    u32_ctrl_text(hdc, d, wi, r2, 0x4u | 0x20u /*DT_VCENTER|DT_SINGLELINE*/, 8 /*COLOR_WINDOWTEXT*/);
}
/* CHECKBOX / 3-STATE control: fill COLOR_3DFACE, the 13x13 check glyph left-vcentred,
 * then the label text to its right. Whole-control paint is composite-only (Wine paints
 * nothing for a checkbox via WM_PRINTCLIENT); the glyph is bit-exact (DrawFrameControl). */
static void u32_check_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(15 /*COLOR_3DFACE*/));
    int gy = (h - 13) / 2; if (gy < 0) gy = 0;
    u32_draw_check_glyph(bm, 0, gy, g_u32_win[wi].check_state ? 1 : 0);
    int32_t r2[4] = { 16, 0, w, h };
    u32_ctrl_text(hdc, d, wi, r2, 0x4u | 0x20u /*DT_VCENTER|DT_SINGLELINE*/, 8 /*COLOR_WINDOWTEXT*/);
}
/* RADIO BUTTON control: like the checkbox but with the round radio glyph. Composite-only
 * whole-control paint; the glyph primitive is bit-exact (DrawFrameControl). */
static void u32_radio_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(15 /*COLOR_3DFACE*/));
    int gy = (h - 13) / 2; if (gy < 0) gy = 0;
    u32_draw_radio_glyph(bm, 0, gy, g_u32_win[wi].check_state ? 1 : 0);
    int32_t r2[4] = { 16, 0, w, h };
    u32_ctrl_text(hdc, d, wi, r2, 0x4u | 0x20u /*DT_VCENTER|DT_SINGLELINE*/, 8 /*COLOR_WINDOWTEXT*/);
}
/* GROUP BOX (BS_GROUPBOX): fill COLOR_3DFACE, an etched frame whose top runs through the
 * caption row, and the caption at top-left drawn OPAQUE (COLOR_3DFACE background) so it
 * breaks the top border line — the classic labelled frame. Whole-control paint is
 * composite-only; the etched frame primitive is bit-exact (DrawEdge EDGE_ETCHED). */
static void u32_group_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(15 /*COLOR_3DFACE*/));
    int32_t fr[4] = { 0, 6, w, h };
    u32_drawedge(bm, fr, 0x6 /*EDGE_ETCHED*/, 0xFu /*BF_RECT*/);
    const char *cap = g_u32_win[wi].title;
    if (cap && cap[0] && g_u32_win[wi].ctrl_font) {
        uint32_t sf = g_gdi[d].sel_font, tc = g_gdi[d].text_color, bc = g_gdi[d].bk_color; int bk = g_gdi[d].bk_mode;
        g_gdi[d].sel_font = g_u32_win[wi].ctrl_font;
        g_gdi[d].text_color = u32_syscolor(8 /*COLOR_WINDOWTEXT*/);
        g_gdi[d].bk_color = u32_syscolor(15 /*COLOR_3DFACE*/);
        g_gdi[d].bk_mode = 2 /*OPAQUE — erases the border behind the label*/;
        uint32_t cps[256]; int m = 0; for (; cap[m] && m < 255; m++) cps[m] = u32_ansi_cp((unsigned char)cap[m]);
        int32_t r2[4] = { 8, 0, w, 14 };
        u32_drawtext(hdc, cps, m, (uint32_t)(uintptr_t)r2, 0x20u /*DT_SINGLELINE (left/top)*/);
        g_gdi[d].sel_font = sf; g_gdi[d].text_color = tc; g_gdi[d].bk_color = bc; g_gdi[d].bk_mode = bk;
    }
}
/* LISTBOX client (matches WM_PRINTCLIENT): COLOR_WINDOW fill + each item's text; the
 * selected row is filled COLOR_HIGHLIGHT with COLOR_HIGHLIGHTTEXT. Row height ≈ the
 * control-font cell (font-metric dependent, like all text); the sunken border is
 * composite-only. */
static void u32_listbox_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    u32_ctrl_fill(bm, w, h, u32_syscolor(5 /*COLOR_WINDOW*/));
    int ih = 14;   /* item row height (approx) */
    for (int k = 0; k < g_u32_win[wi].item_count; k++) {
        int y = k * ih; if (y >= h) break;
        int sel = (k == g_u32_win[wi].cur_sel);
        if (sel) for (int yy = y; yy < y + ih && yy < h; yy++) for (int xx = 0; xx < w; xx++)
            gdi_put(bm, xx, yy, u32_syscolor(13 /*COLOR_HIGHLIGHT*/));
        int32_t r2[4] = { 2, y, w, y + ih };
        u32_paint_text(hdc, d, g_u32_win[wi].ctrl_font, u32_items_get(wi, k), r2,
                       0x4u | 0x20u /*DT_VCENTER|DT_SINGLELINE*/, sel ? 14 /*HIGHLIGHTTEXT*/ : 8 /*WINDOWTEXT*/);
    }
}
/* COMBOBOX (closed state): a top field (≈ one text row) showing the current selection,
 * with a raised drop-down arrow button on the right; below the field is dialog background
 * (the drop list only appears when open). Composite-only; field/arrow use bit-exact
 * primitives. `fh` = field height. */
static void u32_combobox_paint(uint32_t hdc, int wi) {
    struct gdi_obj *bm = gdi_dc_surface(hdc); int d = gdi_idx(hdc);
    if (!bm || d < 0) return;
    int w = g_u32_win[wi].w, h = g_u32_win[wi].h;
    int fh = h < 21 ? h : 21;                      /* closed field height */
    /* A focused CBS_DROPDOWNLIST shows its selection highlighted (COLOR_HIGHLIGHT field
     * + COLOR_HIGHLIGHTTEXT), like Wine; unfocused it is COLOR_WINDOW + COLOR_WINDOWTEXT. */
    int focused = (g_u32_focus == (uint32_t)(wi + 1));
    u32_ctrl_fill(bm, w, h, u32_syscolor(15 /*COLOR_3DFACE*/));  /* area below the field */
    uint32_t fieldc = focused ? u32_syscolor(13 /*COLOR_HIGHLIGHT*/) : u32_syscolor(5 /*COLOR_WINDOW*/);
    for (int y = 0; y < fh; y++) for (int x = 0; x < w - 17; x++) gdi_put(bm, x, y, fieldc);
    for (int y = 0; y < fh; y++) for (int x = w - 17; x < w; x++) gdi_put(bm, x, y, u32_syscolor(5 /*COLOR_WINDOW*/));
    int32_t r2[4] = { 2, 0, w - 18, fh };
    u32_paint_text(hdc, d, g_u32_win[wi].ctrl_font, u32_items_get(wi, g_u32_win[wi].cur_sel), r2,
                   0x4u | 0x20u /*DT_VCENTER|DT_SINGLELINE*/, focused ? 14 /*HIGHLIGHTTEXT*/ : 8 /*WINDOWTEXT*/);
    int32_t br[4] = { w - 17, 0, w, fh };
    u32_drawedge(bm, br, 0x5 /*EDGE_RAISED*/, 0xFu | 0x800u | 0x1000u);   /* drop button bevel */
    int ax = w - 9, ay = fh / 2 - 1; uint32_t tx = u32_syscolor(8);       /* down arrow */
    for (int r = 0; r < 4; r++) for (int c = -3 + r; c <= 3 - r; c++) gdi_put(bm, ax + c, ay + r, tx);
}
/* Which predefined classes have a built-in paint. */
static int u32_ctrl_paintable(const char *cls) {
    return !strcasecmp(cls, "button") || !strcasecmp(cls, "static") || !strcasecmp(cls, "edit")
        || !strcasecmp(cls, "listbox") || !strcasecmp(cls, "combobox");
}
/* BUTTON sub-styles (low 4 bits BS_*): push = BS_PUSHBUTTON/BS_DEFPUSHBUTTON; check =
 * BS_CHECKBOX/BS_AUTOCHECKBOX/BS_3STATE/BS_AUTO3STATE; group box = BS_GROUPBOX(7)
 * (radio 4/9 = curved, not painted). */
static int u32_btn_is_push(uint32_t style)  { uint32_t t = style & 0xFu; return t == 0 || t == 1; }
static int u32_btn_is_check(uint32_t style) { uint32_t t = style & 0xFu; return t == 2 || t == 3 || t == 5 || t == 6; }
static int u32_btn_is_radio(uint32_t style) { uint32_t t = style & 0xFu; return t == 4 || t == 9; }
static int u32_btn_is_group(uint32_t style) { return (style & 0xFu) == 7; }
/* Full on-screen appearance of a control (for the dialog composite): the client paint
 * plus any non-client 3D border (EDIT gets a sunken edge). */
static void u32_control_paint_full(uint32_t hdc, int wi) {
    const char *cls = g_u32_win[wi].classname;
    if (!strcasecmp(cls, "button")) {
        uint32_t st = g_u32_win[wi].style;
        if (u32_btn_is_check(st))      u32_check_paint(hdc, wi);
        else if (u32_btn_is_radio(st)) u32_radio_paint(hdc, wi);
        else if (u32_btn_is_group(st)) u32_group_paint(hdc, wi);
        else if (u32_btn_is_push(st))  u32_button_paint(hdc, wi);
        return;
    }
    if (!strcasecmp(cls, "static")) { u32_static_paint(hdc, wi); return; }
    if (!strcasecmp(cls, "edit")) {
        u32_edit_paint(hdc, wi, 2);
        struct gdi_obj *bm = gdi_dc_surface(hdc);
        int32_t rc[4] = { 0, 0, g_u32_win[wi].w, g_u32_win[wi].h };
        if (bm) u32_drawedge(bm, rc, 0x0Au /*EDGE_SUNKEN*/, 0xFu /*BF_RECT*/);
        return;
    }
    if (!strcasecmp(cls, "listbox")) {
        u32_listbox_paint(hdc, wi);
        struct gdi_obj *bm = gdi_dc_surface(hdc);
        int32_t rc[4] = { 0, 0, g_u32_win[wi].w, g_u32_win[wi].h };
        if (bm) u32_drawedge(bm, rc, 0x0Au /*EDGE_SUNKEN*/, 0xFu /*BF_RECT*/);
        return;
    }
    if (!strcasecmp(cls, "combobox")) { u32_combobox_paint(hdc, wi); return; }
}
/* Recomposite the parent dialog of control `ci` (reflect a state change on screen).
 * esp==0 (SDL input path) is fine: a dialog's controls are painted without a lifted
 * call; only a lifted child (driven by WM_PAINT) needs esp, and that path always has it. */
static void u32_ctrl_recomposite(uint32_t esp, int ci) {
#ifdef ARET_HAVE_SDL
    uint32_t par = g_u32_win[ci].parent;
    if (par >= 1 && par <= U32_MAX_WIN && g_u32_win[par - 1].used && g_u32_win[par - 1].is_dialog) {
        u32_dialog_composite(esp, (int)par - 1); sdl_window_present((int)par - 1);
    }
#else
    (void)esp; (void)ci;
#endif
}
/* Apply a click to a button control `i`: run the auto behaviour (toggle checkbox, cycle
 * 3-state, select radio + clear siblings), recomposite, and notify the parent with
 * WM_COMMAND(BN_CLICKED). The auto-state model follows the Win32 spec (BM_CLICK); Wine
 * gives no deterministic headless oracle for click behaviour, so this is verified
 * qualitatively (a real click) + by the documented auto semantics. */
static void u32_ctrl_click(uint32_t esp, int i) {
    uint32_t t = g_u32_win[i].style & 0xFu;
    if (t == 3)      g_u32_win[i].check_state = g_u32_win[i].check_state ? 0 : 1;   /* BS_AUTOCHECKBOX */
    else if (t == 6) g_u32_win[i].check_state = (g_u32_win[i].check_state + 1) % 3; /* BS_AUTO3STATE */
    else if (t == 9) {                                                              /* BS_AUTORADIOBUTTON */
        uint32_t par = g_u32_win[i].parent;
        for (int k = 0; k < U32_MAX_WIN; k++)
            if (g_u32_win[k].used && g_u32_win[k].parent == par && (g_u32_win[k].style & 0xFu) == 9)
                g_u32_win[k].check_state = 0;
        g_u32_win[i].check_state = 1;
    }
    u32_ctrl_recomposite(esp, i);
    uint32_t par = g_u32_win[i].parent;
    if (par) {
        uint32_t pp = u32_win_wndproc(par);
        if (pp) u32_call_wndproc(esp, pp, par, 0x0111u /*WM_COMMAND*/,
                                 (uint32_t)(g_u32_win[i].ctrl_id & 0xFFFF) /*BN_CLICKED<<16 = 0*/,
                                 (uint32_t)(i + 1));
    }
}
/* Built-in proc for a predefined control with no app WNDPROC. BUTTON/STATIC/EDIT:
 * WM_SETFONT stores the font, WM_GETFONT returns it, WM_PRINTCLIENT paints the control's
 * client into the given DC; a BUTTON also handles BM_GETCHECK/BM_SETCHECK/BM_CLICK.
 * Everything else stays unhandled (caller falls back to 0). */
static int u32_control_proc(uint32_t esp, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t *out) {
    int i = (hwnd >= 1 && hwnd <= U32_MAX_WIN && g_u32_win[hwnd - 1].used) ? (int)hwnd - 1 : -1;
    if (i < 0) return 0;
    const char *cls = g_u32_win[i].classname;
    if (!u32_ctrl_paintable(cls)) return 0;
    if (msg == 0x0030u /*WM_SETFONT*/)  { g_u32_win[i].ctrl_font = wp; *out = 0; return 1; }
    if (msg == 0x0031u /*WM_GETFONT*/)  { *out = g_u32_win[i].ctrl_font; return 1; }
    if (msg == 0x0087u /*WM_GETDLGCODE*/) {   /* control classification (values measured vs Wine) */
        if (!strcasecmp(cls, "button")) { uint32_t t = g_u32_win[i].style & 0xFu;
            *out = (t == 4 || t == 9) ? 0x2040u   /* DLGC_BUTTON|DLGC_RADIOBUTTON */
                 : (t == 1)          ? 0x2010u   /* DLGC_BUTTON|DLGC_DEFPUSHBUTTON */
                 : (t == 0)          ? 0x2020u   /* DLGC_BUTTON|DLGC_UNDEFPUSHBUTTON */
                 : (t == 7)          ? 0x0100u   /* DLGC_STATIC (group box) */
                 :                     0x2000u;  /* DLGC_BUTTON (checkbox/3-state) */
            return 1; }
        if (!strcasecmp(cls, "edit"))     { *out = 0x0089u; return 1; }  /* WANTCHARS|HASSETSEL|WANTARROWS */
        if (!strcasecmp(cls, "static"))   { *out = 0x0100u; return 1; }  /* DLGC_STATIC */
        if (!strcasecmp(cls, "combobox")) { *out = 0x0081u; return 1; }  /* WANTCHARS|WANTARROWS */
        if (!strcasecmp(cls, "listbox"))  { *out = 0x0081u; return 1; }
    }
    if (!strcasecmp(cls, "button")) {
        if (msg == 0x00F0u /*BM_GETCHECK*/) { *out = (uint32_t)g_u32_win[i].check_state; return 1; }
        if (msg == 0x00F1u /*BM_SETCHECK*/) { g_u32_win[i].check_state = (int)wp; u32_ctrl_recomposite(esp, i); *out = 0; return 1; }
        if (msg == 0x00F5u /*BM_CLICK*/)    { u32_ctrl_click(esp, i); *out = 0; return 1; }
    }
    /* LISTBOX (LB_*) / COMBOBOX (CB_*) item model — same operations, different opcodes. */
    if (!strcasecmp(cls, "listbox") || !strcasecmp(cls, "combobox")) {
        int lb = !strcasecmp(cls, "listbox");
        uint32_t A = lb?0x180u:0x143u, IN = lb?0x181u:0x14Au, DE = lb?0x182u:0x144u, RS = lb?0x184u:0x14Bu,
                 SC = lb?0x186u:0x14Eu, GC = lb?0x188u:0x147u, GT = lb?0x189u:0x148u, GL = lb?0x18Au:0x149u, CN = lb?0x18Bu:0x146u;
        if (msg == A)  { int p = u32_items_insert(i, -1, (const char *)(uintptr_t)lp); u32_ctrl_recomposite(esp, i); *out = (uint32_t)p; return 1; }
        if (msg == IN) { int p = u32_items_insert(i, (int)wp, (const char *)(uintptr_t)lp); u32_ctrl_recomposite(esp, i); *out = (uint32_t)p; return 1; }
        if (msg == RS) { u32_items_free(i); u32_ctrl_recomposite(esp, i); *out = 0; return 1; }
        if (msg == CN) { *out = (uint32_t)g_u32_win[i].item_count; return 1; }
        if (msg == SC) { g_u32_win[i].cur_sel = (int)wp; u32_ctrl_recomposite(esp, i); *out = wp; return 1; }
        if (msg == GC) { *out = (uint32_t)g_u32_win[i].cur_sel; return 1; }
        if (msg == GT) { const char *s = u32_items_get(i, (int)wp); char *dst = (char *)(uintptr_t)lp;
                         if (!s) { *out = (uint32_t)-1; return 1; }
                         int n = 0; if (dst) { for (; s[n]; n++) dst[n] = s[n]; dst[n] = 0; } else n = (int)strlen(s);
                         *out = (uint32_t)n; return 1; }
        if (msg == GL) { const char *s = u32_items_get(i, (int)wp); *out = s ? (uint32_t)strlen(s) : (uint32_t)-1; return 1; }
        if (msg == DE) { int idx = (int)wp;
                         if (idx >= 0 && idx < g_u32_win[i].item_count) { free(g_u32_win[i].items[idx]);
                             for (int k = idx; k < g_u32_win[i].item_count - 1; k++) g_u32_win[i].items[k] = g_u32_win[i].items[k + 1];
                             g_u32_win[i].item_count--; }
                         u32_ctrl_recomposite(esp, i); *out = (uint32_t)g_u32_win[i].item_count; return 1; }
    }
    if (msg == 0x0318u /*WM_PRINTCLIENT*/) {
        /* Match Wine's observable WM_PRINTCLIENT: only push buttons paint via this
         * message (checkbox/radio paint nothing here — their glyph is composite-only). */
        if (!strcasecmp(cls, "button"))        { if (u32_btn_is_push(g_u32_win[i].style)) u32_button_paint(wp, i); }
        else if (!strcasecmp(cls, "static"))   u32_static_paint(wp, i);
        else if (!strcasecmp(cls, "listbox"))  u32_listbox_paint(wp, i);
        else if (!strcasecmp(cls, "combobox")) u32_combobox_paint(wp, i);
        else                                   u32_edit_paint(wp, i, 0);   /* client only */
        *out = 0; return 1;
    }
    (void)esp;
    return 0;
}
/* Hit-test a dialog's clickable child controls at client (x,y) and click the topmost
 * one under the point (buttons only; a group box is not clickable). */
static void u32_dialog_hittest_click(uint32_t esp, int di, int x, int y) {
    for (int c = 0; c < U32_MAX_WIN; c++) {
        if (!g_u32_win[c].used || g_u32_win[c].parent != (uint32_t)(di + 1)) continue;
        if (!g_u32_win[c].visible || !g_u32_win[c].enabled) continue;
        int cx = g_u32_win[c].x, cy = g_u32_win[c].y, cw = g_u32_win[c].w, ch = g_u32_win[c].h;
        if (!(x >= cx && x < cx + cw && y >= cy && y < cy + ch)) continue;
        const char *cls = g_u32_win[c].classname;
        if (!strcasecmp(cls, "edit")) { g_u32_focus = (uint32_t)(c + 1); return; }   /* focus for typing */
        if (!strcasecmp(cls, "button") && !u32_btn_is_group(g_u32_win[c].style)) {
            g_u32_focus = (uint32_t)(c + 1);
            u32_ctrl_click(esp, c); return;
        }
    }
}
#ifdef ARET_HAVE_SDL
/* Blit a source RGB888 buffer (sw×sh) into dst (W×H) at (ox,oy), clipped to dst. */
static void u32_blit_clip(uint32_t *dst, int W, int H, const uint32_t *src, int sw, int sh, int ox, int oy) {
    for (int yy = 0; yy < sh; yy++) { int dy = oy + yy; if (dy < 0 || dy >= H) continue;
        for (int xx = 0; xx < sw; xx++) { int dx = ox + xx; if (dx < 0 || dx >= W) continue;
            dst[dy * W + dx] = src[yy * sw + xx]; } }
}
/* Give a child control its own client framebuffer (its WNDPROC paints here via
 * BeginPaint/GetDC, exactly like a top-level window). Size = the control's rect. */
static void u32_ensure_child_bmp(int ci) {
    if (ci < 0 || g_u32_win[ci].client_bmp) return;
    int w = g_u32_win[ci].w, h = g_u32_win[ci].h;
    if (w <= 0 || h <= 0) return;
    int b = gdi_alloc(GDIT_BITMAP); if (!b) return;
    g_gdi[b].w = w; g_gdi[b].h = h; g_gdi[b].topdown = 1; g_gdi[b].bpp = 32;
    g_gdi[b].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[b].owns_bits = 1;
    if (!g_gdi[b].bits) { g_gdi[b].used = 0; return; }
    g_u32_win[ci].client_bmp = gdi_handle(b);
    g_u32_win[ci].cw = w; g_u32_win[ci].ch = h;
}
/* Free a child control's own client framebuffer (on destroy). */
static void u32_free_child_bmp(int i) {
    if (i < 0 || !g_u32_win[i].client_bmp) return;
    int b = gdi_idx(g_u32_win[i].client_bmp);
    if (b >= 0) { if (g_gdi[b].owns_bits) free(g_gdi[b].bits); g_gdi[b].used = 0; }
    g_u32_win[i].client_bmp = 0;
}
/* Composite one visible child control `ci` into the parent framebuffer dst (W×H) at
 * the child's offset. Two kinds of child:
 *  - a PREDEFINED control we paint bit-exact (button/static/edit/list/combo, no app
 *    WNDPROC): drawn via u32_control_paint_full into a temp surface, then blitted.
 *  - a LIFTED control (its own WNDPROC — e.g. a real comctl32 progress bar/trackbar):
 *    it paints ITSELF. We give it its own client framebuffer and send a synchronous
 *    WM_PAINT (the lifted call needs the machine stack `esp`); it Begin/EndPaints into
 *    that framebuffer, which we then blit. `esp==0` (a present with no machine stack at
 *    hand) skips driving a lifted control this pass — sound: it paints on the next
 *    esp-bearing present (UpdateWindow/EndPaint). Correct by composition: the child's
 *    own lifted paint (cpudiff/funcdiff) + our GDI (winediff) land at the measured
 *    offset; no API captures Wine's composited children into a DIB, so verified
 *    qualitatively (Xvfb), per doc 70 §7. */
static void u32_composite_one_child(uint32_t esp, int ci, uint32_t *dst, int W, int H) {
    int cw = g_u32_win[ci].w, ch = g_u32_win[ci].h, ox = g_u32_win[ci].x, oy = g_u32_win[ci].y;
    if (cw <= 0 || ch <= 0) return;
    /* A known predefined class (button/edit/combo/…) is painted by us via its system
     * appearance — even when an app (e.g. MFC via DDX_Control) has SUBCLASSED it, because
     * the control's standard drawing still comes from the system, and its saved "original"
     * proc is our non-painting stub. Driving the subclass's WM_PAINT would paint nothing
     * (a black box). Only a genuinely non-standard control (a lifted comctl32 progress
     * bar/trackbar, unknown class + own WNDPROC) paints itself via WM_PAINT below. */
    if (u32_ctrl_paintable(g_u32_win[ci].classname)) {
        int td = gdi_alloc(GDIT_DC); if (!td) return;
        u32_dc_defaults(td);
        int tb = gdi_alloc(GDIT_BITMAP); if (!tb) { g_gdi[td].used = 0; return; }
        g_gdi[tb].w = cw; g_gdi[tb].h = ch; g_gdi[tb].topdown = 1; g_gdi[tb].bpp = 32;
        g_gdi[tb].bits = (uint8_t *)calloc((size_t)cw * ch, 4); g_gdi[tb].owns_bits = 1;
        if (!g_gdi[tb].bits) { g_gdi[tb].used = 0; g_gdi[td].used = 0; return; }
        g_gdi[td].sel_bitmap = gdi_handle(tb);
        u32_control_paint_full(gdi_handle(td), ci);
        u32_blit_clip(dst, W, H, (uint32_t *)g_gdi[tb].bits, cw, ch, ox, oy);
        free(g_gdi[tb].bits); g_gdi[tb].used = 0; g_gdi[td].used = 0;
        return;
    }
    if (g_u32_win[ci].wndproc != 0 && esp != 0) {
        u32_ensure_child_bmp(ci);
        int b = gdi_idx(g_u32_win[ci].client_bmp);
        if (b < 0 || !g_gdi[b].bits) return;
        g_u32_win[ci].needs_erase = 1;   /* BeginPaint erases the control's class background first */
        u32_call_wndproc(esp, g_u32_win[ci].wndproc, (uint32_t)(ci + 1), U32_WM_PAINT, 0, 0);
        u32_blit_clip(dst, W, H, (uint32_t *)g_gdi[b].bits, g_gdi[b].w, g_gdi[b].h, ox, oy);
        return;
    }
    /* Unknown predefined class with no paint, or no esp to drive a lifted control:
     * sound visible gap (background), never a guessed rendering. */
}
/* Composite every visible direct child of `di` into its client framebuffer at their
 * offsets. Does NOT fill the background (the caller already painted the client: the
 * dialog erase, or the window's own WM_PAINT). Children draw on top, as in Windows. */
static void u32_composite_children(uint32_t esp, int di) {
    if (di < 0 || di >= U32_MAX_WIN || !g_u32_win[di].used) return;
    int b = gdi_idx(g_u32_win[di].client_bmp);
    if (b < 0 || !g_gdi[b].bits) return;
    int W = g_u32_win[di].cw, H = g_u32_win[di].ch;
    uint32_t *dst = (uint32_t *)g_gdi[b].bits;
    for (int c = 0; c < U32_MAX_WIN; c++) {
        if (!g_u32_win[c].used || g_u32_win[c].parent != (uint32_t)(di + 1)) continue;
        if (!g_u32_win[c].visible) continue;
        u32_composite_one_child(esp, c, dst, W, H);
    }
}
/* Composite a dialog's client framebuffer: fill COLOR_3DFACE (the dialog erase colour,
 * measured vs Wine) then compose its child controls on top. */
static void u32_dialog_composite(uint32_t esp, int di) {
    if (di < 0 || di >= U32_MAX_WIN || !g_u32_win[di].used) return;
    int b = gdi_idx(g_u32_win[di].client_bmp);
    if (b < 0 || !g_gdi[b].bits) return;
    int W = g_u32_win[di].cw, H = g_u32_win[di].ch;
    uint32_t *dst = (uint32_t *)g_gdi[b].bits;
    uint32_t face = u32_syscolor(15 /*COLOR_3DFACE*/);
    for (int i = 0; i < W * H; i++) dst[i] = face;
    u32_composite_children(esp, di);
}
/* Present a top-level window: compose its child controls (a dialog also refills its
 * 3DFACE background; a plain window keeps whatever its own WM_PAINT drew), then blit
 * the client framebuffer to the SDL window. This is what makes a child control —
 * predefined or lifted comctl32 — actually appear on screen. */
static void u32_present_toplevel(uint32_t esp, int wi) {
    if (wi < 0 || wi >= U32_MAX_WIN || !g_u32_win[wi].used) return;
    if (g_u32_win[wi].is_dialog) u32_dialog_composite(esp, wi);
    else                         u32_composite_children(esp, wi);
    sdl_window_present(wi);
}
#endif /* ARET_HAVE_SDL */
/* PolylineTo(hdc, const POINT* pts, int count) -> BOOL. Like a run of LineTo: from
 * the current position, a Bresenham segment to each point (endpoint excluded),
 * updating the current position to the last point (measured on Wine). */
uint32_t aret_PolylineTo(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *pts = (const int32_t *)(uintptr_t)WU(1); int n = WI(2);
    uint32_t c; int pr = gdi_pen(d, &c);
    if (pr < 0) return 0;
    if (pts && n >= 1) {
        int cx = g_gdi[d].cur_x, cy = g_gdi[d].cur_y;
        for (int i = 0; i < n; i++) {
            int nx = pts[2 * i], ny = pts[2 * i + 1];
            if (pr && bm && bm->bpp == 32) gdi_bres(bm, cx, cy, nx, ny, c);
            cx = nx; cy = ny;
        }
        g_gdi[d].cur_x = cx; g_gdi[d].cur_y = cy;
    }
    return 1;
}
/* FillRect(hdc, const RECT*, hbrush) -> int. [left,right) x [top,bottom). */
uint32_t aret_FillRect(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *r = (const int32_t *)WP(1);
    if (!bm || !r) return 0;
    uint32_t c;
    if (!gdi_brush_color(WU(2), &c)) return 1;            /* null brush: nothing */
    for (int y = r[1]; y < r[3]; y++)
        for (int x = r[0]; x < r[2]; x++) gdi_put(bm, x, y, c);
    return 1;
}
/* PatBlt(hdc, x, y, w, h, rop) -> BOOL. PATCOPY = fill with the selected brush;
 * BLACKNESS/WHITENESS = solid black/white. Other ROPs abort sound. */
uint32_t aret_PatBlt(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    if (!bm) return 0;
    int x0 = WI(1), y0 = WI(2), x1 = x0 + WI(3), y1 = y0 + WI(4);
    uint32_t rop = WU(5), c;
    if (rop == 0x00F00021u /* PATCOPY */) {
        int b = gdi_idx(g_gdi[gdi_idx(WU(0))].sel_brush);
        if (b < 0) return 0;
        if (g_gdi[b].null_obj) return 1;
        c = g_gdi[b].color;
    } else if (rop == 0x00000042u /* BLACKNESS */) c = 0x000000;
    else if (rop == 0x00FF0062u /* WHITENESS */) c = 0xFFFFFF;
    else { aret_partial("PatBlt: only PATCOPY/BLACKNESS/WHITENESS modelled"); return 0; }
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) gdi_put(bm, x, y, c);
    return 1;
}
/* BitBlt(hdcDst, x, y, w, h, hdcSrc, x1, y1, rop) -> BOOL. SRCCOPY only. */
/* Combine source/destination pixels per a binary raster-op (the common S,D BitBlt
 * ROP3 codes — pattern-based ROPs that read the brush are a follow-up). */
static int gdi_rop_needs_src(uint32_t rop) {
    return !(rop == 0x00550009u /*DSTINVERT*/ || rop == 0x00000042u /*BLACKNESS*/ || rop == 0x00FF0062u /*WHITENESS*/);
}
static int gdi_rop_apply(uint32_t rop, uint32_t s, uint32_t d, uint32_t *out) {
    switch (rop) {
    case 0x00CC0020u: *out = s;        return 1;   /* SRCCOPY    */
    case 0x008800C6u: *out = s & d;    return 1;   /* SRCAND     */
    case 0x00EE0086u: *out = s | d;    return 1;   /* SRCPAINT   */
    case 0x00660046u: *out = s ^ d;    return 1;   /* SRCINVERT  */
    case 0x00330008u: *out = ~s;       return 1;   /* NOTSRCCOPY */
    case 0x00440328u: *out = s & ~d;   return 1;   /* SRCERASE   */
    case 0x001100A6u: *out = ~(s | d); return 1;   /* NOTSRCERASE*/
    case 0x00BB0226u: *out = ~s | d;   return 1;   /* MERGEPAINT */
    case 0x00550009u: *out = ~d;       return 1;   /* DSTINVERT  */
    case 0x00000042u: *out = 0;        return 1;   /* BLACKNESS  */
    case 0x00FF0062u: *out = ~0u;      return 1;   /* WHITENESS  */
    default: return 0;
    }
}
uint32_t aret_BitBlt(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *dst = gdi_dc_surface(WU(0));
    uint32_t rop = WU(8), tmp;
    if (!gdi_rop_apply(rop, 0, 0, &tmp)) { aret_partial("BitBlt: unmodelled raster-op"); return 0; }
    int needs = gdi_rop_needs_src(rop);
    struct gdi_obj *src = needs ? gdi_dc_surface(WU(5)) : NULL;
    if (!dst || (needs && !src)) return 0;
    int dx = WI(1), dy = WI(2), w = WI(3), h = WI(4), sx = WI(6), sy = WI(7);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t *d = gdi_px(dst, dx + i, dy + j);
            if (!d) continue;
            uint32_t S = 0;
            if (needs) {
                uint8_t *s = gdi_px(src, sx + i, sy + j);
                if (!s) continue;
                S = (uint32_t)s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
            }
            uint32_t D = (uint32_t)d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24, R;
            gdi_rop_apply(rop, S, D, &R);
            d[0] = (uint8_t)R; d[1] = (uint8_t)(R >> 8); d[2] = (uint8_t)(R >> 16); d[3] = (uint8_t)(R >> 24);
        }
    return 1;
}
/* ---- comctl32 socle batch 7: StretchBlt / SetDIBits / GdiAlphaBlend / pens.
 * All reuse the proven 32bpp DIB model (gdi_px / gdi_put / gdi_rop_apply). Semantics
 * measured bit-exact vs Wine: StretchBlt nearest-neighbour (src = s0 + i*sw/dw),
 * SetDIBits bottom-up (image row y = H-1-scan), GdiAlphaBlend
 * out = (src*ca + dst*(255-ca))/255 (AC_SRC_ALPHA adds premultiplied per-pixel alpha). */
uint32_t aret_StretchBlt(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    struct gdi_obj *dst = gdi_dc_surface(WU(0));
    uint32_t rop = WU(10), tmp;
    if (!gdi_rop_apply(rop, 0, 0, &tmp)) { aret_partial("StretchBlt: unmodelled raster-op"); return 0; }
    int needs = gdi_rop_needs_src(rop);
    struct gdi_obj *src = needs ? gdi_dc_surface(WU(5)) : NULL;
    if (!dst || (needs && !src)) return 0;
    int dx = WI(1), dy = WI(2), dw = WI(3), dh = WI(4), sx = WI(6), sy = WI(7), sw = WI(8), sh = WI(9);
    if (dw <= 0 || dh <= 0 || (needs && (sw <= 0 || sh <= 0))) {
        aret_partial("StretchBlt: only positive (non-mirrored) extents modelled"); return 0;
    }
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            uint8_t *d = gdi_px(dst, dx + i, dy + j);
            if (!d) continue;
            uint32_t S = 0;
            if (needs) {
                uint8_t *s = gdi_px(src, sx + i * sw / dw, sy + j * sh / dh);   /* nearest-neighbour */
                if (!s) continue;
                S = (uint32_t)s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24;
            }
            uint32_t D = (uint32_t)d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24, R;
            gdi_rop_apply(rop, S, D, &R);
            d[0] = (uint8_t)R; d[1] = (uint8_t)(R >> 8); d[2] = (uint8_t)(R >> 16); d[3] = (uint8_t)(R >> 24);
        }
    return 1;
}
/* SetDIBits(hdc, hbm, uStartScan, cScanLines, lpBits, BITMAPINFO*, uColorUse) -> lines set. */
uint32_t aret_SetDIBits(uint32_t esp) {
    int bi = gdi_idx(WU(1)); if (bi < 0 || g_gdi[bi].type != GDIT_BITMAP) return 0;
    struct gdi_obj *bm = &g_gdi[bi];
    uint32_t start = WU(2), lines = WU(3);
    const uint8_t *bits = (const uint8_t *)WP(4);
    const int32_t *h = (const int32_t *)WP(5);       /* BITMAPINFOHEADER */
    if (!bits || !h) return 0;
    int W = h[1], Hs = h[2], botup = Hs > 0, H = Hs < 0 ? -Hs : Hs;
    int bpp = (h[3] >> 16) & 0xFFFF; uint32_t comp = (uint32_t)h[4];
    if (comp != 0 || (bpp != 32 && bpp != 24)) { aret_partial("SetDIBits: only BI_RGB 24/32bpp modelled"); return 0; }
    int bppB = bpp / 8, stride = (W * bppB + 3) & ~3;
    uint32_t done = 0;
    for (uint32_t k = 0; k < lines; k++) {
        int scan = (int)(start + k); if (scan < 0 || scan >= H) break;
        int y = botup ? (H - 1 - scan) : scan;         /* memory scanline -> image row (0 = top) */
        const uint8_t *row = bits + (size_t)scan * stride;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = row + x * bppB;
            gdi_put(bm, x, y, (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]);   /* [B,G,R] -> COLORREF 0x00BBGGRR */
        }
        done++;
    }
    return done;
}
/* GdiAlphaBlend(hdcD, xd,yd,wd,hd, hdcS, xs,ys,ws,hs, BLENDFUNCTION packed in a DWORD). */
uint32_t aret_GdiAlphaBlend(uint32_t esp) {
    struct gdi_obj *dst = gdi_dc_surface(WU(0)), *src = gdi_dc_surface(WU(5));
    if (!dst || !src) return 0;
    int dx = WI(1), dy = WI(2), dw = WI(3), dh = WI(4), sx = WI(6), sy = WI(7), sw = WI(8), sh = WI(9);
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) { aret_partial("GdiAlphaBlend: positive extents only"); return 0; }
    uint32_t bf = WU(10);
    int ca = (bf >> 16) & 0xFF, af = (bf >> 24) & 0xFF;   /* SourceConstantAlpha, AlphaFormat */
    for (int j = 0; j < dh; j++)
        for (int i = 0; i < dw; i++) {
            uint8_t *s = gdi_px(src, sx + i * sw / dw, sy + j * sh / dh), *d = gdi_px(dst, dx + i, dy + j);
            if (!s || !d) continue;
            int inv;
            if (af & 1) {   /* AC_SRC_ALPHA: source is premultiplied; blend by src.A*ca */
                int a = s[3] * ca / 255; inv = 255 - a;
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)(s[c] * ca / 255 + d[c] * inv / 255);
            } else {        /* constant alpha only */
                inv = 255 - ca;
                for (int c = 0; c < 3; c++) d[c] = (uint8_t)((s[c] * ca + d[c] * inv) / 255);
            }
        }
    return 1;
}
uint32_t aret_CreatePenIndirect(uint32_t esp) {
    const int32_t *lp = (const int32_t *)WP(0); if (!lp) return 0;   /* LOGPEN{style, POINT width, color} */
    int i = gdi_alloc(GDIT_PEN); if (!i) return 0;
    g_gdi[i].pen_style = lp[0]; g_gdi[i].pen_width = lp[1]; g_gdi[i].color = (uint32_t)lp[3] & 0x00FFFFFFu;
    if (lp[0] == 5) g_gdi[i].null_obj = 1;                            /* PS_NULL */
    return gdi_handle(i);
}
uint32_t aret_ExtCreatePen(uint32_t esp) {
    uint32_t style = WU(0); int width = WI(1);
    const int32_t *lb = (const int32_t *)WP(2);                       /* LOGBRUSH{lbStyle, lbColor, lbHatch} */
    int i = gdi_alloc(GDIT_PEN); if (!i) return 0;
    g_gdi[i].pen_style = (int)(style & 0xF); g_gdi[i].pen_width = width;
    g_gdi[i].color = lb ? ((uint32_t)lb[1] & 0x00FFFFFFu) : 0;
    if ((style & 0xF) == 5) g_gdi[i].null_obj = 1;
    return gdi_handle(i);
}
uint32_t aret_GetDIBColorTable(uint32_t esp) { (void)esp; return 0; }   /* 32bpp DC: no palette */
uint32_t aret_SetDIBColorTable(uint32_t esp) { (void)esp; return 0; }

/* ================================================================== */
/* Common controls (comctl32) — image list. The data foundation of      */
/* toolbars / list-view / tree-view: a list of same-size 32bpp images.   */
/* Images are stored as a vertical strip (cx wide, count*cy tall); the    */
/* per-pixel alpha byte carries the transparency mask (0 = transparent).  */
/* Draw blits into a DC surface (bit-exact, reuses the GDI DIB model).    */
/* ================================================================== */
#define U32_MAX_IML 64
#define IML_BASE 0x50000000u
static struct { int used, cx, cy, count, cap; uint32_t bkcolor; uint8_t *bits; } g_iml[U32_MAX_IML];
static int iml_idx(uint32_t h) {
    if ((h & 0xFF000000u) != IML_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < U32_MAX_IML && g_iml[i].used) ? (int)i : -1;
}
static uint8_t *iml_px(int m, int img, int x, int y) {
    if (x < 0 || y < 0 || x >= g_iml[m].cx || y >= g_iml[m].cy) return NULL;
    return g_iml[m].bits + ((size_t)(img * g_iml[m].cy + y) * g_iml[m].cx + x) * 4;
}
static int iml_grow(int m, int need) {
    if (need <= g_iml[m].cap) return 1;
    int nc = g_iml[m].cap * 2; if (nc < need) nc = need;
    uint8_t *nb = (uint8_t *)calloc((size_t)g_iml[m].cx * g_iml[m].cy * nc, 4);
    if (!nb) return 0;
    if (g_iml[m].bits) { memcpy(nb, g_iml[m].bits, (size_t)g_iml[m].cx * g_iml[m].cy * g_iml[m].count * 4); free(g_iml[m].bits); }
    g_iml[m].bits = nb; g_iml[m].cap = nc; return 1;
}
uint32_t aret_ImageList_Create(uint32_t esp) {
    int cx = WI(0), cy = WI(1), cInit = WI(3);
    if (cx <= 0 || cy <= 0 || cx > 1024 || cy > 1024) return 0;
    for (int i = 0; i < U32_MAX_IML; i++) if (!g_iml[i].used) {
        memset(&g_iml[i], 0, sizeof g_iml[i]);
        g_iml[i].used = 1; g_iml[i].cx = cx; g_iml[i].cy = cy;
        g_iml[i].bkcolor = 0xFFFFFFFFu;                 /* CLR_NONE */
        g_iml[i].cap = cInit > 0 ? cInit : 4;
        g_iml[i].bits = (uint8_t *)calloc((size_t)cx * cy * g_iml[i].cap, 4);
        if (!g_iml[i].bits) { g_iml[i].used = 0; return 0; }
        return IML_BASE | (uint32_t)i;
    }
    return 0;
}
/* Copy the cx-wide tiles of a source HBITMAP into the list (opaque 32bpp copy).
 * We model 32bpp colour images without a colour-key mask plane, matching Wine's
 * behaviour for an ILC_COLOR32 source built from a flat BI_RGB DIB: the mask a
 * lower-depth list would derive from a colour key is not applied (32bpp images
 * carry their own alpha, which our sources don't). Returns the first new index. */
static int iml_add(int m, uint32_t hbm) {
    int b = gdi_idx(hbm);
    if (b < 0 || g_gdi[b].type != GDIT_BITMAP || !g_gdi[b].bits) return -1;
    int n = g_gdi[b].w / g_iml[m].cx; if (n < 1) n = 1;
    if (!iml_grow(m, g_iml[m].count + n)) return -1;
    int first = g_iml[m].count;
    for (int k = 0; k < n; k++) {
        int img = first + k;
        for (int y = 0; y < g_iml[m].cy; y++) for (int x = 0; x < g_iml[m].cx; x++) {
            uint8_t *s = gdi_px(&g_gdi[b], k * g_iml[m].cx + x, y);
            uint8_t *d = iml_px(m, img, x, y);
            if (!d) continue;
            if (s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0; }
            else  { d[0] = d[1] = d[2] = d[3] = 0; }
        }
    }
    g_iml[m].count = first + n;
    return first;
}
uint32_t aret_ImageList_Add(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? (uint32_t)-1 : (uint32_t)iml_add(m, WU(1)); }
uint32_t aret_ImageList_AddMasked(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? (uint32_t)-1 : (uint32_t)iml_add(m, WU(1)); }
/* ImageList_Draw(himl, i, hdcDst, x, y, fStyle) -> BOOL. An opaque tile copy into
 * the destination surface (see iml_add on the 32bpp mask model). */
uint32_t aret_ImageList_Draw(uint32_t esp) {
    int m = iml_idx(WU(0)), img = WI(1);
    if (m < 0 || img < 0 || img >= g_iml[m].count) return 0;
    struct gdi_obj *dst = gdi_dc_surface(WU(2));
    if (!dst) return 0;
    int x0 = WI(3), y0 = WI(4);
    for (int y = 0; y < g_iml[m].cy; y++) for (int x = 0; x < g_iml[m].cx; x++) {
        uint8_t *s = iml_px(m, img, x, y), *d = gdi_px(dst, x0 + x, y0 + y);
        if (s && d) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 0; }
    }
    return 1;
}
uint32_t aret_ImageList_GetImageCount(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? 0 : (uint32_t)g_iml[m].count; }
uint32_t aret_ImageList_SetBkColor(uint32_t esp) { int m = iml_idx(WU(0)); if (m < 0) return 0xFFFFFFFFu; uint32_t o = g_iml[m].bkcolor; g_iml[m].bkcolor = WU(1); return o; }
uint32_t aret_ImageList_GetBkColor(uint32_t esp) { int m = iml_idx(WU(0)); return m < 0 ? 0xFFFFFFFFu : g_iml[m].bkcolor; }
uint32_t aret_ImageList_GetIconSize(uint32_t esp) {
    int m = iml_idx(WU(0)); if (m < 0) return 0;
    int32_t *cx = (int32_t *)WP(1), *cy = (int32_t *)WP(2);
    if (cx) *cx = g_iml[m].cx; if (cy) *cy = g_iml[m].cy; return 1;
}
uint32_t aret_ImageList_Destroy(uint32_t esp) { int m = iml_idx(WU(0)); if (m < 0) return 0; free(g_iml[m].bits); g_iml[m].used = 0; return 1; }
/* InitCommonControls() -> void; InitCommonControlsEx(const INITCOMMONCONTROLSEX*)
 * -> BOOL. Registering the control classes is a no-op in our model. */
uint32_t aret_InitCommonControls(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_InitCommonControlsEx(uint32_t esp) { (void)esp; return 1; }
/* ---- Runtime delay-load resolver -------------------------------------------
 * A lifted DLL can delay-load an API on first use (Wine's comctl32 delay-loads
 * uxtheme for visual styles) by calling ResolveDelayLoadedAPI, which must return
 * a callable address. ARET's dispatch table is static, so a resolved import gets
 * a synthetic VA at runtime; `aret_call` dispatches it through the
 * `aret_delay_dispatch` fallback and `__aret_callee_pop` gets its stdcall pop
 * through the `aret_delay_pop` fallback. We only resolve APIs we model soundly —
 * uxtheme to a "no visual theme" behaviour so the control falls back to classic
 * rendering (its logic/state is unaffected). Anything else -> named hard abort. */
static uint32_t aret_theme_null(uint32_t esp) { (void)esp; return 0; } /* NULL/FALSE/S_OK */

static int u32_stricmp_(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
/* Known delay-loadable APIs we model. uxtheme "no theme": the query functions
 * report not-themed / a NULL handle, so a control takes its classic path. */
static const struct { const char *dll, *fn; uint32_t (*shim)(uint32_t); uint16_t pop; } k_delay[] = {
    { "uxtheme.dll", "OpenThemeData",             aret_theme_null, 8  },
    { "uxtheme.dll", "OpenThemeDataEx",           aret_theme_null, 12 },
    { "uxtheme.dll", "CloseThemeData",            aret_theme_null, 4  },
    { "uxtheme.dll", "IsThemeActive",             aret_theme_null, 0  },
    { "uxtheme.dll", "IsAppThemed",               aret_theme_null, 0  },
    { "uxtheme.dll", "GetWindowTheme",            aret_theme_null, 4  },
    { "uxtheme.dll", "SetWindowTheme",            aret_theme_null, 12 },
    { "uxtheme.dll", "EnableThemeDialogTexture",  aret_theme_null, 8  },
    { "uxtheme.dll", "DrawThemeParentBackground", aret_theme_null, 16 },
    { NULL, NULL, NULL, 0 }
};
#define DELAY_VA_BASE 0x7EDA0000u
static struct { uint32_t va; uint32_t (*shim)(uint32_t); uint16_t pop; } g_delay_res[64];
static int g_delay_res_n;

/* Dispatch a resolved delay VA (called by aret_call on a static-table miss). */
int aret_delay_dispatch(uint32_t va, uint32_t esp, uint64_t *out) {
    for (int i = 0; i < g_delay_res_n; i++)
        if (g_delay_res[i].va == va) { *out = g_delay_res[i].shim(esp + 4); return 1; }
    return 0;
}
/* Stdcall pop for a resolved delay VA (called by __aret_callee_pop). */
uint32_t aret_delay_pop(uint32_t va) {
    for (int i = 0; i < g_delay_res_n; i++)
        if (g_delay_res[i].va == va) return g_delay_res[i].pop;
    return 0;
}

/* ---- CoGetMalloc / IMalloc — the first COM INTERFACE in the HLE -------------
 *
 * Everything COM-shaped until now has been a flat function. This one hands the
 * program a VTABLE and the program then calls THROUGH it, so the HLE has to be
 * callable from lifted code rather than the other way round. That mechanism already
 * exists and is proven: the delay-load resolver hands out synthetic VAs that
 * `aret_call` dispatches back into the HLE. A vtable is just nine of them, so no new
 * machinery is invented here — the doc's "COM vtable problem" turns out to be the
 * delay-load problem in a different shape.
 *
 * MEASURED (`winecorpus/win32_comalloc.c`), and three rows contradict the documented
 * COM contract, which is exactly why they were probed rather than reasoned:
 *   - QueryInterface does **NOT** AddRef (three successful QIs leave the count at 1).
 *   - QueryInterface for an unsupported interface returns E_NOINTERFACE and leaves
 *     the out-parameter **untouched** — COM says it must be NULLed; it is not.
 *   - the reference count is real (AddRef->2, Release->1), not a fixed 1, even
 *     though the object is a process singleton that is never destroyed.
 * And the useful positive result: a CoTaskMemAlloc block is known to IMalloc and
 * freeable through it — the two entry points are ONE allocator, not two.
 *
 * GetSize/DidAlloc answer from the side table above, so DidAlloc on a pointer we
 * never allocated is a correct "no" rather than a fault. */
static uint32_t g_imalloc_refs = 1;

static int u32_iid_eq(const uint8_t *iid, uint32_t d1) {
    /* IID_IUnknown {00000000-...-C000-...46}, IID_IMalloc {00000002-...}: they differ
     * only in Data1, and the 12-byte tail is the standard one. Compare in full. */
    static const uint8_t tail[12] = { 0,0, 0,0, 0xC0,0,0,0, 0,0,0,0x46 };
    if (!iid) return 0;
    return *(const uint32_t *)iid == d1 && memcmp(iid + 4, tail, 12) == 0;
}
static uint32_t u32_im_qi(uint32_t esp) {
    uint32_t self = WU(0);
    const uint8_t *iid = (const uint8_t *)(uintptr_t)WU(1);
    uint32_t *out = (uint32_t *)(uintptr_t)WU(2);
    if (u32_iid_eq(iid, 0) || u32_iid_eq(iid, 2)) {   /* IUnknown or IMalloc */
        if (out) *out = self;                          /* no AddRef: measured */
        return 0;                                      /* S_OK */
    }
    return 0x80004002u;          /* E_NOINTERFACE, out left untouched: measured */
}
static uint32_t u32_im_addref(uint32_t esp)  { (void)esp; return ++g_imalloc_refs; }
static uint32_t u32_im_release(uint32_t esp) {
    (void)esp;
    /* A process singleton is never destroyed; the count is still real. */
    return g_imalloc_refs > 1 ? --g_imalloc_refs : 1;
}
static uint32_t u32_im_alloc(uint32_t esp) {
    size_t n = (size_t)WU(1);
    void *p = malloc(n);
    u32_com_track(p, n);
    return (uint32_t)(uintptr_t)p;
}
static uint32_t u32_im_realloc(uint32_t esp) {
    void *old = (void *)(uintptr_t)WU(1);
    size_t n = (size_t)WU(2);
    void *p = realloc(old, n);
    if (p) { u32_com_untrack(old); u32_com_track(p, n); }
    return (uint32_t)(uintptr_t)p;
}
static uint32_t u32_im_free(uint32_t esp) {
    void *p = (void *)(uintptr_t)WU(1);
    u32_com_untrack(p);
    free(p);
    return 0;
}
static uint32_t u32_im_getsize(uint32_t esp) {
    return (uint32_t)u32_com_size((void *)(uintptr_t)WU(1));   /* -1 when not ours */
}
static uint32_t u32_im_didalloc(uint32_t esp) {
    void *p = (void *)(uintptr_t)WU(1);
    if (!p) return 0xFFFFFFFFu;                                 /* -1: measured */
    return u32_com_size(p) == (size_t)-1 ? 0u : 1u;
}
static uint32_t u32_im_heapmin(uint32_t esp) { (void)esp; return 0; }

/* The object and its vtable, in memory the lifted program can reach and read. */
static uint32_t g_imalloc_vtbl[9];
static uint32_t g_imalloc_obj;          /* one field: the vtable pointer */

/* Lazily build the process-singleton IMalloc (vtable in program-reachable memory)
 * and return a pointer to the object. 0 if no synthetic VA is left. Shared by
 * CoGetMalloc and SHGetMalloc — they hand out the SAME allocator, so a block from
 * one frees through the other (a PIDL from SHGetSpecialFolderLocation is freed via
 * SHGetMalloc()->Free in the classic shell idiom). */
static uint32_t u32_get_imalloc(void) {
    if (!g_imalloc_obj) {
        static const struct { uint32_t (*fn)(uint32_t); uint16_t pop; } meth[9] = {
            { u32_im_qi, 12 }, { u32_im_addref, 4 }, { u32_im_release, 4 },
            { u32_im_alloc, 8 }, { u32_im_realloc, 12 }, { u32_im_free, 8 },
            { u32_im_getsize, 8 }, { u32_im_didalloc, 8 }, { u32_im_heapmin, 4 },
        };
        for (int i = 0; i < 9; i++) {
            if (g_delay_res_n >= 64) {
                aret_unmodelled("CoGetMalloc: no synthetic VA left for the IMalloc vtable");
                return 0;
            }
            uint32_t va = DELAY_VA_BASE + (uint32_t)g_delay_res_n;
            g_delay_res[g_delay_res_n].va = va;
            g_delay_res[g_delay_res_n].shim = meth[i].fn;
            g_delay_res[g_delay_res_n].pop = meth[i].pop;
            g_delay_res_n++;
            g_imalloc_vtbl[i] = va;
        }
        g_imalloc_obj = (uint32_t)(uintptr_t)g_imalloc_vtbl;
    }
    return (uint32_t)(uintptr_t)&g_imalloc_obj;   /* singleton: measured */
}

uint32_t aret_CoGetMalloc(uint32_t esp) {
    uint32_t *ppv = (uint32_t *)(uintptr_t)WU(1);
    if (!ppv) return 0x80004003u;       /* E_POINTER */
    uint32_t obj = u32_get_imalloc();
    if (!obj) return 0x80004005u;       /* E_FAIL: no VA left */
    *ppv = obj;
    return 0;                           /* S_OK */
}

/* SHGetMalloc(IMalloc **ppMalloc) -> HRESULT. The shell's task allocator IS the COM
 * task allocator (Wine and Windows return the same IMalloc), so hand out the shared
 * singleton. Its single argument sits at [esp+0] (vs CoGetMalloc's [esp+1]). */
uint32_t aret_SHGetMalloc(uint32_t esp) {
    uint32_t *ppv = (uint32_t *)(uintptr_t)WU(0);
    if (!ppv) return 0x80004003u;       /* E_POINTER */
    uint32_t obj = u32_get_imalloc();
    if (!obj) return 0x80004005u;
    *ppv = obj;
    return 0;                           /* S_OK */
}

/* ---- Shell special folders (shell32 CSIDL / PIDL) --------------------------
 * The CSIDL family, modelled as ONE coherent, sound unit:
 *   SHGetSpecialFolderLocation(hwnd, csidl, &pidl)  -> a synthetic PIDL
 *   SHGetPathFromIDList{W,A}(pidl, path)            -> the path back out
 *   SHGetSpecialFolderPath{W,A}(hwnd, path, csidl, create)  -> path directly
 *   SHGetFolderPath{W,A}(hwnd, csidl, tok, flags, path)     -> path directly (HRESULT)
 * A CSIDL resolves to a Windows path under a synthetic user profile; the file
 * subsystem then maps THAT under the ARET prefix like any other path (a program
 * writing into CSIDL_APPDATA lands in <prefix>/drive_c/users/aret/AppData/...).
 * The exact bytes of a special-folder path are environment-specific (user name,
 * OS layout) and NOT a soundness property — any valid, writable folder of the
 * right kind is correct; so this returns a standard-layout path, not a guess.
 * An UNKNOWN csidl returns a DEFINED failure (E_INVALIDARG / FALSE), never a made-up
 * path. A PIDL we did not create decodes to FALSE (no real shell-namespace PIDL can
 * exist — the enumerators are unimplemented), never a wrong path. The PIDL is a
 * CoTaskMem-tracked block, so IMalloc::Free / CoTaskMemFree / ILFree free it. */
#define ARET_CSIDL_MASK        0x00ffu
#define ARET_CSIDL_FLAG_CREATE 0x8000u
static const uint8_t ARET_PIDL_MAGIC[4] = { 'A','P','I','L' };

static int csidl_to_winpath(unsigned csidl, char *out, size_t cap) {
    const char *p = 0;
    switch (csidl & ARET_CSIDL_MASK) {
        case 0x00: p = "C:\\users\\aret\\Desktop"; break;                 /* DESKTOP */
        case 0x02: p = "C:\\users\\aret\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs"; break; /* PROGRAMS */
        case 0x05: p = "C:\\users\\aret\\Documents"; break;               /* PERSONAL */
        case 0x06: p = "C:\\users\\aret\\Favorites"; break;               /* FAVORITES */
        case 0x07: p = "C:\\users\\aret\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"; break; /* STARTUP */
        case 0x08: p = "C:\\users\\aret\\Recent"; break;                  /* RECENT */
        case 0x09: p = "C:\\users\\aret\\SendTo"; break;                  /* SENDTO */
        case 0x0b: p = "C:\\users\\aret\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"; break; /* STARTMENU */
        case 0x0d: p = "C:\\users\\aret\\Music"; break;                   /* MYMUSIC */
        case 0x0e: p = "C:\\users\\aret\\Videos"; break;                  /* MYVIDEO */
        case 0x10: p = "C:\\users\\aret\\Desktop"; break;                 /* DESKTOPDIRECTORY */
        case 0x14: p = "C:\\windows\\Fonts"; break;                       /* FONTS */
        case 0x15: p = "C:\\users\\aret\\Templates"; break;               /* TEMPLATES */
        case 0x16: p = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu"; break; /* COMMON_STARTMENU */
        case 0x17: p = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs"; break; /* COMMON_PROGRAMS */
        case 0x18: p = "C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"; break; /* COMMON_STARTUP */
        case 0x19: p = "C:\\users\\Public\\Desktop"; break;               /* COMMON_DESKTOPDIRECTORY */
        case 0x1a: p = "C:\\users\\aret\\AppData\\Roaming"; break;        /* APPDATA */
        case 0x1c: p = "C:\\users\\aret\\AppData\\Local"; break;          /* LOCAL_APPDATA */
        case 0x1f: p = "C:\\users\\Public\\Favorites"; break;             /* COMMON_FAVORITES */
        case 0x20: p = "C:\\users\\aret\\AppData\\Local\\Microsoft\\Windows\\Temporary Internet Files"; break; /* INTERNET_CACHE */
        case 0x21: p = "C:\\users\\aret\\AppData\\Roaming\\Microsoft\\Windows\\Cookies"; break; /* COOKIES */
        case 0x22: p = "C:\\users\\aret\\AppData\\Local\\Microsoft\\Windows\\History"; break; /* HISTORY */
        case 0x23: p = "C:\\ProgramData"; break;                          /* COMMON_APPDATA */
        case 0x24: p = "C:\\windows"; break;                              /* WINDOWS */
        case 0x25: p = "C:\\windows\\system32"; break;                    /* SYSTEM */
        case 0x26: p = "C:\\Program Files"; break;                        /* PROGRAM_FILES */
        case 0x27: p = "C:\\users\\aret\\Pictures"; break;                /* MYPICTURES */
        case 0x28: p = "C:\\users\\aret"; break;                          /* PROFILE */
        case 0x2b: p = "C:\\Program Files\\Common Files"; break;          /* PROGRAM_FILES_COMMON */
        case 0x2d: p = "C:\\ProgramData\\Microsoft\\Windows\\Templates"; break; /* COMMON_TEMPLATES */
        case 0x2e: p = "C:\\users\\Public\\Documents"; break;             /* COMMON_DOCUMENTS */
        case 0x35: p = "C:\\users\\Public\\Music"; break;                 /* COMMON_MUSIC */
        case 0x36: p = "C:\\users\\Public\\Pictures"; break;              /* COMMON_PICTURES */
        case 0x37: p = "C:\\users\\Public\\Videos"; break;                /* COMMON_VIDEO */
        default: return 0;
    }
    snprintf(out, cap, "%s", p);
    return 1;
}

/* Create the native directory a CSIDL folder maps to (mkdir -p), so fCreate
 * semantics hold and a program that stats the folder finds it. */
static void shell_ensure_dir(const char *winpath) {
    char nat[1024];
    translate_path(winpath, nat, sizeof nat);
    for (char *s = nat + 1; *s; s++) {
        if (*s == '/') { *s = 0; mkdir(nat, 0777); *s = '/'; }
    }
    mkdir(nat, 0777);
}

/* Copy a narrow string to a guest WIDE (UTF-16) buffer (Latin-1 widen — the folder
 * names above are ASCII). Writes the terminating NUL. */
static void shell_put_wide(uint16_t *dst, const char *src) {
    size_t i = 0;
    for (; src[i]; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
}

uint32_t aret_SHGetSpecialFolderLocation(uint32_t esp) {
    /* (HWND hwnd, int csidl, LPITEMIDLIST *ppidl) -> HRESULT */
    unsigned csidl = (unsigned)WU(1);
    uint32_t *ppidl = (uint32_t *)WP(2);
    char win[512];
    if (!csidl_to_winpath(csidl, win, sizeof win)) {
        if (ppidl) *ppidl = 0;
        return 0x80070057u;                 /* E_INVALIDARG: unknown CSIDL, defined failure */
    }
    shell_ensure_dir(win);
    size_t n = strlen(win) + 1;
    unsigned char *blob = (unsigned char *)malloc(4 + n);
    if (!blob) { if (ppidl) *ppidl = 0; return 0x8007000Eu; }  /* E_OUTOFMEMORY */
    memcpy(blob, ARET_PIDL_MAGIC, 4);
    memcpy(blob + 4, win, n);
    u32_com_track(blob, 4 + n);             /* freeable via IMalloc::Free / CoTaskMemFree */
    if (ppidl) *ppidl = (uint32_t)(uintptr_t)blob;
    return 0;                               /* S_OK */
}

/* Decode our synthetic PIDL back to its Windows path; 0 and empty if foreign. */
static const char *shell_pidl_path(uint32_t pidl) {
    const unsigned char *b = (const unsigned char *)(uintptr_t)pidl;
    if (!b || memcmp(b, ARET_PIDL_MAGIC, 4) != 0) return 0;
    return (const char *)(b + 4);
}

uint32_t aret_SHGetPathFromIDListW(uint32_t esp) {
    /* (LPCITEMIDLIST pidl, LPWSTR pszPath) -> BOOL */
    const char *win = shell_pidl_path(WU(0));
    uint16_t *out = (uint16_t *)WP(1);
    if (!win || !out) return 0;             /* FALSE: foreign PIDL, defined failure */
    shell_put_wide(out, win);
    return 1;
}
uint32_t aret_SHGetPathFromIDListA(uint32_t esp) {
    const char *win = shell_pidl_path(WU(0));
    char *out = WS(1);
    if (!win || !out) return 0;
    strcpy(out, win);
    return 1;
}

uint32_t aret_SHGetSpecialFolderPathW(uint32_t esp) {
    /* (HWND hwnd, LPWSTR pszPath, int csidl, BOOL fCreate) -> BOOL */
    uint16_t *out = (uint16_t *)WP(1);
    unsigned csidl = (unsigned)WU(2);
    char win[512];
    if (!out || !csidl_to_winpath(csidl, win, sizeof win)) return 0;
    if (WU(3)) shell_ensure_dir(win);
    shell_put_wide(out, win);
    return 1;
}
uint32_t aret_SHGetSpecialFolderPathA(uint32_t esp) {
    char *out = WS(1);
    unsigned csidl = (unsigned)WU(2);
    char win[512];
    if (!out || !csidl_to_winpath(csidl, win, sizeof win)) return 0;
    if (WU(3)) shell_ensure_dir(win);
    strcpy(out, win);
    return 1;
}

uint32_t aret_SHGetFolderPathW(uint32_t esp) {
    /* (HWND, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath) -> HRESULT */
    unsigned csidl = (unsigned)WU(1);
    uint16_t *out = (uint16_t *)WP(4);
    char win[512];
    if (!out) return 0x80004003u;           /* E_POINTER */
    if (!csidl_to_winpath(csidl, win, sizeof win)) return 0x80070057u; /* E_INVALIDARG */
    shell_ensure_dir(win);                  /* SHGFP always ensures the folder exists */
    shell_put_wide(out, win);
    return 0;                               /* S_OK */
}
uint32_t aret_SHGetFolderPathA(uint32_t esp) {
    unsigned csidl = (unsigned)WU(1);
    char *out = WS(4);
    char win[512];
    if (!out) return 0x80004003u;
    if (!csidl_to_winpath(csidl, win, sizeof win)) return 0x80070057u;
    shell_ensure_dir(win);
    strcpy(out, win);
    return 0;
}

/* CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv) — NOT modelled, but
 * the abort now NAMES what was asked for.
 *
 * "unimplemented import CALLED: CoCreateInstance" is true and useless: which class a
 * program wants is the entire question, and without it the next session has to go
 * find out with a debugger. Printing the CLSID and IID costs nothing and turns the
 * wall into a work item. Same lesson as the x87 guard, which was sound but mute:
 * "loud" and "diagnostic" are not the same property, and only the second saves time.
 *
 * Deliberately still an abort rather than REGDB_E_CLASSNOTREG. That would be a
 * DEFINED failure and therefore tempting, but it is not a sound one here: under Wine
 * the class IS registered and the call succeeds, so answering "not registered" would
 * silently push the program down an error path it never takes on a real system —
 * a different execution, presented as normal. */
static void u32_fmt_guid(char *out, const uint8_t *g) {
    static const char hex[] = "0123456789ABCDEF";
    if (!g) { memcpy(out, "(null)", 7); return; }
    static const int ord[16] = { 3,2,1,0, -1, 5,4, -1, 7,6, -1, 8,9, -1, 10,11 };
    int o = 0;
    out[o++] = '{';
    for (int i = 0; i < 16; i++) {
        if (ord[i] < 0) { out[o++] = '-'; continue; }
        out[o++] = hex[(g[ord[i]] >> 4) & 15];
        out[o++] = hex[g[ord[i]] & 15];
    }
    for (int i = 12; i < 16; i++) {
        out[o++] = hex[(g[i] >> 4) & 15];
        out[o++] = hex[g[i] & 15];
    }
    out[o++] = '}';
    out[o] = 0;
}
/* IID_IClassFactory {00000001-0000-0000-C000-000000000046}, in memory order. */
static const uint8_t u32_iid_classfactory[16] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
};

/* ---- mlang: IMultiLanguage COM object (CLSID_CMultiLanguage) ----------------
 * WinMerge/MFC activate CoCreateInstance(CLSID_CMultiLanguage, IID_IMultiLanguage) at
 * startup for charset detection/conversion. mlang is a LEAF service (charset tables),
 * not a subsystem, and Wine's builtin mlang PE is a relay stub (0 thunks/forwarders,
 * the shlwapi trap) — so we model the object in HLE rather than lift it. This brick is
 * the ACTIVATION + IUnknown; the 15 interface methods NAME THEMSELVES and abort
 * (instrument-first, doc 81 I5), so a WinMerge rebuild reveals the first one actually
 * called, which is then implemented against the real-Windows/Wine oracle. GUIDs are the
 * full 16 bytes (IMultiLanguage's tail is 9FEA-00AA003F8646, not the IUnknown C000 tail,
 * so u32_iid_eq's Data1+fixed-tail shortcut cannot be used here). */
static const uint8_t IID_IMultiLanguage_b[16] =
    { 0xE1,0x23,0x5C,0x27, 0x47,0x37, 0xD0,0x11, 0x9F,0xEA,0x00,0xAA,0x00,0x3F,0x86,0x46 };
static const uint8_t CLSID_CMultiLanguage_b[16] =
    { 0xE2,0x23,0x5C,0x27, 0x47,0x37, 0xD0,0x11, 0x9F,0xEA,0x00,0xAA,0x00,0x3F,0x86,0x46 };
static int u32_guid_eq(const uint8_t *g, const uint8_t *ref) { return g && memcmp(g, ref, 16) == 0; }

/* Code-page data EXTRACTED from Wine's dlls/mlang/mlang.c (tools/gen_mlang_cp.py) and
 * compiled in — no Wine at runtime. Defines aret_mlang_cps[]. */
#include "mlang_cp_table.h"

/* GDI charset for a family code page, mirroring Wine's fill_cp_info (TranslateCharsetInfo
 * on the family cp; DEFAULT_CHARSET=1 otherwise). Self-contained (u32_tci is defined later
 * in this file), values identical to it. */
static uint32_t u32_family_gdi_charset(uint32_t cp) {
    switch (cp) {
        case 1252: return 0;   /* ANSI */      case 1250: return 238; case 1251: return 204;
        case 1253: return 161; case 1254: return 162; case 1255: return 177;
        case 1256: return 178; case 1257: return 186; case 1258: return 163;
        case 874:  return 222; case 932:  return 128; case 936:  return 134;
        case 949:  return 129; case 950:  return 136; case 1361: return 130;
        default:   return 1;   /* DEFAULT_CHARSET */
    }
}
/* Copy an ASCII string into a guest UTF-16 buffer of `cap` WCHARs (NUL-terminated). */
static void u32_ml_putw(uint16_t *dst, const char *s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) dst[i] = (unsigned char)s[i];
    dst[i] = 0;
}

static uint32_t g_mlang_refs = 1;
static uint32_t g_mlang_vtbl[18];
static uint32_t g_mlang_obj;   /* holds the vtable pointer; the object handed out is &g_mlang_obj */

static uint32_t u32_ml_qi(uint32_t esp) {
    const uint8_t *iid = (const uint8_t *)(uintptr_t)WU(1);
    uint32_t *out = (uint32_t *)(uintptr_t)WU(2);
    if (u32_iid_eq(iid, 0) || u32_guid_eq(iid, IID_IMultiLanguage_b)) {   /* IUnknown | IMultiLanguage */
        if (out) *out = (uint32_t)(uintptr_t)&g_mlang_obj;
        g_mlang_refs++;
        return 0;                                     /* S_OK */
    }
    if (out) *out = 0;                                 /* IMultiLanguage2/3 not modelled yet */
    return 0x80004002u;                                /* E_NOINTERFACE */
}
static uint32_t u32_ml_addref(uint32_t esp)  { (void)esp; return ++g_mlang_refs; }
static uint32_t u32_ml_release(uint32_t esp) { (void)esp; return g_mlang_refs > 1 ? --g_mlang_refs : 1; }
/* Instrument-first stubs: each names itself and aborts, so the first method WinMerge
 * calls is identified by a single rebuild instead of guessed. */
#define ML_STUB(name) static uint32_t u32_ml_##name(uint32_t esp) { (void)esp; \
    aret_unmodelled("IMultiLanguage::" #name); return 0x80004001u; }
/* GetNumberOfCodePageInfo: left instrument-first. Wine's runtime total_cp (73) does not
 * match a naive source count of mlang_data (74) — a value it derives at load time that a
 * static extraction does not reproduce. Rather than ship a count that diverges from the
 * oracle (§0), we do not model it until the discrepancy is understood. (The oracle caught
 * this — the point of verifying.) */
ML_STUB(GetNumberOfCodePageInfo)
/* GetCodePageInfo(uiCodePage, PMIMECPINFO) — fills MIMECPINFO from the Wine-extracted
 * table, mirroring Wine's fnIMultiLanguage_GetCodePageInfo + fill_cp_info exactly (fields
 * at their fixed MSVC offsets; bGDICharset from the family cp). S_OK if found, S_FALSE
 * otherwise (an unknown code page is a defined "not available", not a guess). */
static uint32_t u32_ml_GetCodePageInfo(uint32_t esp) {
    uint32_t cp = WU(1);
    uint8_t *o = (uint8_t *)(uintptr_t)WU(2);
    if (!o) return 0x80070057u;                     /* E_INVALIDARG (never reached in practice) */
    for (unsigned i = 0; i < sizeof(aret_mlang_cps) / sizeof(aret_mlang_cps[0]); i++) {
        const struct aret_mlang_cp *e = &aret_mlang_cps[i];
        if (e->cp != cp) continue;
        memset(o, 0, 572);                           /* sizeof(MIMECPINFO), 4-aligned */
        *(uint32_t *)(o + 0) = e->flags;
        *(uint32_t *)(o + 4) = e->cp;
        *(uint32_t *)(o + 8) = e->family_cp;
        u32_ml_putw((uint16_t *)(o + 12),  e->desc,   64);   /* wszDescription  */
        u32_ml_putw((uint16_t *)(o + 140), e->web,    50);   /* wszWebCharset   */
        u32_ml_putw((uint16_t *)(o + 240), e->header, 50);   /* wszHeaderCharset*/
        u32_ml_putw((uint16_t *)(o + 340), e->body,   50);   /* wszBodyCharset  */
        u32_ml_putw((uint16_t *)(o + 440), e->fixed,  32);   /* wszFixedWidthFont */
        u32_ml_putw((uint16_t *)(o + 504), e->prop,   32);   /* wszProportionalFont */
        o[568] = (uint8_t)u32_family_gdi_charset(e->family_cp);   /* bGDICharset */
        return 0;                                    /* S_OK */
    }
    return 1;                                        /* S_FALSE: unknown code page */
}
/* GetFamilyCodePage(uiCodePage, *puiFamilyCodePage) — ported from Wine's GetFamilyCodePage:
 * scan the table for the code page, return its family code page (S_OK); S_FALSE if the
 * pointer is NULL or the code page is unknown. The table keeps each cp's FIRST family in
 * mlang_data order, matching Wine's first-match. */
static uint32_t u32_ml_GetFamilyCodePage(uint32_t esp) {
    uint32_t cp = WU(1);
    uint32_t *out = (uint32_t *)(uintptr_t)WU(2);
    if (!out) return 1;                              /* S_FALSE */
    for (unsigned i = 0; i < sizeof(aret_mlang_cps) / sizeof(aret_mlang_cps[0]); i++)
        if (aret_mlang_cps[i].cp == cp) { *out = aret_mlang_cps[i].family_cp; return 0; }
    return 1;                                        /* S_FALSE */
}
ML_STUB(EnumCodePages)
ML_STUB(GetCharsetInfo)
ML_STUB(IsConvertible)
ML_STUB(ConvertString)
ML_STUB(ConvertStringToUnicode)
ML_STUB(ConvertStringFromUnicode)
ML_STUB(ConvertStringReset)
ML_STUB(GetRfc1766FromLcid)
ML_STUB(GetLcidFromRfc1766)
ML_STUB(EnumRfc1766)
ML_STUB(GetRfc1766Info)
ML_STUB(CreateConvertCharset)

static uint32_t u32_get_mlang(void) {
    if (!g_mlang_obj) {
        static const struct { uint32_t (*fn)(uint32_t); uint16_t pop; } meth[18] = {
            { u32_ml_qi, 12 }, { u32_ml_addref, 4 }, { u32_ml_release, 4 },
            { u32_ml_GetNumberOfCodePageInfo, 8 }, { u32_ml_GetCodePageInfo, 12 },
            { u32_ml_GetFamilyCodePage, 12 }, { u32_ml_EnumCodePages, 12 },
            { u32_ml_GetCharsetInfo, 12 }, { u32_ml_IsConvertible, 12 },
            { u32_ml_ConvertString, 28 }, { u32_ml_ConvertStringToUnicode, 28 },
            { u32_ml_ConvertStringFromUnicode, 28 }, { u32_ml_ConvertStringReset, 4 },
            { u32_ml_GetRfc1766FromLcid, 12 }, { u32_ml_GetLcidFromRfc1766, 12 },
            { u32_ml_EnumRfc1766, 8 }, { u32_ml_GetRfc1766Info, 12 },
            { u32_ml_CreateConvertCharset, 16 },
        };
        for (int i = 0; i < 18; i++) {
            if (g_delay_res_n >= 64) { aret_unmodelled("mlang: no synthetic VA left for IMultiLanguage vtable"); return 0; }
            uint32_t va = DELAY_VA_BASE + (uint32_t)g_delay_res_n;
            g_delay_res[g_delay_res_n].va = va;
            g_delay_res[g_delay_res_n].shim = meth[i].fn;
            g_delay_res[g_delay_res_n].pop = meth[i].pop;
            g_delay_res_n++;
            g_mlang_vtbl[i] = va;
        }
        g_mlang_obj = (uint32_t)(uintptr_t)g_mlang_vtbl;
    }
    return (uint32_t)(uintptr_t)&g_mlang_obj;
}

uint32_t aret_CoCreateInstance(uint32_t esp) {
    uint32_t rclsid = WU(0), punk = WU(1), ctx = WU(2), riid = WU(3), ppv = WU(4);
    if (!ppv) return 0x80004003u;                     /* E_POINTER */
    *(uint32_t *)(uintptr_t)ppv = 0;                  /* COM: clear on every path */

    /* mlang: served by an HLE object (leaf service; Wine's builtin is a relay stub). */
    if ((ctx & 0x3u) && u32_guid_eq((const uint8_t *)(uintptr_t)rclsid, CLSID_CMultiLanguage_b)) {
        const uint8_t *iid = (const uint8_t *)(uintptr_t)riid;
        if (u32_iid_eq(iid, 0) || u32_guid_eq(iid, IID_IMultiLanguage_b)) {
            uint32_t obj = u32_get_mlang();
            if (!obj) return 0x80004005u;             /* E_FAIL: no VA left */
            *(uint32_t *)(uintptr_t)ppv = obj;
            return 0;                                 /* S_OK */
        }
        return 0x80004002u;                           /* E_NOINTERFACE (IMultiLanguage2/3) */
    }

    /* THE ACTIVATION PATH, served entirely by LIFTED code.
     *
     * There is no CLSID table here, and there must not be one: which class a DLL
     * implements is the DLL's own answer, and hard-coding it would be a per-binary
     * patch (§0.3) that goes stale the moment the module changes. So we do what an
     * in-proc COM loader does minus the registry: ask every lifted module that
     * exports `DllGetClassObject` whether it serves this CLSID. A module that does
     * not returns CLASS_E_CLASSNOTAVAILABLE, which is a real answer from real code,
     * not a guess of ours.
     *
     * Both steps below run lifted: `DllGetClassObject` through the export table,
     * then `IClassFactory::CreateInstance` through the module's OWN vtable, which
     * lives in its rebased data and therefore holds VAs `aret_call` dispatches —
     * the same mechanism as the comctl32 WNDPROC. Nothing about COM needed new
     * machinery, exactly as `IMalloc` did not.
     *
     * Only in-proc contexts are attempted: CLSCTX_LOCAL_SERVER/REMOTE would mean
     * launching another process, which is a sound failure elsewhere in the HLE and
     * must not be silently downgraded to in-proc here. */
    int asked = 0;
    if (ctx & 0x3u) { /* INPROC_SERVER | INPROC_HANDLER */
        const char *dll, *fn;
        uint32_t va;
        for (int k = 0; aret_lifted_export_iter(k, &dll, &fn, &va); k++) {
            if (strcmp(fn, "DllGetClassObject") != 0) continue;
            asked++;
            /* stdcall frame below the caller's esp: [0] return slot, then args. */
            uint32_t frame = (esp - 0x100) & ~15u;
            uint32_t *fr = (uint32_t *)(uintptr_t)frame;
            uint32_t cf = 0;
            fr[0] = 0;
            fr[1] = rclsid;
            fr[2] = (uint32_t)(uintptr_t)u32_iid_classfactory;
            fr[3] = (uint32_t)(uintptr_t)&cf;
            uint32_t hr = (uint32_t)aret_call(va, frame, 0, 0, 0, 0, 0, 0, 0);
            if (hr != 0 || !cf) continue;      /* this module does not serve it */

            uint32_t vtbl = *(const uint32_t *)(uintptr_t)cf;
            uint32_t create  = *(const uint32_t *)(uintptr_t)(vtbl + 3 * 4);
            uint32_t release = *(const uint32_t *)(uintptr_t)(vtbl + 2 * 4);
            fr[0] = 0;
            fr[1] = cf;                        /* this */
            fr[2] = punk;                      /* pUnkOuter (aggregation) */
            fr[3] = riid;
            fr[4] = ppv;
            hr = (uint32_t)aret_call(create, frame, 0, 0, 0, 0, 0, 0, 0);
            /* The factory is a separate object with its own lifetime; the caller
             * only ever learns about the instance, so release it here or it leaks
             * a reference that keeps the module pinned. */
            fr[0] = 0;
            fr[1] = cf;
            (void)aret_call(release, frame, 0, 0, 0, 0, 0, 0, 0);
            return hr;
        }
    }

    /* Nobody served it. Still an abort naming what was asked for — and never
     * REGDB_E_CLASSNOTREG, which is a DEFINED failure but not a sound one: on a
     * real system the class IS registered, so answering "not registered" would run
     * the program down an error path it never takes. */
    char msg[224], c[48], i[48];
    u32_fmt_guid(c, (const uint8_t *)(uintptr_t)rclsid);
    u32_fmt_guid(i, (const uint8_t *)(uintptr_t)riid);
    snprintf(msg, sizeof msg,
             "CoCreateInstance class %s as %s (ctx %u); %d lifted module(s) offered "
             "DllGetClassObject and none served it", c, i, (unsigned)ctx, asked);
    aret_unmodelled(msg);
    return 0x80004005u;   /* E_FAIL, never reached: aret_unmodelled aborts */
}

/* ResolveDelayLoadedAPI(base, descriptor, failDllHook, failSysHook, thunk, flags)
 * — parse the IMAGE_DELAYLOAD_DESCRIPTOR (RVA-based), resolve the API to a
 * synthetic VA (patching the delay-IAT slot so later calls dispatch directly),
 * and return it. A modelled API -> its synthetic VA; anything else -> named hard
 * abort (never a 0 pointer / silent wrong). */
uint32_t aret_ResolveDelayLoadedAPI(uint32_t esp) {
    uint32_t base = WU(0);
    const uint8_t *d = (const uint8_t *)WP(1);
    uint32_t thunk = WU(4);
    char m[160];
    if (!d) { aret_unmodelled("ResolveDelayLoadedAPI (no descriptor)"); return 0; }
    const char *dll = (const char *)(uintptr_t)(base + *(const uint32_t *)(d + 4));
    int idx = (int)((thunk - (base + *(const uint32_t *)(d + 12))) / 4);
    const uint32_t *intt = (const uint32_t *)(uintptr_t)(base + *(const uint32_t *)(d + 16));
    const char *fn = (const char *)(uintptr_t)(base + intt[idx] + 2);
    for (int i = 0; k_delay[i].dll; i++) {
        if (u32_stricmp_(dll, k_delay[i].dll) && strcmp(fn, k_delay[i].fn) == 0) {
            /* find or assign a synthetic VA for this (dll, fn) */
            for (int j = 0; j < g_delay_res_n; j++)
                if (g_delay_res[j].shim == k_delay[i].shim && g_delay_res[j].pop == k_delay[i].pop) {
                    *(uint32_t *)(uintptr_t)thunk = g_delay_res[j].va;
                    return g_delay_res[j].va;
                }
            if (g_delay_res_n >= 64) break;
            uint32_t va = DELAY_VA_BASE + (uint32_t)g_delay_res_n;
            g_delay_res[g_delay_res_n].va = va;
            g_delay_res[g_delay_res_n].shim = k_delay[i].shim;
            g_delay_res[g_delay_res_n].pop = k_delay[i].pop;
            g_delay_res_n++;
            *(uint32_t *)(uintptr_t)thunk = va; /* patch the delay-IAT slot */
            return va;
        }
    }
    /* Not in the small curated list above — but the HLE may implement it anyway.
     * The generated name table (aret_dispatch.c) covers every shim, so a delay-load
     * reaches exactly what a direct import would, with the same stdcall pop. Before
     * this, Wine's lifted shell32 aborted on `ole32.CoTaskMemAlloc` — an API we HAVE:
     * the resolver simply could not see it, and two lists of "APIs we model" had
     * drifted apart. Matching by function name only, as the static import path does:
     * a Win32 API name identifies its behaviour regardless of which DLL forwards it. */
    {
        uint32_t (*shim)(uint32_t) = 0;
        uint16_t pop = 0;
        if (aret_hle_shim_lookup(fn, &shim, &pop)) {
            for (int j = 0; j < g_delay_res_n; j++)
                if (g_delay_res[j].shim == shim) {
                    *(uint32_t *)(uintptr_t)thunk = g_delay_res[j].va;
                    return g_delay_res[j].va;
                }
            if (g_delay_res_n < 64) {
                uint32_t va = DELAY_VA_BASE + (uint32_t)g_delay_res_n;
                g_delay_res[g_delay_res_n].va = va;
                g_delay_res[g_delay_res_n].shim = shim;
                g_delay_res[g_delay_res_n].pop = pop;
                g_delay_res_n++;
                *(uint32_t *)(uintptr_t)thunk = va;
                return va;
            }
        }
    }
    snprintf(m, sizeof m, "delay-load %s.%s (not modelled)", dll, fn);
    aret_unmodelled(m);
    return 0;
}
/* DisableThreadLibraryCalls(hModule) -> BOOL: opt out of DLL_THREAD_ATTACH/DETACH
 * notifications. We don't deliver those to lifted DllMains anyway, so it's a
 * sound success no-op. (Called by comctl32's DllMain at process attach.) */
uint32_t aret_DisableThreadLibraryCalls(uint32_t esp) { (void)esp; return 1; }

/* ---- Bitmap resources: LoadBitmap / LoadImage(IMAGE_BITMAP) ---- */
/* Decode a packed DIB (BITMAPINFOHEADER + palette + bits, no BITMAPFILEHEADER)
 * from an RT_BITMAP resource into an internal 32bpp HBITMAP the GDI model can
 * blit. This is how real GUI apps load their toolbar/UI images (then hand them to
 * ImageList). BI_RGB 1/4/8/24/32 bpp; other compression aborts sound. */
static uint32_t u32_load_dib_resource(uint32_t name_ref) {
    const uint8_t *de = u32_rsrc_data_entry(2 /* RT_BITMAP */, name_ref);
    if (!de) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
    uint32_t hdrsize = *(const uint32_t *)p;
    if (hdrsize < 40) return 0;                             /* need a BITMAPINFOHEADER */
    int32_t w = *(const int32_t *)(p + 4), hh = *(const int32_t *)(p + 8);
    uint16_t bitcount = *(const uint16_t *)(p + 14);
    uint32_t comp = *(const uint32_t *)(p + 16), clrused = *(const uint32_t *)(p + 32);
    if (comp != 0) { aret_partial("LoadBitmap: only BI_RGB (uncompressed) DIB modelled"); return 0; }
    if (bitcount != 1 && bitcount != 4 && bitcount != 8 && bitcount != 24 && bitcount != 32) {
        aret_partial("LoadBitmap: only 1/4/8/24/32 bpp modelled"); return 0;
    }
    if (w <= 0 || w > 8192) return 0;
    int topdown = hh < 0, h = topdown ? -hh : hh;
    if (h <= 0 || h > 8192) return 0;
    const uint8_t *pal = p + hdrsize;
    uint32_t ncol = (bitcount <= 8) ? (clrused ? clrused : (1u << bitcount)) : 0;
    const uint8_t *px = pal + (size_t)ncol * 4;
    int stride = ((w * bitcount + 31) / 32) * 4;
    int bi = gdi_alloc(GDIT_BITMAP); if (!bi) return 0;
    g_gdi[bi].w = w; g_gdi[bi].h = h; g_gdi[bi].topdown = 1; g_gdi[bi].bpp = 32;
    g_gdi[bi].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[bi].owns_bits = 1;
    if (!g_gdi[bi].bits) { g_gdi[bi].used = 0; return 0; }
    for (int y = 0; y < h; y++) {
        int srow = topdown ? y : (h - 1 - y);
        const uint8_t *row = px + (size_t)srow * stride;
        uint8_t *dst = g_gdi[bi].bits + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            uint8_t b8 = 0, g8 = 0, r8 = 0;
            if (bitcount <= 8) {
                uint32_t idx = (bitcount == 1) ? ((row[x >> 3] >> (7 - (x & 7))) & 1)
                            : (bitcount == 4) ? ((x & 1) ? (row[x >> 1] & 0xF) : (row[x >> 1] >> 4))
                            : row[x];
                if (idx < ncol) { b8 = pal[idx * 4]; g8 = pal[idx * 4 + 1]; r8 = pal[idx * 4 + 2]; }
            } else if (bitcount == 24) { b8 = row[x * 3]; g8 = row[x * 3 + 1]; r8 = row[x * 3 + 2]; }
            else                       { b8 = row[x * 4]; g8 = row[x * 4 + 1]; r8 = row[x * 4 + 2]; }
            dst[x * 4] = b8; dst[x * 4 + 1] = g8; dst[x * 4 + 2] = r8; dst[x * 4 + 3] = 0;
        }
    }
    return gdi_handle(bi);
}
uint32_t aret_LoadBitmapA(uint32_t esp) { return u32_load_dib_resource(WU(1)); }
uint32_t aret_LoadBitmapW(uint32_t esp) { return u32_load_dib_resource(WU(1)); }  /* MAKEINTRESOURCE ids */
/* LoadImageA(hInst, name, type, cx, cy, fuLoad) -> HANDLE. IMAGE_BITMAP from a
 * resource is the modelled case; icons/cursors and LR_LOADFROMFILE are not yet
 * (return 0 = sound "not loaded", never a bogus handle). */
uint32_t aret_LoadImageA(uint32_t esp) {
    if (WU(2) == 0 /* IMAGE_BITMAP */ && !(WU(5) & 0x0010u /* LR_LOADFROMFILE */))
        return u32_load_dib_resource(WU(1));
    return 0;
}
uint32_t aret_LoadImageW(uint32_t esp) { return aret_LoadImageA(esp); }

/* ------------------------------------------------------------------ */
/* GDI text raster (G3-text) — FreeType, bit-identical to Wine          */
/* ------------------------------------------------------------------ */
#ifdef ARET_HAVE_FREETYPE
static FT_Library g_ft_lib;
static int g_ft_ready = -1;   /* -1 unknown, 0 failed, 1 ready */
static int ft_ensure(void) {
    if (g_ft_ready >= 0) return g_ft_ready;
    g_ft_ready = 0;
    if (FT_Init_FreeType(&g_ft_lib) == 0 && FcInit()) {
        /* Wine's subpixel (ClearType) path uses FreeType's default LCD filter. */
        FT_Library_SetLcdFilter(g_ft_lib, FT_LCD_FILTER_DEFAULT);
        g_ft_ready = 1;
    }
    return g_ft_ready;
}
/* Alpha-over one channel: dst = round((fg*cov + old*(255-cov)) / 255). Matches
 * Wine's GDI text blend (verified against measured RGB on coloured cases). */
static inline uint8_t u32_blend1(int fg, int old, int cov) {
    return (uint8_t)((fg * cov + old * (255 - cov) + 127) / 255);
}
/* Map the classic Windows UI *sans* face names to their metric-compatible
 * replacement (Liberation Sans) — the same substitution Wine applies. fontconfig
 * alone routes these to its generic default (DejaVu), which diverges from Wine for
 * the exact faces real GUI apps use (MS Sans Serif, MS Shell Dlg, Tahoma…). Only
 * the sans UI family is remapped here; serif/mono legacy names keep fontconfig's
 * (correct) metric-compatible answer (Times→Liberation Serif, etc.). Case-
 * insensitive; returns the original face when not a known UI-sans name. */
static const char *u32_face_subst(const char *face) {
    if (!face || !face[0]) return face;
    static const char *ui_sans[] = {
        "MS Sans Serif", "MS Shell Dlg", "MS Shell Dlg 2", "Microsoft Sans Serif",
        "Tahoma", "Helv", "Helvetica", "System", "Segoe UI", "Arial", NULL };
    for (int i = 0; ui_sans[i]; i++) {
        const char *a = face, *b = ui_sans[i];
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) { a++; b++; }
        if (!*a && !*b) return "Liberation Sans";
    }
    return face;
}
/* Resolve a logical face name to a font file path with fontconfig — the same
 * mechanism Wine uses on Linux, so we pick the same file Wine picks (Arial→
 * Liberation Sans, etc.). Returns 1 + fills `out` on success. */
static int ft_resolve_face(const char *face, int bold, int italic, char *out, size_t outsz) {
    face = u32_face_subst(face);
    FcPattern *pat = FcPatternCreate();
    if (!pat) return 0;
    if (face && face[0]) FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)face);
    FcPatternAddInteger(pat, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    int ok = 0;
    if (m) {
        FcChar8 *file = NULL;
        if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch && file) {
            strncpy(out, (const char *)file, outsz - 1); out[outsz - 1] = 0; ok = 1;
        }
        FcPatternDestroy(m);
    }
    FcPatternDestroy(pat);
    return ok;
}
/* Face cache keyed by path (avoid reparsing the TTF per TextOut). */
static struct { char path[256]; FT_Face face; } g_face_cache[8];
static int g_face_n;
static FT_Face ft_get_face(const char *path) {
    for (int i = 0; i < g_face_n; i++)
        if (strcmp(g_face_cache[i].path, path) == 0) return g_face_cache[i].face;
    if (g_face_n >= (int)(sizeof g_face_cache / sizeof g_face_cache[0])) return NULL;
    FT_Face f;
    if (FT_New_Face(g_ft_lib, path, 0, &f) != 0) return NULL;
    strncpy(g_face_cache[g_face_n].path, path, sizeof g_face_cache[0].path - 1);
    g_face_cache[g_face_n].path[sizeof g_face_cache[0].path - 1] = 0;
    g_face_cache[g_face_n].face = f;
    return g_face_cache[g_face_n++].face;
}
#endif /* ARET_HAVE_FREETYPE */

#ifdef ARET_HAVE_FREETYPE
/* When set, u32_dc_font returns NULL *quietly* on an unresolvable/unmodelled font
 * instead of aborting — used by best-effort callers (dialog base units) where a
 * missing font just leaves the metric unavailable rather than being a hard error. */
static int g_dc_font_quiet;
static FT_Face u32_dc_font_fail(const char *msg) { if (!g_dc_font_quiet) aret_partial(msg); return NULL; }
/* Resolve the DC's selected font to an FT_Face sized to its LOGFONT, plus Wine's
 * tmAscent/tmDescent (from OS/2 usWinAscent/usWinDescent scaled). Returns the face
 * (NULL ⇒ a sound abort, `aret_unimpl` already called). Shared by TextOut and the
 * text-measurement APIs so they agree exactly. Gates the subset we render/measure
 * exactly: real face name, regular upright weight, OS/2 table present. Quality,
 * alignment and background are *render-only* concerns handled by TextOut itself. */
static FT_Face u32_dc_font(int d, int *ascent, int *descent) {
    int fi = gdi_idx(g_gdi[d].sel_font);
    int height = (fi >= 0) ? g_gdi[fi].lf_height : 0;
    int weight = (fi >= 0) ? g_gdi[fi].lf_weight : 0;
    int italic = (fi >= 0) ? g_gdi[fi].lf_italic : 0;
    const char *face = (fi >= 0) ? g_gdi[fi].lf_face : "";
    int bold = weight >= 700;   /* FW_BOLD; fontconfig picks the bold/italic face */
    if (!face[0]) return u32_dc_font_fail("GDI text: stock font (no face name) pending");
    if (!ft_ensure()) return u32_dc_font_fail("GDI text: FreeType/fontconfig init failed");
    char path[256];
    if (!ft_resolve_face(face, bold, italic ? 1 : 0, path, sizeof path)) return u32_dc_font_fail("GDI text: face not resolvable by fontconfig");
    FT_Face ftf = ft_get_face(path);
    if (!ftf) return u32_dc_font_fail("GDI text: font file load failed");
    /* Real bold/italic faces render bit-exactly. When no real face exists, Wine
     * *synthesizes* the style (embolden / oblique shear with a specific matrix);
     * replicating that exactly is a follow-up, so abort soundly rather than render
     * an upright/unemboldened glyph (which would be silently wrong). */
    if (bold && !(ftf->style_flags & FT_STYLE_FLAG_BOLD))
        return u32_dc_font_fail("GDI text: synthesized bold (no real bold face) pending");
    if (italic && !(ftf->style_flags & FT_STYLE_FLAG_ITALIC))
        return u32_dc_font_fail("GDI text: synthesized italic (no real italic face) pending");
    TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(ftf, FT_SFNT_OS2);
    if (!os2 || os2->version == 0xFFFF) return u32_dc_font_fail("GDI text: font has no OS/2 table (metrics undefined)");
    /* ppem from LOGFONT height: negative = em/character height (ppem = |height|);
     * positive = cell height → Wine maps ppem = round(height·upm/(winAsc+winDesc))
     * so the resulting tmHeight equals the requested cell height (measured: for
     * Liberation Sans, height 16 → ppem 14). */
    int upm = ftf->units_per_EM ? ftf->units_per_EM : 2048;
    int ppem;
    if (height < 0) ppem = -height;
    else if (height > 0) {
        int cell = os2->usWinAscent + os2->usWinDescent;
        ppem = cell ? (height * upm + cell / 2) / cell : height;
    } else ppem = 16;
    if (ppem <= 0) ppem = 1;
    if (FT_Set_Pixel_Sizes(ftf, 0, (FT_UInt)ppem) != 0) return u32_dc_font_fail("GDI text: set pixel size failed");
    FT_Fixed ys = ftf->size->metrics.y_scale;
    if (ascent)  *ascent  = (int)((FT_MulFix(os2->usWinAscent,  ys) + 32) >> 6);
    if (descent) *descent = (int)((FT_MulFix(os2->usWinDescent, ys) + 32) >> 6);
    return ftf;
}
/* Text extent width (pixels) = sum of the *default*-hinted advances — Wine's
 * GetTextExtentPoint32 / opaque-fill regime, which is distinct from the mono
 * render-pen advance (measured: 'Hi!' extent 22 but mono pen sum 21). Codepoints
 * (not bytes) so wide text with real Unicode works. */
static int u32_text_width(FT_Face f, const uint32_t *cps, int len) {
    int w = 0;
    for (int i = 0; i < len; i++)
        if (FT_Load_Char(f, cps[i], FT_LOAD_DEFAULT) == 0)
            w += (int)(f->glyph->advance.x >> 6);
    return w;
}
#endif /* ARET_HAVE_FREETYPE */

/* ANSI byte → Unicode codepoint via CP1252 (Windows ACP, verified GetACP()=1252):
 * ASCII and 0xA0-0xFF are identical to Latin-1; 0x80-0x9F are the CP1252-specific
 * slots (€, curly quotes, dashes…). Undefined slots (0x81/0x8D/0x8F/0x90/0x9D) map
 * to the byte value (as MultiByteToWideChar does). */
static uint32_t u32_ansi_cp(unsigned char b) {
    static const uint16_t hi[32] = {
        0x20AC,0x0081,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
        0x02C6,0x2030,0x0160,0x2039,0x0152,0x008D,0x017D,0x008F,
        0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x02DC,0x2122,0x0161,0x203A,0x0153,0x009D,0x017E,0x0178 };
    return (b >= 0x80 && b <= 0x9F) ? hi[b - 0x80] : (uint32_t)b;
}
/* Shared ANSI(CP1252) -> UTF-16 conversion: the SINGLE source of truth for kernel32's
 * MultiByteToWideChar (CP_ACP) and the ntdll Rtl*ToUnicode floor (runtime/wine_heavy).
 * Full CP1252 via u32_ansi_cp -> bit-identical Wine on every byte 0x00-0xFF, not just ASCII.
 * Non-static so the separately-compiled Wine floor object links against it. */
void aret_cp1252_to_wc(uint16_t *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = (uint16_t)u32_ansi_cp((unsigned char)src[i]);
}
/* Reverse: UTF-16 -> ANSI(CP1252) with Wine's exact best-fit, MEASURED (tools/gen_cp1252.py:
 * sweep of WideCharToMultiByte(CP_ACP) over all 65536 code points). A code point absent from
 * the table maps to the default char '?' (0x3F) — exactly Wine's lpUsedDefaultChar path.
 * The SINGLE source of truth for WideCharToMultiByte(CP_ACP) and the ntdll Rtl*ToMultiByte
 * floor; bit-identical Wine (kernel32 == ntdll, measured). */
#include "cp1252_rev_table.h"
static int aret_cp1252_rev_byte(uint16_t cp) { /* byte, or -1 if unmappable */
    int lo = 0, hi = (int)(sizeof aret_cp1252_rev_tab / sizeof aret_cp1252_rev_tab[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2; uint16_t c = aret_cp1252_rev_tab[m].cp;
        if (c == cp) return aret_cp1252_rev_tab[m].b;
        if (c < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
/* Returns 1 if any code point was unmappable (substituted with the default char '?') — this is
 * exactly Wine's lpUsedDefaultChar. U+003F itself IS in the table, so it does NOT set the flag. */
int aret_cp1252_from_wc(char *dst, const uint16_t *src, int n) {
    int used = 0;
    for (int i = 0; i < n; i++) {
        int b = aret_cp1252_rev_byte(src[i]);
        if (b < 0) { b = 0x3F; used = 1; }
        dst[i] = (char)b;
    }
    return used;
}
/* Same, for the OEM code page (CP437, GetOEMCP()=437) — measured tables (tools/gen_cp437.py).
 * The single source of truth for MultiByteToWideChar/WideCharToMultiByte(CP_OEMCP) and the
 * ntdll Rtl*Oem* floor. Bit-identical Wine incl. best-fit + default char. */
#include "cp437_tables.h"
void aret_cp437_to_wc(uint16_t *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = aret_cp437_fwd[(unsigned char)src[i]];
}
static int aret_cp437_rev_byte(uint16_t cp) {
    int lo = 0, hi = (int)(sizeof aret_cp437_rev_tab / sizeof aret_cp437_rev_tab[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2; uint16_t c = aret_cp437_rev_tab[m].cp;
        if (c == cp) return aret_cp437_rev_tab[m].b;
        if (c < cp) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
int aret_cp437_from_wc(char *dst, const uint16_t *src, int n) {
    int used = 0;
    for (int i = 0; i < n; i++) {
        int b = aret_cp437_rev_byte(src[i]);
        if (b < 0) { b = 0x3F; used = 1; }
        dst[i] = (char)b;
    }
    return used;
}
/* Render an ANSI string with the DC's selected font into the DC's 32bpp DIB
 * surface, bit-identically to Wine: FreeType mono raster (the rasterizer Wine
 * uses) + Wine's usWinAscent baseline + mono blend; OPAQUE background fills the
 * cell rect [x,y .. x+extentWidth, y+tmHeight] with bkColor first (measured Wine
 * behaviour). Returns 1 on success. Outside the proven-exact subset it aborts
 * *soundly* (never a silent wrong render): no FreeType linked, antialiased/bold/
 * italic, non-default alignment, stock font (no face), non-32bpp target. Each is a
 * follow-up increment, verified against Wine before it ships. */
static int u32_textout_full(uint32_t hdc, int x, int y, const uint32_t *cps, int len,
                            const int32_t *dx, const int32_t *rect, int do_opaque, int do_clip) {
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *surf = gdi_dc_surface(hdc);
    if (!surf || surf->bpp != 32) { aret_partial("TextOut: only 32bpp DIB target modelled"); return 0; }
    int fi = gdi_idx(g_gdi[d].sel_font);
    int quality = (fi >= 0) ? g_gdi[fi].lf_quality : 0;
    /* Wine's render mode by CreateFont quality (measured): NONANTIALIASED→mono,
     * ANTIALIASED→grayscale, DEFAULT/DRAFT/PROOF/CLEARTYPE→subpixel LCD (ClearType).
     * The subpixel path is FreeType's LCD render (the same rasterizer + default LCD
     * filter Wine uses); RGB values match Wine bit-for-bit. */
    enum { AA_MONO, AA_GRAY, AA_LCD };
    int aa = (quality == 3) ? AA_MONO : (quality == 4) ? AA_GRAY : AA_LCD;
    /* Grayscale ANTIALIASED_QUALITY renders differently from FreeType's NORMAL/LIGHT
     * (Wine's grayscale has harder edges — a distinct pipeline) → abort soundly until
     * matched. Mono and subpixel (the DEFAULT/ClearType path) are bit-exact. */
    if (aa == AA_GRAY) { aret_partial("TextOut: grayscale ANTIALIASED_QUALITY pending (mono + subpixel done)"); return 0; }
    int load_target = (aa == AA_MONO) ? FT_LOAD_TARGET_MONO
                    : (aa == AA_GRAY) ? FT_LOAD_TARGET_NORMAL : FT_LOAD_TARGET_LCD;
    uint32_t ta = g_gdi[d].text_align;
    if (ta & ~0x1Eu) { aret_partial("TextOut: TA_UPDATECP/RTL alignment pending"); return 0; }  /* only TA_{LEFT,RIGHT,CENTER,TOP,BASELINE,BOTTOM} */
    int bkmode = g_gdi[d].bk_mode;
    if (bkmode != 1 /*TRANSPARENT*/ && bkmode != 2 /*OPAQUE*/) { aret_partial("TextOut: unknown background mode"); return 0; }
    int ascent, descent;
    FT_Face ftf = u32_dc_font(d, &ascent, &descent);
    if (!ftf) return 0;
    /* ETO_OPAQUE: fill the *explicit* rectangle with bkColor first (independent of
     * the bkmode cell fill below). */
    if (rect && do_opaque) {
        uint32_t bg = g_gdi[d].bk_color;
        for (int Y = rect[1]; Y < rect[3]; Y++)
            for (int X = rect[0]; X < rect[2]; X++)
                if (X >= 0 && X < surf->w && Y >= 0 && Y < surf->h) gdi_put(surf, X, Y, bg);
    }
    /* Text width: sum of per-char spacing (lpDx) if given, else the extent. */
    int need_width = (ta & 6) || (bkmode == 2);
    int width = 0;
    if (dx) { for (int k = 0; k < len; k++) width += dx[k]; }
    else if (need_width) width = u32_text_width(ftf, cps, len);
    /* Alignment origin (Wine's rules, measured): horizontal LEFT=x, RIGHT=x-width,
     * CENTER=x-width/2; vertical TOP baseline=y+ascent, BASELINE=y, BOTTOM=y-descent. */
    int penx = x;
    if ((ta & 6) == 2) penx = x - width;              /* TA_RIGHT  */
    else if ((ta & 6) == 6) penx = x - width / 2;     /* TA_CENTER */
    int baseline;
    if ((ta & 24) == 8) baseline = y - descent;       /* TA_BOTTOM   */
    else if ((ta & 24) == 24) baseline = y;           /* TA_BASELINE */
    else baseline = y + ascent;                       /* TA_TOP      */
    /* OPAQUE bkmode: fill the (aligned) text cell rectangle with bkColor. */
    if (bkmode == 2) {
        int top = baseline - ascent;
        uint32_t bg = g_gdi[d].bk_color;
        for (int Y = top; Y < top + ascent + descent; Y++)
            for (int X = penx; X < penx + width; X++)
                if (X >= 0 && X < surf->w && Y >= 0 && Y < surf->h) gdi_put(surf, X, Y, bg);
    }
    /* ETO_CLIPPED: restrict glyph pixels to the rectangle. */
    int cl = rect && do_clip ? rect[0] : 0, ct = rect && do_clip ? rect[1] : 0;
    int cr = rect && do_clip ? rect[2] : surf->w, cb = rect && do_clip ? rect[3] : surf->h;
    uint32_t fg = g_gdi[d].text_color;   /* 0x00BBGGRR = (fg_R, fg_G, fg_B) low→high */
    int fR = fg & 0xFF, fG = (fg >> 8) & 0xFF, fB = (fg >> 16) & 0xFF;
    int penx0 = penx;   /* start of the run — underline/strikeout span [penx0, penx) */
    for (int k = 0; k < len; k++) {
        if (FT_Load_Char(ftf, cps[k], FT_LOAD_RENDER | load_target) != 0) { if (dx) penx += dx[k]; continue; }
        FT_GlyphSlot g = ftf->glyph; FT_Bitmap *b = &g->bitmap;
        int ox = penx + g->bitmap_left, oy = baseline - g->bitmap_top;
        int pxw = (aa == AA_LCD) ? (int)b->width / 3 : (int)b->width;   /* LCD bitmap is 3× wide */
        for (int r = 0; r < (int)b->rows; r++)
            for (int c = 0; c < pxw; c++) {
                int X = ox + c, Y = oy + r;
                if (!(X >= cl && X < cr && Y >= ct && Y < cb && X >= 0 && X < surf->w && Y >= 0 && Y < surf->h)) continue;
                uint8_t *p = gdi_px(surf, X, Y);   /* DIB bytes [B,G,R,0] */
                if (!p) continue;
                if (aa == AA_MONO) {
                    if ((b->buffer[r * b->pitch + (c >> 3)] >> (7 - (c & 7))) & 1) { p[0] = (uint8_t)fB; p[1] = (uint8_t)fG; p[2] = (uint8_t)fR; }
                } else if (aa == AA_GRAY) {
                    int cov = b->buffer[r * b->pitch + c];
                    if (cov) { p[2] = u32_blend1(fR, p[2], cov); p[1] = u32_blend1(fG, p[1], cov); p[0] = u32_blend1(fB, p[0], cov); }
                } else { /* AA_LCD: 3 subpixel coverages (R,G,B) per pixel */
                    int covR = b->buffer[r * b->pitch + c * 3 + 0];
                    int covG = b->buffer[r * b->pitch + c * 3 + 1];
                    int covB = b->buffer[r * b->pitch + c * 3 + 2];
                    if (covR | covG | covB) { p[2] = u32_blend1(fR, p[2], covR); p[1] = u32_blend1(fG, p[1], covG); p[0] = u32_blend1(fB, p[0], covB); }
                }
            }
        penx += dx ? dx[k] : (int)(g->advance.x >> 6);   /* lpDx spacing, else pen advance */
    }
    /* Underline / strikeout bars (lfUnderline / lfStrikeOut): solid fg rectangles
     * spanning the run [penx0, penx). Positions match Wine's OUTLINETEXTMETRIC:
     * underline top = baseline - round(MulFix(post.underline_pos + thickness/2, ys));
     * strikeout top = baseline - round(MulFix(OS/2 yStrikeoutPosition, ys)); both
     * thicknesses from the scaled font metrics, min 1px. */
    int f_ul = (fi >= 0) ? g_gdi[fi].lf_underline : 0;
    int f_so = (fi >= 0) ? g_gdi[fi].lf_strikeout : 0;
    /* The bar spans the text *extent* (default advances) from the run origin, not
     * the mono pen advance (they differ by ~1px at small sizes). With lpDx the span
     * follows the dx-driven pen. */
    int span_end = dx ? penx : penx0 + u32_text_width(ftf, cps, len);
    if ((f_ul || f_so) && span_end > penx0) {
        FT_Fixed ys = ftf->size->metrics.y_scale;
        TT_OS2 *os2b = (TT_OS2 *)FT_Get_Sfnt_Table(ftf, FT_SFNT_OS2);
        struct { int top, thk; } bars[2]; int nb = 0;
        if (f_ul) {
            int thk = (int)((FT_MulFix(ftf->underline_thickness, ys) + 32) >> 6); if (thk < 1) thk = 1;
            int pos = (int)((FT_MulFix(ftf->underline_position + ftf->underline_thickness / 2, ys) + 32) >> 6);
            bars[nb].top = baseline - pos; bars[nb].thk = thk; nb++;
        }
        if (f_so && os2b) {
            int thk = (int)((FT_MulFix(os2b->yStrikeoutSize, ys) + 32) >> 6); if (thk < 1) thk = 1;
            int pos = (int)((FT_MulFix(os2b->yStrikeoutPosition, ys) + 32) >> 6);
            bars[nb].top = baseline - pos; bars[nb].thk = thk; nb++;
        }
        for (int bi = 0; bi < nb; bi++)
            for (int Y = bars[bi].top; Y < bars[bi].top + bars[bi].thk; Y++)
                for (int X = penx0; X < span_end; X++)
                    if (X >= cl && X < cr && Y >= ct && Y < cb && X >= 0 && X < surf->w && Y >= 0 && Y < surf->h)
                        gdi_put(surf, X, Y, fg);
    }
    return 1;
#else
    (void)hdc; (void)x; (void)y; (void)cps; (void)len; (void)dx; (void)rect; (void)do_opaque; (void)do_clip;
    aret_partial("TextOut: FreeType not linked (rebuild with freetype2+fontconfig)");
    return 0;
#endif
}
static int u32_textout_core(uint32_t hdc, int x, int y, const uint32_t *cps, int len) {
    return u32_textout_full(hdc, x, y, cps, len, NULL, NULL, 0, 0);
}
/* ExtTextOut shared path: parse options/rect/dx, then render. `wide` picks A vs W
 * for the string; only ETO_OPAQUE|ETO_CLIPPED are modelled (glyph-index/PDY/RTL/
 * numeric-shaping abort soundly). */
static int u32_exttextout(uint32_t hdc, int x, int y, uint32_t opts, uint32_t prect,
                          const void *str, int count, uint32_t pdx, int wide) {
    if (opts & ~0x6u) { aret_partial("ExtTextOut: only ETO_OPAQUE|ETO_CLIPPED modelled"); return 0; }
    if (count < 0) count = 0;
    uint32_t cps[1024]; int m = count < 1024 ? count : 1024;
    if (str) {
        if (wide) { const uint16_t *w = (const uint16_t *)str; for (int i = 0; i < m; i++) cps[i] = w[i]; }
        else      { const uint8_t  *b = (const uint8_t  *)str; for (int i = 0; i < m; i++) cps[i] = u32_ansi_cp(b[i]); }
    } else m = 0;
    int32_t rectbuf[4]; const int32_t *rect = NULL;
    if (prect) { const int32_t *r = (const int32_t *)(uintptr_t)prect; rectbuf[0]=r[0];rectbuf[1]=r[1];rectbuf[2]=r[2];rectbuf[3]=r[3]; rect = rectbuf; }
    int32_t dxbuf[1024]; const int32_t *dx = NULL;
    if (pdx) { const int32_t *p = (const int32_t *)(uintptr_t)pdx; for (int i = 0; i < m; i++) dxbuf[i] = p[i]; dx = dxbuf; }
    return u32_textout_full(hdc, x, y, cps, m, dx, rect, (opts & 2) != 0, (opts & 4) != 0);
}
uint32_t aret_ExtTextOutA(uint32_t esp) {
    return u32_exttextout(WU(0), WI(1), WI(2), WU(3), WU(4), WP(5), WI(6), WU(7), 0) ? 1 : 0;
}
uint32_t aret_ExtTextOutW(uint32_t esp) {
    return u32_exttextout(WU(0), WI(1), WI(2), WU(3), WU(4), WP(5), WI(6), WU(7), 1) ? 1 : 0;
}

/* DrawText{A,W}(hdc, text, count, lprc, format) -> text height. Single-line layout
 * within lprc (measured Wine rules): horizontal DT_LEFT/CENTER/RIGHT, vertical
 * DT_TOP/VCENTER/BOTTOM, DT_CALCRECT (measure only). Returns the text height, or
 * for DT_VCENTER/DT_BOTTOM the offset from lprc->top to the text bottom. Outside
 * the modelled subset aborts *soundly*: multi-line/word-break, tabs, ellipsis, and
 * '&' prefix processing (without DT_NOPREFIX). */
static uint32_t u32_drawtext(uint32_t hdc, const uint32_t *cps, int len, uint32_t prc, uint32_t fmt) {
#ifdef ARET_HAVE_FREETYPE
    enum { DT_CENTER=1, DT_RIGHT=2, DT_VCENTER=4, DT_BOTTOM=8, DT_WORDBREAK=0x10, DT_SINGLELINE=0x20,
           DT_EXPANDTABS=0x40, DT_TABSTOP=0x80, DT_NOCLIP=0x100, DT_CALCRECT=0x400, DT_NOPREFIX=0x800 };
    if (fmt & (DT_EXPANDTABS|DT_TABSTOP|0x8000/*DT_END_ELLIPSIS*/|0x40000/*DT_PATH_ELLIPSIS*/|0x20000/*DT_WORD_ELLIPSIS*/))
        { aret_partial("DrawText: tabs/ellipsis pending"); return 0; }
    /* '&' prefix (unless DT_NOPREFIX): a single '&' is removed and marks the next
     * char as the underlined accelerator; '&&' is a literal '&'; a trailing '&' is
     * dropped. Single-line only (accelerators in wrapped text are rare → abort). */
    int acc = -1;
    uint32_t pcps[1024];
    if (!(fmt & DT_NOPREFIX)) {
        int has_amp = 0; for (int i = 0; i < len; i++) if (cps[i] == '&') { has_amp = 1; break; }
        if (has_amp) {
            if (!(fmt & DT_SINGLELINE)) { aret_partial("DrawText: '&' accelerator in multi-line text pending"); return 0; }
            int o = 0;
            for (int i = 0; i < len && o < 1024; ) {
                if (cps[i] == '&') {
                    if (i + 1 < len && cps[i + 1] == '&') { pcps[o++] = '&'; i += 2; }
                    else if (i + 1 < len) { if (acc < 0) acc = o; pcps[o++] = cps[i + 1]; i += 2; }
                    else i++;   /* trailing '&' dropped */
                } else pcps[o++] = cps[i++];
            }
            cps = pcps; len = o;
        }
    }
    int32_t *rc = (int32_t *)(uintptr_t)prc;
    if (!rc) return 0;
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int ascent, descent;
    FT_Face ftf = u32_dc_font(d, &ascent, &descent);
    if (!ftf) return 0;
    int lineH = ascent + descent;
    int clip = !(fmt & DT_NOCLIP);
    uint32_t saved_align = g_gdi[d].text_align; g_gdi[d].text_align = 0;

    if (!(fmt & DT_SINGLELINE)) {
        /* Multi-line: split on '\n' (hard breaks); within a segment, DT_WORDBREAK
         * wraps greedily at the last space that keeps the line within the rect
         * width (a lone over-long word breaks at the character). Each line is drawn
         * at rc.top + i*tmHeight, horizontally aligned; vertical alignment does not
         * apply to multi-line (per Win32). ret / DT_CALCRECT = lines*tmHeight. */
        int rw = rc[2] - rc[0];
        int nlines = 0, maxw = 0, drawn = 0;
        int i = 0;
        while (i <= len) {
            int segEnd = i; while (segEnd < len && cps[segEnd] != '\n') segEnd++;
            int pos = i;
            do {
                int lineEnd;
                if (fmt & DT_WORDBREAK) {
                    int lastSpaceEnd = -1, k = pos;
                    lineEnd = segEnd;
                    while (k < segEnd) {
                        if (u32_text_width(ftf, cps + pos, k + 1 - pos) > rw && k > pos) {
                            lineEnd = (lastSpaceEnd > pos) ? lastSpaceEnd : k;
                            break;
                        }
                        if (cps[k] == ' ') lastSpaceEnd = k + 1;
                        k++;
                    }
                } else lineEnd = segEnd;
                /* Wine excludes trailing spaces from a line's width (for CALCRECT
                 * width and for center/right alignment). */
                int te = lineEnd; while (te > pos && cps[te - 1] == ' ') te--;
                int lw = u32_text_width(ftf, cps + pos, te - pos);
                if (lw > maxw) maxw = lw;
                int y = rc[1] + nlines * lineH;
                if (!(fmt & DT_CALCRECT) && y < rc[3]) {   /* draw lines starting within the rect */
                    int x = rc[0];
                    if (fmt & DT_CENTER) x = rc[0] + (rw - lw) / 2;
                    else if (fmt & DT_RIGHT) x = rc[2] - lw;
                    u32_textout_full(hdc, x, y, cps + pos, te - pos, NULL, rc, 0, clip);
                    drawn++;
                }
                nlines++;
                pos = lineEnd;
            } while (pos < segEnd);
            i = segEnd + 1;   /* skip the '\n' */
        }
        g_gdi[d].text_align = saved_align;
        if (fmt & DT_CALCRECT) { rc[2] = rc[0] + maxw; rc[3] = rc[1] + nlines * lineH; return (uint32_t)(nlines * lineH); }
        return (uint32_t)(drawn * lineH);   /* DRAW: height of the lines that fit */
    }

    int textW = u32_text_width(ftf, cps, len);
    int textH = lineH;
    if (fmt & DT_CALCRECT) { g_gdi[d].text_align = saved_align; rc[2] = rc[0] + textW; rc[3] = rc[1] + textH; return (uint32_t)textH; }
    int rw = rc[2] - rc[0], rh = rc[3] - rc[1];
    int x = rc[0];
    if (fmt & DT_CENTER) x = rc[0] + (rw - textW) / 2;
    else if (fmt & DT_RIGHT) x = rc[2] - textW;
    int y = rc[1];
    if (fmt & DT_VCENTER) y = rc[1] + (rh - textH + 1) / 2;   /* Wine rounds up */
    else if (fmt & DT_BOTTOM) y = rc[3] - textH;
    u32_textout_full(hdc, x, y, cps, len, NULL, rc, 0, clip);
    /* Accelerator underline: DrawText draws a 1px pen line (not the font's underline)
     * at baseline+1, spanning the char's extent minus one (LineTo excludes its
     * endpoint) — measured across sizes. */
    if (acc >= 0) {
        struct gdi_obj *surf = gdi_dc_surface(hdc);
        if (surf && surf->bpp == 32) {
            int pre_w = u32_text_width(ftf, cps, acc);
            int char_w = u32_text_width(ftf, cps + acc, 1);
            int Y = y + ascent + 1;             /* baseline + 1 */
            int x0 = x + pre_w, x1 = x0 + char_w - 1;
            uint32_t fg = g_gdi[d].text_color;
            int cl = clip ? rc[0] : 0, ct = clip ? rc[1] : 0, crr = clip ? rc[2] : surf->w, cbb = clip ? rc[3] : surf->h;
            for (int X = x0; X < x1; X++)
                if (X >= cl && X < crr && Y >= ct && Y < cbb && X >= 0 && X < surf->w && Y >= 0 && Y < surf->h)
                    gdi_put(surf, X, Y, fg);
        }
    }
    g_gdi[d].text_align = saved_align;
    if (fmt & (DT_VCENTER|DT_BOTTOM)) return (uint32_t)(y + textH - rc[1]);
    return (uint32_t)textH;
#else
    (void)hdc; (void)cps; (void)len; (void)prc; (void)fmt;
    aret_partial("DrawText: FreeType not linked"); return 0;
#endif
}
uint32_t aret_DrawTextA(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    const char *s = WCS(1); int n = WI(2); if (n < 0) n = s ? (int)strlen(s) : 0;
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = s ? u32_ansi_cp((unsigned char)s[i]) : 0;
    return u32_drawtext(WU(0), cps, s ? m : 0, WU(3), WU(4));
}
uint32_t aret_DrawTextW(uint32_t esp) {
    const uint16_t *ws = (const uint16_t *)(uintptr_t)WU(1); int n = WI(2);
    if (n < 0) { n = 0; if (ws) while (ws[n]) n++; }
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_drawtext(WU(0), cps, ws ? m : 0, WU(3), WU(4));
}
static int u32_textout_ansi(uint32_t hdc, int x, int y, const char *str, int len) {
    uint32_t cps[1024]; int m = len < 1024 ? len : 1024;
    if (m < 0) m = 0;
    for (int i = 0; i < m; i++) cps[i] = u32_ansi_cp((unsigned char)str[i]);
    return u32_textout_core(hdc, x, y, cps, m);
}

/* GetTextExtentPoint32A(hdc, str, count, lpSize) -> BOOL. cx = sum of default
 * advances (Wine's extent regime), cy = tmHeight. Shares u32_dc_font so it agrees
 * with TextOut's OPAQUE fill exactly. */
static int u32_text_extent(uint32_t hdc, const uint32_t *cps, int len, uint32_t psize) {
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int ascent, descent;
    FT_Face ftf = u32_dc_font(d, &ascent, &descent);
    if (!ftf) return 0;
    int w = u32_text_width(ftf, cps, len);
    if (psize) { int32_t *sz = (int32_t *)(uintptr_t)psize; sz[0] = w; sz[1] = ascent + descent; }
    return 1;
#else
    (void)hdc; (void)cps; (void)len; (void)psize;
    aret_partial("GetTextExtentPoint32: FreeType not linked");
    return 0;
#endif
}
static int u32_text_extent_ansi(uint32_t hdc, const char *s, int len, uint32_t psize) {
    uint32_t cps[1024]; int m = len < 1024 ? len : 1024; if (m < 0) m = 0;
    for (int i = 0; i < m; i++) cps[i] = u32_ansi_cp((unsigned char)s[i]);
    return u32_text_extent(hdc, cps, m, psize);
}

/* TextOutA(hdc, x, y, lpString, cbCount) -> BOOL. */
uint32_t aret_TextOutA(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    return u32_textout_ansi(WU(0), WI(1), WI(2), WCS(3), WI(4)) ? 1 : 0;
}
/* TextOutW(hdc, x, y, lpWideString, cchCount) -> BOOL. Full Unicode: each UTF-16
 * unit is a codepoint FT_Load_Char maps through the font's cmap (BMP; surrogate
 * pairs are a follow-up). */
uint32_t aret_TextOutW(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    const uint16_t *ws = (const uint16_t *)(uintptr_t)WU(3);
    int n = WI(4); if (n < 0) n = 0;
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_textout_core(WU(0), WI(1), WI(2), cps, ws ? m : 0) ? 1 : 0;
}

/* GetTextExtentPoint32A / GetTextExtentPointA(hdc, str, count, lpSize) -> BOOL.
 * (The non-32 variant has the same signature; both share one path.) */
uint32_t aret_GetTextExtentPoint32A(uint32_t esp) {
    return u32_text_extent_ansi(WU(0), WCS(1), WI(2), WU(3)) ? 1 : 0;
}
uint32_t aret_GetTextExtentPointA(uint32_t esp) { return aret_GetTextExtentPoint32A(esp); }
uint32_t aret_GetTextExtentPoint32W(uint32_t esp) {
    const uint16_t *ws = (const uint16_t *)(uintptr_t)WU(1);
    int n = WI(2); if (n < 0) n = 0;
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_text_extent(WU(0), cps, ws ? m : 0, WU(3)) ? 1 : 0;
}
uint32_t aret_GetTextExtentPointW(uint32_t esp) { return aret_GetTextExtentPoint32W(esp); }

/* ---- comctl32 socle batch 6a: per-character metrics (FreeType). Share the exact DC
 * font path as GetTextExtentPoint32 (already bit-identical to Wine), so the advances
 * agree by construction. GetCharWidthW = per-char advance; GetCharABCWidthsW = left
 * bearing (A) / black-box width (B) / advance-A-B (C); GetTextExtentExPointW = extent
 * with a fit count + cumulative dx array; GdiGetCharDimensions = the internal average-
 * width helper (52-letter alphabet, reused by the DLU code). All measured vs Wine. */
uint32_t aret_GetCharWidthW(uint32_t esp) {
    int first = WI(1), last = WI(2); int32_t *out = (int32_t *)WP(3);
    if (!out || last < first) return 0;
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int asc, desc; FT_Face f = u32_dc_font(d, &asc, &desc); if (!f) return 0;
    for (int c = first; c <= last; c++) {
        uint32_t cp = (uint32_t)c;
        out[c - first] = (FT_Load_Char(f, cp, FT_LOAD_DEFAULT) == 0) ? (int)(f->glyph->advance.x >> 6) : 0;
    }
    return 1;
#else
    (void)esp; aret_partial("GetCharWidthW: FreeType not linked"); return 0;
#endif
}
uint32_t aret_GetCharWidthA(uint32_t esp) { return aret_GetCharWidthW(esp); }
uint32_t aret_GetCharABCWidthsW(uint32_t esp) {
    int first = WI(1), last = WI(2); int32_t *out = (int32_t *)WP(3);   /* ABC = 3 x LONG */
    if (!out || last < first) return 0;
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int asc, desc; FT_Face f = u32_dc_font(d, &asc, &desc); if (!f) return 0;
    for (int c = first; c <= last; c++) {
        int32_t *abc = &out[(c - first) * 3];
        if (FT_Load_Char(f, (uint32_t)c, FT_LOAD_DEFAULT) != 0) { abc[0] = abc[1] = abc[2] = 0; continue; }
        int adv = (int)(f->glyph->advance.x >> 6);
        int a = (int)(f->glyph->metrics.horiBearingX >> 6);
        int b = (int)(f->glyph->metrics.width >> 6);
        abc[0] = a; abc[1] = (int32_t)(uint32_t)b; abc[2] = adv - a - b;   /* B is unsigned; A+B+C = advance */
    }
    return 1;
#else
    (void)esp; aret_partial("GetCharABCWidthsW: FreeType not linked"); return 0;
#endif
}
static int u32_gtee(uint32_t hdc, const uint32_t *cps, int n, int maxext,
                    int32_t *pfit, int32_t *dx, int32_t *sz) {
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(hdc); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int asc, desc; FT_Face f = u32_dc_font(d, &asc, &desc); if (!f) return 0;
    int total = 0, fit = 0, fitting = 1;
    for (int i = 0; i < n; i++) {
        int adv = (FT_Load_Char(f, cps[i], FT_LOAD_DEFAULT) == 0) ? (int)(f->glyph->advance.x >> 6) : 0;
        total += adv;
        if (dx) dx[i] = total;                          /* cumulative extent through char i */
        if (fitting && (maxext < 0 || total <= maxext)) fit = i + 1; else fitting = 0;
    }
    if (pfit) *pfit = fit;
    if (sz) { sz[0] = total; sz[1] = asc + desc; }       /* SIZE = full-string extent (not the fitted one) */
    return 1;
#else
    (void)hdc; (void)cps; (void)n; (void)maxext; (void)pfit; (void)dx; (void)sz;
    aret_partial("GetTextExtentExPoint: FreeType not linked"); return 0;
#endif
}
uint32_t aret_GetTextExtentExPointW(uint32_t esp) {
    const uint16_t *ws = (const uint16_t *)WP(1); int n = WI(2); if (n < 0) n = 0;
    uint32_t cps[2048]; int m = n < 2048 ? n : 2048;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_gtee(WU(0), cps, ws ? m : 0, WI(3), (int32_t *)WP(4), (int32_t *)WP(5), (int32_t *)WP(6)) ? 1 : 0;
}
uint32_t aret_GetTextExtentExPointA(uint32_t esp) {
    const char *s = WCS(1); int n = WI(2); if (n < 0) n = s ? (int)strlen(s) : 0;
    uint32_t cps[2048]; int m = n < 2048 ? n : 2048;
    for (int i = 0; i < m; i++) cps[i] = u32_ansi_cp((unsigned char)(s ? s[i] : 0));
    return u32_gtee(WU(0), cps, s ? m : 0, WI(3), (int32_t *)WP(4), (int32_t *)WP(5), (int32_t *)WP(6)) ? 1 : 0;
}
/* GdiGetCharDimensions(hdc, TEXTMETRIC* tm, LONG* height) -> average char width. Wine's
 * exact recipe: cx = (GetTextExtentPoint of the 52-letter A..Za..z alphabet / 26 + 1)/2;
 * height (if requested) = tmHeight. Reused by the dialog-unit base-units computation. */
uint32_t aret_GdiGetCharDimensions(uint32_t esp) {
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    int asc, desc; FT_Face f = u32_dc_font(d, &asc, &desc); if (!f) return 0;
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint32_t cps[52]; for (int i = 0; i < 52; i++) cps[i] = (unsigned char)alpha[i];
    int w = u32_text_width(f, cps, 52);
    int32_t *ph = (int32_t *)WP(2); if (ph) *ph = asc + desc;   /* tmHeight */
    return (uint32_t)((w / 26 + 1) / 2);
#else
    (void)esp; aret_partial("GdiGetCharDimensions: FreeType not linked"); return 0;
#endif
}
/* GetCharWidthInfo(hdc, struct char_width_info{lsb,rsb,unk}) -> internal ntgdi helper
 * comctl32 calls during font-linking/uniscribe fallback. No per-font side-bearing data
 * in our model -> report zero bearings (sound: "no special width info"), return TRUE. */
uint32_t aret_GetCharWidthInfo(uint32_t esp) {
    int32_t *info = (int32_t *)WP(1); if (info) { info[0] = 0; info[1] = 0; info[2] = 0; }
    return 1;
}

/* Tabbed text (TabbedTextOut / GetTabbedTextExtent). Expands '\t' to tab stops and
 * lays out the runs, sharing the proven FreeType text core (u32_textout_core / extent
 * regime). Tab-stop recipe MEASURED bit-exact vs Wine:
 *   - segment width = GetTextExtentPoint32 (default advances);
 *   - the pen jumps to the next tab stop STRICTLY greater than the current pen;
 *   - stops: >1 positions -> org+lpTabPos[j] (absolute, from nTabOrigin); with <=1
 *     position or beyond the array -> multiples of defWidth, where defWidth =
 *     lpTabPos[0] (exactly one stop) else 8*tmAveCharWidth (measured Wine default);
 *   - return value = MAKELONG(totalWidth, tmHeight).
 * Negative (right-aligned) tab stops and non-positive uniform widths abort soundly. */
#ifdef ARET_HAVE_FREETYPE
static int u32_next_tab(int pen, int org, int nTabs, const int32_t *tabs, int defWidth) {
    if (nTabs > 1)
        for (int j = 0; j < nTabs; j++) { int tp = org + tabs[j]; if (tp > pen) return tp; }
    int rel = pen - org; if (rel < 0) rel = 0;
    return org + ((rel / defWidth) + 1) * defWidth;
}
#endif
static uint32_t u32_tabbed_core(uint32_t hdc, int x, int y, const uint32_t *cps, int len,
                                int nTabs, const int32_t *tabs, int org, int draw) {
#ifdef ARET_HAVE_FREETYPE
    int d = gdi_idx(hdc);
    if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    if (!tabs) nTabs = 0;
    for (int j = 0; j < nTabs; j++)
        if (tabs[j] < 0) { aret_partial("TabbedTextOut: negative (right-aligned) tab stops pending"); return 0; }
    int ascent, descent;
    FT_Face ftf = u32_dc_font(d, &ascent, &descent);
    if (!ftf) return 0;
    TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(ftf, FT_SFNT_OS2);
    int ave = os2 ? (int)((FT_MulFix(os2->xAvgCharWidth, ftf->size->metrics.x_scale) + 32) >> 6) : 0;
    int defWidth = (nTabs == 1) ? tabs[0] : (8 * ave);
    if (defWidth <= 0) { aret_partial("TabbedTextOut: non-positive tab width"); return 0; }
    int pen = x, i = 0;
    while (i < len) {
        int seg = i; while (seg < len && cps[seg] != '\t') seg++;
        if (draw && seg > i) u32_textout_core(hdc, pen, y, cps + i, seg - i);
        pen += u32_text_width(ftf, cps + i, seg - i);
        if (seg < len) { pen = u32_next_tab(pen, org, nTabs, tabs, defWidth); i = seg + 1; }
        else i = seg;
    }
    return ((uint32_t)(ascent + descent) << 16) | ((uint32_t)(pen - x) & 0xFFFFu);
#else
    (void)hdc; (void)x; (void)y; (void)cps; (void)len; (void)nTabs; (void)tabs; (void)org; (void)draw;
    aret_partial("TabbedTextOut: FreeType not linked"); return 0;
#endif
}
uint32_t aret_TabbedTextOutA(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    const char *s = WCS(3); int n = WI(4); if (n < 0) n = s ? (int)strlen(s) : 0;
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024; if (m < 0) m = 0;
    for (int i = 0; i < m; i++) cps[i] = s ? u32_ansi_cp((unsigned char)s[i]) : 0;
    return u32_tabbed_core(WU(0), WI(1), WI(2), cps, s ? m : 0, WI(5), (const int32_t *)WP(6), WI(7), 1);
}
uint32_t aret_TabbedTextOutW(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    const uint16_t *ws = (const uint16_t *)(uintptr_t)WU(3);
    int n = WI(4); if (n < 0) { n = 0; if (ws) while (ws[n]) n++; }
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_tabbed_core(WU(0), WI(1), WI(2), cps, ws ? m : 0, WI(5), (const int32_t *)WP(6), WI(7), 1);
}
uint32_t aret_GetTabbedTextExtentA(uint32_t esp) {
    const char *s = WCS(1); int n = WI(2); if (n < 0) n = s ? (int)strlen(s) : 0;
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024; if (m < 0) m = 0;
    for (int i = 0; i < m; i++) cps[i] = s ? u32_ansi_cp((unsigned char)s[i]) : 0;
    return u32_tabbed_core(WU(0), 0, 0, cps, s ? m : 0, WI(3), (const int32_t *)WP(4), 0, 0);
}
uint32_t aret_GetTabbedTextExtentW(uint32_t esp) {
    const uint16_t *ws = (const uint16_t *)(uintptr_t)WU(1);
    int n = WI(2); if (n < 0) { n = 0; if (ws) while (ws[n]) n++; }
    uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
    for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
    return u32_tabbed_core(WU(0), 0, 0, cps, ws ? m : 0, WI(3), (const int32_t *)WP(4), 0, 0);
}

/* Fill a TEXTMETRIC{A,W} for the DC's selected font, every field matching Wine
 * (formulas mined + verified against GetTextMetrics across DejaVu/Liberation at
 * several sizes). `wide` selects the W layout (WCHAR char fields at +44). */
#ifdef ARET_HAVE_FREETYPE
/* Fill a TEXTMETRIC{A,W} from an already-resolved, already-sized face. Split out of
 * u32_fill_textmetric so the font ENUMERATION can reuse the very same (Wine-verified)
 * formulas on a face it resolved itself, instead of inventing metrics for a font it
 * did not measure. */
static int u32_tm_from_face(FT_Face f, int ascent, int descent, int italic,
                            int wide, uint32_t out) {
    TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
    TT_HoriHeader *hh = (TT_HoriHeader *)FT_Get_Sfnt_Table(f, FT_SFNT_HHEA);
    FT_Fixed ys = f->size->metrics.y_scale, xs = f->size->metrics.x_scale;
    int EM = f->units_per_EM ? f->units_per_EM : 2048;
    int32_t H  = ascent + descent;
    int32_t IL = (int32_t)((FT_MulFix(os2->usWinAscent + os2->usWinDescent - EM, ys) + 32) >> 6);
    int32_t EL = hh ? (int32_t)((FT_MulFix(hh->Line_Gap, ys) + 32) >> 6) : 0;
    int32_t ave  = (int32_t)((FT_MulFix(os2->xAvgCharWidth, xs) + 32) >> 6);
    int32_t maxw = (int32_t)((FT_MulFix((FT_Long)(f->bbox.xMax - f->bbox.xMin), xs) + 32) >> 6);
    int32_t weight = os2->usWeightClass;
    int fam;
    switch ((os2->sFamilyClass >> 8) & 0xff) {           /* IBM font-class → FF_* */
        case 1: case 2: case 3: case 4: case 5: case 7: fam = 0x10; break; /* FF_ROMAN  */
        case 8:  fam = 0x20; break;                       /* FF_SWISS      */
        case 10: fam = 0x40; break;                       /* FF_SCRIPT     */
        case 12: fam = 0x50; break;                       /* FF_DECORATIVE */
        default: fam = 0x20; break;                       /* FF_SWISS (Wine's default for unclassified) */
    }
    uint8_t pf = (uint8_t)(0x06 /*TMPF_VECTOR|TMPF_TRUETYPE*/ | (FT_IS_FIXED_WIDTH(f) ? 0 : 0x01) | fam);
    int32_t *L = (int32_t *)(uintptr_t)out;
    L[0]=H; L[1]=ascent; L[2]=descent; L[3]=IL; L[4]=EL; L[5]=ave; L[6]=maxw; L[7]=weight;
    L[8]=0; L[9]=96; L[10]=96;                            /* overhang, digitized aspect X/Y */
    uint8_t *B = (uint8_t *)(uintptr_t)out;
    if (!wide) {
        B[44]=30; B[45]=255; B[46]=31; B[47]=32;          /* first/last/default/break (ANSI) */
        B[48]=italic; B[49]=0; B[50]=0; B[51]=pf; B[52]=0;
    } else {
        uint16_t *W = (uint16_t *)(uintptr_t)(B + 44);
        W[0]=30; W[1]=255; W[2]=31; W[3]=32;
        B[52]=(uint8_t)italic; B[53]=0; B[54]=0; B[55]=pf; B[56]=0;
    }
    return 1;
}
#endif

static int u32_fill_textmetric(int d, int wide, uint32_t out) {
#ifdef ARET_HAVE_FREETYPE
    if (!out) return 0;
    int ascent, descent;
    FT_Face f = u32_dc_font(d, &ascent, &descent);
    if (!f) return 0;
    int fi = gdi_idx(g_gdi[d].sel_font);
    int italic = (fi >= 0 && g_gdi[fi].lf_italic) ? 1 : 0;
    return u32_tm_from_face(f, ascent, descent, italic, wide, out);
#else
    (void)d; (void)wide; (void)out; aret_partial("GetTextMetrics: FreeType not linked"); return 0;
#endif
}
uint32_t aret_GetTextMetricsA(uint32_t esp) { int d = gdi_idx(WU(0)); return (d >= 0 && u32_fill_textmetric(d, 0, WU(1))) ? 1 : 0; }
uint32_t aret_GetTextMetricsW(uint32_t esp) { int d = gdi_idx(WU(0)); return (d >= 0 && u32_fill_textmetric(d, 1, WU(1))) ? 1 : 0; }

/* EnumFontFamilies(A/W)(hdc, lpszFamily, lpProc, lParam) -> int.
 *
 * Enumerates the installed font families, invoking a LIFTED stdcall callback
 * `proc(const LOGFONT*, const TEXTMETRIC*, DWORD FontType, LPARAM)` per family. Real
 * GUI apps use it to populate a font picker (the wall real MFC/WinMerge hits after
 * its non-client metrics).
 *
 * WHAT IS EXACT vs WHAT IS ENVIRONMENTAL (doc 70 §4.5 / doc 72 §4.5). The *contract*
 * is deterministic and is reproduced exactly — verified against Wine by
 * `winecorpus/gdi_enumfonts.c`:
 *   - a callback returning 0 STOPS the enumeration immediately, and the function
 *     returns that 0 (not the number enumerated);
 *   - a family that does not exist yields ZERO callbacks and still returns 1;
 *   - enumerating everything (lpszFamily == NULL) returns 1.
 * The *set* of families, and their metrics, are environment-dependent (they come from
 * the host's installed fonts — 399 families here) exactly as they are under Wine, so
 * they are NOT bit-compared: the list is taken from fontconfig (the same source Wine
 * uses on Linux) and each face's metrics are computed by the SAME Wine-verified
 * formulas as GetTextMetrics (u32_tm_from_face) on the real font file. A family whose
 * file cannot be loaded or measured is skipped rather than reported with invented
 * metrics. */
#ifdef ARET_HAVE_FREETYPE
static int u32_str_ci_eq(const char *a, const char *b) {
    while (*a && *b && ((*a | 0x20) == (*b | 0x20))) { a++; b++; }
    return !*a && !*b;
}
static int u32_cmp_family(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
/* One enumerated family: fill LOGFONT + TEXTMETRIC in guest memory and call back.
 * Returns the callback's value, or 1 to keep going when the face is unusable. */
static uint32_t u32_enum_one(uint32_t esp, const char *family, uint32_t proc,
                             uint32_t lparam, int wide, uint32_t lf, uint32_t tm) {
    char path[256];
    if (!ft_resolve_face(family, 0, 0, path, sizeof path)) return 1;
    FT_Face f = ft_get_face(path);
    if (!f) return 1;
    TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
    if (!os2 || os2->version == 0xFFFF) return 1;   /* no metrics -> do not report it */
    /* Size the face the way u32_dc_font does for a cell height, at the reference
     * height below, so the reported metrics are the face's real ones. */
    const int ref_height = 16;
    int upm = f->units_per_EM ? f->units_per_EM : 2048;
    int cell = os2->usWinAscent + os2->usWinDescent;
    int ppem = cell ? (int)(((long)ref_height * upm + cell / 2) / cell) : ref_height;
    if (ppem <= 0) ppem = 1;
    if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)ppem) != 0) return 1;
    FT_Fixed ys = f->size->metrics.y_scale;
    int ascent  = (int)((FT_MulFix(os2->usWinAscent, ys) + 32) >> 6);
    int descent = (int)((FT_MulFix(os2->usWinDescent, ys) + 32) >> 6);
    if (!u32_tm_from_face(f, ascent, descent, 0, wide, tm)) return 1;
    /* LOGFONT{A,W}: only the fields an enumeration defines; the face name is the
     * family, and the pitch/family byte mirrors the TEXTMETRIC's. */
    uint8_t *L = (uint8_t *)(uintptr_t)lf;
    memset(L, 0, wide ? 92u : 60u);
    *(int32_t *)(L + 0) = ascent + descent;                 /* lfHeight (cell)     */
    *(int32_t *)(L + 16) = (int32_t)os2->usWeightClass;     /* lfWeight            */
    /* lfPitchAndFamily and tmPitchAndFamily are NOT the same byte (measured: Wine
     * reports lf 0x22 vs tm 0x27 for every proportional TrueType face). They share
     * the FF_* family nibble, but the low bits differ: the LOGFONT carries the
     * pitch request (VARIABLE_PITCH 2 / FIXED_PITCH 1) while the TEXTMETRIC carries
     * TMPF_* flags (fixed-pitch/vector/truetype). Copying one into the other would
     * be a silent divergence, so derive the LOGFONT byte from the family nibble. */
    uint8_t tm_pf = ((uint8_t *)(uintptr_t)tm)[wide ? 55 : 51];
    L[27] = (uint8_t)((tm_pf & 0xf0) | (FT_IS_FIXED_WIDTH(f) ? 1u : 2u));
    for (unsigned i = 0; family[i] && i < 31; i++) {
        if (wide) *(uint16_t *)(L + 28 + 2 * i) = (uint16_t)(unsigned char)family[i];
        else L[28 + i] = (uint8_t)family[i];
    }
    uint32_t frame = (lf - 64) & ~15u;                       /* below the structs  */
    uint32_t *fr = (uint32_t *)(uintptr_t)frame;
    fr[0] = 0; fr[1] = lf; fr[2] = tm; fr[3] = 4 /*TRUETYPE_FONTTYPE*/; fr[4] = lparam;
    (void)esp;
    return (uint32_t)aret_call(proc, frame, 0, 0, 0, 0, 0, 0, 0);
}
#endif

static uint32_t u32_enum_font_families(uint32_t esp, const char *want, uint32_t proc,
                                       uint32_t lparam, int wide) {
#ifdef ARET_HAVE_FREETYPE
    if (!proc) return 1;
    if (!ft_ensure()) { aret_partial("EnumFontFamilies: FreeType/fontconfig init failed"); return 0; }
    /* The two guest structures live between the caller's esp and the callback's
     * frame, so the callback's own stack (which grows below that frame) cannot
     * clobber them. */
    uint32_t lf = (esp - 256) & ~15u, tm = lf + 128;
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)0);
    FcFontSet *fs = (pat && os) ? FcFontList(NULL, pat, os) : NULL;
    uint32_t ret = 1;
    if (fs) {
        /* Deduplicate and sort, so the enumeration order is deterministic for a
         * given set of installed fonts (fontconfig's own order is not). */
        char **fam = (char **)calloc((size_t)fs->nfont, sizeof(char *));
        int n = 0;
        for (int i = 0; fam && i < fs->nfont; i++) {
            FcChar8 *s = NULL;
            if (FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &s) != FcResultMatch || !s) continue;
            if (want && !u32_str_ci_eq((const char *)s, want)) continue;
            int dup = 0;
            for (int j = 0; j < n; j++) if (strcmp(fam[j], (const char *)s) == 0) { dup = 1; break; }
            if (!dup) fam[n++] = strdup((const char *)s);
        }
        if (fam) {
            qsort(fam, (size_t)n, sizeof(char *), u32_cmp_family);
            for (int i = 0; i < n; i++) {
                if (ret) ret = u32_enum_one(esp, fam[i], proc, lparam, wide, lf, tm);
                free(fam[i]);
            }
            free(fam);
        }
        FcFontSetDestroy(fs);
    }
    if (os) FcObjectSetDestroy(os);
    if (pat) FcPatternDestroy(pat);
    return ret;
#else
    (void)esp; (void)want; (void)proc; (void)lparam; (void)wide;
    aret_partial("EnumFontFamilies: FreeType not linked");
    return 0;
#endif
}
uint32_t aret_EnumFontFamiliesA(uint32_t esp) {
    const char *want = WCS(1);
    return u32_enum_font_families(esp, (want && want[0]) ? want : NULL, WU(2), WU(3), 0);
}
uint32_t aret_EnumFontFamiliesW(uint32_t esp) {
    char buf[128];
    const uint16_t *w = (const uint16_t *)(uintptr_t)WU(1);
    const char *want = NULL;
    if (w && w[0]) { u32_w2n(w, buf, sizeof buf); want = buf; }
    return u32_enum_font_families(esp, want, WU(2), WU(3), 1);
}

/* ---- DC attributes ---- */
uint32_t aret_SetTextColor(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].text_color; g_gdi[d].text_color = WU(1) & 0xFFFFFFu; return p; }
uint32_t aret_SetBkColor(uint32_t esp)   { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].bk_color; g_gdi[d].bk_color = WU(1) & 0xFFFFFFu; return p; }
uint32_t aret_SetBkMode(uint32_t esp)    { int d = gdi_idx(WU(0)); if (d < 0) return 0; int p = g_gdi[d].bk_mode; g_gdi[d].bk_mode = WI(1); return (uint32_t)p; }
uint32_t aret_GetTextColor(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].text_color; }
uint32_t aret_GetBkColor(uint32_t esp)   { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].bk_color; }
/* SetTextAlign/GetTextAlign(hdc[, align]) -> UINT. DC state (default TA_LEFT|TA_TOP=0). */
uint32_t aret_SetTextAlign(uint32_t esp) { int d = gdi_idx(WU(0)); if (d < 0) return 0xFFFFFFFFu; uint32_t p = g_gdi[d].text_align; g_gdi[d].text_align = WU(1); return p; }
uint32_t aret_GetTextAlign(uint32_t esp) { int d = gdi_idx(WU(0)); return d < 0 ? 0xFFFFFFFFu : g_gdi[d].text_align; }

/* GetSysColor(nIndex) -> COLORREF. Classic Win32 default scheme (deterministic;
 * the values a the/me-less Wine also serves for the common indices). */
static uint32_t u32_syscolor(int i) {
    switch (i) {
    case 0:  return 0x808080;   /* COLOR_SCROLLBAR */
    case 1:  return 0x000000;   /* COLOR_BACKGROUND / DESKTOP */
    case 2:  return 0x800000;   /* COLOR_ACTIVECAPTION */
    case 3:  return 0x808080;   /* COLOR_INACTIVECAPTION */
    case 4:  return 0xC0C0C0;   /* COLOR_MENU */
    case 5:  return 0xFFFFFF;   /* COLOR_WINDOW */
    case 6:  return 0x000000;   /* COLOR_WINDOWFRAME */
    case 7:  return 0x000000;   /* COLOR_MENUTEXT */
    case 8:  return 0x000000;   /* COLOR_WINDOWTEXT */
    case 9:  return 0xFFFFFF;   /* COLOR_CAPTIONTEXT */
    case 10: return 0xC0C0C0;   /* COLOR_ACTIVEBORDER */
    case 11: return 0xC0C0C0;   /* COLOR_INACTIVEBORDER */
    case 12: return 0x808080;   /* COLOR_APPWORKSPACE */
    case 13: return 0x800000;   /* COLOR_HIGHLIGHT */
    case 14: return 0xFFFFFF;   /* COLOR_HIGHLIGHTTEXT */
    case 15: return 0xC0C0C0;   /* COLOR_BTNFACE / 3DFACE */
    case 16: return 0x808080;   /* COLOR_BTNSHADOW */
    case 17: return 0x808080;   /* COLOR_GRAYTEXT */
    case 18: return 0x000000;   /* COLOR_BTNTEXT */
    case 19: return 0xC0C0C0;   /* COLOR_INACTIVECAPTIONTEXT */
    case 20: return 0xFFFFFF;   /* COLOR_BTNHIGHLIGHT */
    case 21: return 0x000000;   /* COLOR_3DDKSHADOW (classic Win95 black) */
    case 22: return 0xE0E0E0;   /* COLOR_3DLIGHT (classic light gray, lighter than face) */
    case 23: return 0x000000;   /* COLOR_INFOTEXT */
    case 24: return 0xE1FFFF;   /* COLOR_INFOBK (classic tooltip yellow-ish) */
    default: return 0x000000;
    }
}
uint32_t aret_GetSysColor(uint32_t esp) { return u32_syscolor(WI(0)); }
/* GetSysColorBrush(nIndex) -> HBRUSH (cached solid brush of that colour). */
static uint32_t g_gdi_syscolorbrush[32];
uint32_t aret_GetSysColorBrush(uint32_t esp) {
    int id = WI(0); if (id < 0 || id >= 32) return 0;
    if (!g_gdi_syscolorbrush[id]) {
        int i = gdi_alloc(GDIT_BRUSH); if (!i) return 0;
        g_gdi[i].stock = 1; g_gdi[i].color = u32_syscolor(id);
        g_gdi_syscolorbrush[id] = gdi_handle(i);
    }
    return g_gdi_syscolorbrush[id];
}
/* GetDeviceCaps(hdc, index) -> int. Fixed device-class metrics exact; the
 * environment-dependent screen extents use ARET's virtual desktop (tested by
 * invariant, not raw, like GetSystemMetrics — doc 72 §4.5). */
uint32_t aret_GetDeviceCaps(uint32_t esp) {
    switch (WI(1)) {
    case 2:  return 1;            /* TECHNOLOGY = DT_RASDISPLAY */
    case 8:  return U32_SCREEN_W; /* HORZRES */
    case 10: return U32_SCREEN_H; /* VERTRES */
    case 12: return 32;           /* BITSPIXEL */
    case 14: return 1;            /* PLANES */
    case 88: return 96;           /* LOGPIXELSX */
    case 90: return 96;           /* LOGPIXELSY */
    case 24: return -1;           /* NUMCOLORS (truecolor) */
    case 104: return 1;           /* SIZEPALETTE-ish / COLORMGMTCAPS default */
    case 4:  return 320;          /* HORZSIZE (mm, ~96dpi) */
    case 6:  return 240;          /* VERTSIZE (mm) */
    default: return 0;
    }
}

/* ================================================================== */
/* USER32 window helpers (display-free, measured against Wine)         */
/* ================================================================== */
static uint32_t g_u32_focus, g_u32_active;

/* GetClientRect(hwnd, RECT*) -> BOOL. Client area = {0,0,w,h}. Our windows have
 * no non-client decoration (display-free), so client == window extent — exact for
 * borderless windows (Wine: WS_POPUP client = {0,0,w,h}). */
uint32_t aret_GetClientRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(1);
    if (!r) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { r[0] = r[1] = r[2] = r[3] = 0; return 0; }
    r[0] = 0; r[1] = 0; r[2] = g_u32_win[i].w; r[3] = g_u32_win[i].h;
    return 1;
}
/* AdjustWindowRect(RECT*, style, bMenu) / …Ex -> BOOL. No non-client area is
 * modelled, so the rect is left unchanged (exact for borderless; Wine leaves a
 * WS_POPUP rect unchanged). */
uint32_t aret_AdjustWindowRect(uint32_t esp)   { return WP(0) ? 1u : 0u; }
uint32_t aret_AdjustWindowRectEx(uint32_t esp) { return WP(0) ? 1u : 0u; }

/* Focus / activation: tracked so Get* round-trips Set*; no real input focus.
 * SetFocus notifies the two windows like Wine: WM_KILLFOCUS to the one losing focus
 * (wParam = the window gaining it), then WM_SETFOCUS to the one gaining it (wParam =
 * the window losing it); g_u32_focus is updated first so GetFocus is correct inside
 * the handlers. No change (new==old) sends nothing. Return the previous focus. */
static uint32_t u32_set_focus(uint32_t esp, uint32_t neu) {
    uint32_t old = g_u32_focus;
    if (neu == old) return old;
    g_u32_focus = neu;
    if (old) { uint32_t wp = u32_win_wndproc(old); if (wp) u32_call_wndproc(esp, wp, old, 0x0008u /*WM_KILLFOCUS*/, neu, 0); }
    if (neu) { uint32_t wp = u32_win_wndproc(neu); if (wp) u32_call_wndproc(esp, wp, neu, 0x0007u /*WM_SETFOCUS*/, old, 0); }
    /* Repaint the controls that lost/gained focus so their focus-state rendering updates
     * (a CBS_DROPDOWNLIST highlights its selection, a button/radio shows/hides its focus
     * rect) — as Windows repaints on focus change. Recompositing a dialog child repaints
     * the whole dialog, so one call covers both when they share a parent. */
    if (old) u32_ctrl_recomposite(esp, (int)old - 1);
    if (neu && (!old || g_u32_win[neu - 1].parent != g_u32_win[old - 1].parent))
        u32_ctrl_recomposite(esp, (int)neu - 1);
    return old;
}
uint32_t aret_SetFocus(uint32_t esp) { return u32_set_focus(esp, WU(0)); }
uint32_t aret_GetFocus(uint32_t esp)         { (void)esp; return g_u32_focus; }
uint32_t aret_SetActiveWindow(uint32_t esp)  { uint32_t p = g_u32_active; g_u32_active = WU(0); return p; }
uint32_t aret_GetActiveWindow(uint32_t esp)  { (void)esp; return g_u32_active; }
uint32_t aret_SetForegroundWindow(uint32_t esp) { return u32_win_idx(WU(0)) >= 0 ? 1u : 0u; }
uint32_t aret_GetForegroundWindow(uint32_t esp) { (void)esp; return g_u32_active; }
uint32_t aret_BringWindowToTop(uint32_t esp)    { return u32_win_idx(WU(0)) >= 0 ? 1u : 0u; }

/* Paint invalidation. The update region is coalesced to a whole-client dirty flag
 * (WM_PAINT unions regions anyway); a partial rect still yields one WM_PAINT.
 * InvalidateRect(NULL,...) invalidates every top-level window. */
uint32_t aret_InvalidateRect(uint32_t esp) {
    uint32_t hwnd = WU(0);
    int erase = WU(2) != 0;                        /* bErase */
    if (hwnd == 0) { for (int i = 0; i < U32_MAX_WIN; i++) if (u32_win_paints(i)) { g_u32_win[i].needs_paint = 1; if (erase) g_u32_win[i].needs_erase = 1; } return 1; }
    int i = u32_win_idx(hwnd); if (i >= 0 && u32_win_paints(i)) { g_u32_win[i].needs_paint = 1; if (erase) g_u32_win[i].needs_erase = 1; }
    return 1;
}
uint32_t aret_InvalidateRgn(uint32_t esp)  { return aret_InvalidateRect(esp); }
uint32_t aret_ValidateRect(uint32_t esp)   {
    int i = u32_win_idx(WU(0)); if (i >= 0) g_u32_win[i].needs_paint = 0;
    return 1;
}
uint32_t aret_ValidateRgn(uint32_t esp)    { return aret_ValidateRect(esp); }
/* MessageBeep(uType) -> BOOL. No audio device; the call succeeds. */
uint32_t aret_MessageBeep(uint32_t esp) { (void)esp; return 1; }

/* CallWindowProcA/W(lpPrevWndFunc, hWnd, Msg, wParam, lParam) -> LRESULT. Invoke a
 * (lifted) window procedure directly — the subclassing idiom's call to the original
 * proc. */
/* CallWindowProc(lpPrevWndFunc, hWnd, msg, wParam, lParam). A subclasser (notably MFC
 * via SetWindowLong(GWL_WNDPROC)) saves the control's ORIGINAL proc and chains to it for
 * messages it doesn't handle itself. For our predefined controls that original proc is 0
 * (their behaviour lives in u32_control_proc, invoked implicitly when wndproc==0) — so
 * proc==0 here must emulate that system control proc: its class messages (BM_SETCHECK,
 * CB_*, WM_SETFONT), then the default window proc. Without this, an MFC-subclassed radio
 * never records BM_SETCHECK (no selection dot) and a subclassed combo/edit loses CB_*. */
static uint32_t u32_call_window_proc(uint32_t esp, int wide) {
    uint32_t proc = WU(0);
    if (proc) return u32_call_wndproc(esp, proc, WU(1), WU(2), WU(3), WU(4));
    uint32_t r = 0;
    if (u32_control_proc(esp, WU(1), WU(2), WU(3), WU(4), &r)) return r;
    if (u32_defproc_common(esp, WU(1), WU(2), WU(3), &r)) return r;
    if (u32_defproc_text(WU(1), WU(2), WU(3), WU(4), wide, &r)) return r;
    return 0;
}
uint32_t aret_CallWindowProcA(uint32_t esp) { return u32_call_window_proc(esp, 0); }
uint32_t aret_CallWindowProcW(uint32_t esp) { return u32_call_window_proc(esp, 1); }

/* LoadCursorA/W, LoadIconA/W(hInstance, lpName) -> opaque non-null handle. Cursors
 * and icons are display-only; a program uses the handle as an opaque token (a
 * WNDCLASS field, SetClassLong), never dereferencing it headless. Distinct per
 * (kind, name) and non-null (matches Wine's "found" invariant). */
uint32_t aret_LoadCursorA(uint32_t esp) { return 0xCC000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadCursorW(uint32_t esp) { return 0xCC000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadIconA(uint32_t esp)   { return 0xCD000000u | (WU(1) & 0x00FFFFFFu); }
uint32_t aret_LoadIconW(uint32_t esp)   { return 0xCD000000u | (WU(1) & 0x00FFFFFFu); }

/* MsgWaitForMultipleObjects(nCount, pHandles, bWaitAll, dwMs, dwWakeMask) -> DWORD.
 * A message available -> WAIT_OBJECT_0 + nCount; else WAIT_TIMEOUT (0x102). Same
 * model as the Ex variant (we read only nCount). */
uint32_t aret_MsgWaitForMultipleObjects(uint32_t esp) {
    uint32_t nCount = WU(0);
    u32_pump_timers();
    if (!u32_q_empty() || g_u32_quit) return nCount;
    return 0x00000102u;   /* WAIT_TIMEOUT */
}

/* ================================================================== */
/* USER32 menus — data model (display-free)                           */
/* ================================================================== */
/* A menu is a list of items (id, flags, optional submenu, text). No pixels: the
 * model backs the state APIs (Enable/Check/GetMenuState/GetMenuString) that a
 * program drives; painting a menu bar is display work (later). Handles: base
 * 0x40000000 (distinct from window/GDI/menu ranges). MF_* flags: GRAYED=1,
 * DISABLED=2, BITMAP=4, CHECKED=8, POPUP=0x10, OWNERDRAW=0x100, BYPOSITION=0x400,
 * SEPARATOR=0x800. */
#define U32_MENU_BASE 0x40000000u
#define U32_MAX_MENUS 128
#define U32_MAX_MITEMS 64
static struct u32_menu {
    int used, count;
    struct { uint32_t id, flags, submenu, bmp_unchecked, bmp_checked; char text[128]; } it[U32_MAX_MITEMS];
} g_u32_menu[U32_MAX_MENUS];
static uint32_t g_u32_wmenu[U32_MAX_WIN], g_u32_wsysmenu[U32_MAX_WIN];

static int u32_menu_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_MENU_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < U32_MAX_MENUS && g_u32_menu[i].used) ? (int)i : -1;
}
static uint32_t u32_menu_new(void) {
    for (int i = 1; i < U32_MAX_MENUS; i++)
        if (!g_u32_menu[i].used) { memset(&g_u32_menu[i], 0, sizeof g_u32_menu[i]); g_u32_menu[i].used = 1; return U32_MENU_BASE | (uint32_t)i; }
    return 0;
}
/* Find an item by command id (default) or by position (MF_BYPOSITION). */
static int u32_menu_find(struct u32_menu *m, uint32_t item, uint32_t flags) {
    if (flags & 0x400u) return (item < (uint32_t)m->count) ? (int)item : -1;
    for (int k = 0; k < m->count; k++) if (m->it[k].id == item) return k;
    return -1;
}
static void u32_menu_setitem(int mi, int slot, uint32_t flags, uint32_t idOrSub, const char *text) {
    struct u32_menu *m = &g_u32_menu[mi];
    m->it[slot].flags = flags & ~0x400u;                 /* MF_BYPOSITION is a lookup bit */
    m->it[slot].id = idOrSub;
    m->it[slot].submenu = (flags & 0x10u) ? idOrSub : 0; /* MF_POPUP: id is the submenu */
    m->it[slot].text[0] = 0;
    if (text && !(flags & (0x800u | 0x4u | 0x100u))) {   /* not separator/bitmap/ownerdraw */
        int k = 0; for (; text[k] && k < 127; k++) m->it[slot].text[k] = text[k];
        m->it[slot].text[k] = 0;
    }
}

uint32_t aret_CreateMenu(uint32_t esp)      { (void)esp; return u32_menu_new(); }
uint32_t aret_CreatePopupMenu(uint32_t esp) { (void)esp; return u32_menu_new(); }
uint32_t aret_DestroyMenu(uint32_t esp)     { int i = u32_menu_idx(WU(0)); if (i < 0) return 0; g_u32_menu[i].used = 0; return 1; }

uint32_t aret_AppendMenuA(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    int s = g_u32_menu[i].count++;
    u32_menu_setitem(i, s, WU(1), WU(2), WCS(3));
    return 1;
}
uint32_t aret_AppendMenuW(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    char t[128]; t[0] = 0;
    if (!(WU(1) & (0x800u | 0x4u | 0x100u))) u32_w2n((const uint16_t *)WP(3), t, sizeof t);
    int s = g_u32_menu[i].count++;
    u32_menu_setitem(i, s, WU(1), WU(2), t);
    return 1;
}
uint32_t aret_InsertMenuA(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0 || g_u32_menu[i].count >= U32_MAX_MITEMS) return 0;
    struct u32_menu *m = &g_u32_menu[i];
    int at = u32_menu_find(m, WU(1), WU(2)); if (at < 0) at = m->count;
    for (int k = m->count; k > at; k--) m->it[k] = m->it[k - 1];
    m->count++;
    u32_menu_setitem(i, at, WU(2), WU(3), WCS(4));
    return 1;
}
uint32_t aret_DeleteMenu(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    struct u32_menu *m = &g_u32_menu[i];
    int at = u32_menu_find(m, WU(1), WU(2)); if (at < 0) return 0;
    for (int k = at; k < m->count - 1; k++) m->it[k] = m->it[k + 1];
    m->count--;
    return 1;
}
uint32_t aret_RemoveMenu(uint32_t esp) { return aret_DeleteMenu(esp); }
uint32_t aret_GetMenuItemCount(uint32_t esp) { int i = u32_menu_idx(WU(0)); return i < 0 ? 0xFFFFFFFFu : (uint32_t)g_u32_menu[i].count; }

uint32_t aret_EnableMenuItem(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t prev = g_u32_menu[i].it[s].flags & 0x3u;    /* MF_GRAYED|MF_DISABLED */
    g_u32_menu[i].it[s].flags = (g_u32_menu[i].it[s].flags & ~0x3u) | (WU(2) & 0x3u);
    return prev;
}
uint32_t aret_CheckMenuItem(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t prev = g_u32_menu[i].it[s].flags & 0x8u;    /* MF_CHECKED */
    g_u32_menu[i].it[s].flags = (g_u32_menu[i].it[s].flags & ~0x8u) | (WU(2) & 0x8u);
    return prev;
}
uint32_t aret_GetMenuState(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0xFFFFFFFFu;
    uint32_t f = g_u32_menu[i].it[s].flags;
    if (f & 0x10u) {                                     /* popup: hiword = submenu count */
        int sub = u32_menu_idx(g_u32_menu[i].it[s].submenu);
        uint32_t cnt = sub >= 0 ? (uint32_t)g_u32_menu[sub].count : 0;
        return (cnt << 16) | (f & 0xFFFFu);
    }
    return f & 0xFFFFu;
}
uint32_t aret_GetMenuStringA(uint32_t esp) {
    char *buf = (char *)WP(2); uint32_t cch = WU(3);
    int i = u32_menu_idx(WU(0)); if (i < 0) { if (buf && cch) buf[0] = 0; return 0; }
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(4)); if (s < 0) { if (buf && cch) buf[0] = 0; return 0; }
    const char *t = g_u32_menu[i].it[s].text; uint32_t len = (uint32_t)strlen(t);
    if (!buf || cch == 0) return len;
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = t[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetMenuStringW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(2); uint32_t cch = WU(3);
    int i = u32_menu_idx(WU(0)); if (i < 0) { if (buf && cch) buf[0] = 0; return 0; }
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(4)); if (s < 0) { if (buf && cch) buf[0] = 0; return 0; }
    const char *t = g_u32_menu[i].it[s].text; uint32_t len = (uint32_t)strlen(t);
    if (!buf || cch == 0) return len;
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)t[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetSubMenu(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    int p = WI(1); if (p < 0 || p >= g_u32_menu[i].count) return 0;
    return (g_u32_menu[i].it[p].flags & 0x10u) ? g_u32_menu[i].it[p].submenu : 0;
}
uint32_t aret_GetMenuItemID(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0xFFFFFFFFu;
    int p = WI(1); if (p < 0 || p >= g_u32_menu[i].count) return 0xFFFFFFFFu;
    if (g_u32_menu[i].it[p].flags & 0x10u) return 0xFFFFFFFFu;  /* popup -> -1 */
    return g_u32_menu[i].it[p].id;
}
/* ModifyMenu replaces an existing item in place: the item found by uPosition
 * (MF_BYCOMMAND / MF_BYPOSITION) gets new flags (minus the lookup bit), id/submenu
 * and text — exactly u32_menu_setitem, at the found slot. Missing item -> FALSE. */
uint32_t aret_ModifyMenuA(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0;
    u32_menu_setitem(i, s, WU(2), WU(3), WCS(4));
    return 1;
}
uint32_t aret_ModifyMenuW(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0;
    char t[128]; t[0] = 0;
    if (!(WU(2) & (0x800u | 0x4u | 0x100u))) u32_w2n((const uint16_t *)WP(4), t, sizeof t);
    u32_menu_setitem(i, s, WU(2), WU(3), t);
    return 1;
}
/* SetMenuItemBitmaps: associate check-mark bitmaps with an item. Display-only in
 * effect (no headless getter), so we store the handles and return TRUE on the found
 * item / FALSE when the item does not exist (measured vs Wine). */
uint32_t aret_SetMenuItemBitmaps(uint32_t esp) {
    int i = u32_menu_idx(WU(0)); if (i < 0) return 0;
    int s = u32_menu_find(&g_u32_menu[i], WU(1), WU(2)); if (s < 0) return 0;
    g_u32_menu[i].it[s].bmp_unchecked = WU(3);
    g_u32_menu[i].it[s].bmp_checked   = WU(4);
    return 1;
}
/* GetMenuCheckMarkDimensions = MAKELONG(SM_CXMENUCHECK, SM_CYMENUCHECK); the default
 * check-mark bitmap is 13x13 (measured vs Wine). */
uint32_t aret_GetMenuCheckMarkDimensions(uint32_t esp) {
    (void)esp; return (13u << 16) | 13u;
}

/* Window menu bar + system menu. */
uint32_t aret_GetMenu(uint32_t esp) { int i = u32_win_idx(WU(0)); return i < 0 ? 0 : g_u32_wmenu[i]; }
uint32_t aret_SetMenu(uint32_t esp) { int i = u32_win_idx(WU(0)); if (i < 0) return 0; g_u32_wmenu[i] = WU(1); return 1; }
uint32_t aret_GetSystemMenu(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    if (WU(1)) {                                         /* bRevert: reset, return NULL */
        if (g_u32_wsysmenu[i]) { int mi = u32_menu_idx(g_u32_wsysmenu[i]); if (mi >= 0) g_u32_menu[mi].used = 0; g_u32_wsysmenu[i] = 0; }
        return 0;
    }
    if (!g_u32_wsysmenu[i]) {
        uint32_t h = u32_menu_new(); int mi = u32_menu_idx(h);
        if (mi >= 0) {
            static const struct { uint32_t id; const char *t; } sc[] = {
                {0xF120u, "&Restore"}, {0xF010u, "&Move"}, {0xF000u, "&Size"},
                {0xF020u, "Mi&nimize"}, {0xF030u, "Ma&ximize"}, {0xF060u, "&Close"} };
            for (int k = 0; k < 6; k++) { int s = g_u32_menu[mi].count++; u32_menu_setitem(mi, s, 0, sc[k].id, sc[k].t); }
        }
        g_u32_wsysmenu[i] = h;
    }
    return g_u32_wsysmenu[i];
}
/* TrackPopupMenu(Ex): a modal popup needs a display + user; headless there is no
 * selection -> return 0 (with TPM_RETURNCMD, 0 = "no item chosen"). */
uint32_t aret_TrackPopupMenu(uint32_t esp)   { (void)esp; return 0; }
uint32_t aret_TrackPopupMenuEx(uint32_t esp) { (void)esp; return 0; }

/* ================================================================== */
/* Misc: Global/Local lock+realloc, GetObject, FindWindow, TZ, StdHandle */
/* ================================================================== */
/* GMEM_FIXED handles are the malloc pointer itself (see GlobalAlloc), so Lock is
 * identity and Unlock succeeds; ReAlloc is realloc (GMEM_MOVEABLE may move). */
uint32_t aret_GlobalLock(uint32_t esp)    { return WU(0); }
uint32_t aret_GlobalUnlock(uint32_t esp)  { (void)esp; return 1; }
uint32_t aret_GlobalReAlloc(uint32_t esp) { return WRP(realloc(WP(0), WU(1))); }
uint32_t aret_LocalLock(uint32_t esp)     { return WU(0); }
uint32_t aret_LocalUnlock(uint32_t esp)   { (void)esp; return 1; }
uint32_t aret_LocalReAlloc(uint32_t esp)  { return WRP(realloc(WP(0), WU(1))); }

/* GetObjectA/W(hgdiobj, cb, lpv) -> bytes written. Fills BITMAP (24 bytes) for a
 * bitmap, LOGBRUSH (12) for a brush. Other objects: 0 (sound). */
uint32_t aret_GetObjectA(uint32_t esp) {
    int i = gdi_idx(WU(0)); int cb = WI(1); uint8_t *lpv = (uint8_t *)WP(2);
    if (i < 0 || !lpv) return 0;
    if (g_gdi[i].type == GDIT_BITMAP) {
        if (cb < 24) return 0;
        int32_t *w = (int32_t *)lpv;
        w[0] = 0;                                    /* bmType */
        w[1] = g_gdi[i].w;                           /* bmWidth */
        w[2] = g_gdi[i].h;                           /* bmHeight */
        w[3] = ((g_gdi[i].w * g_gdi[i].bpp + 15) / 16) * 2;  /* bmWidthBytes (WORD-aligned) */
        *(uint16_t *)(lpv + 16) = 1;                 /* bmPlanes */
        *(uint16_t *)(lpv + 18) = (uint16_t)g_gdi[i].bpp;    /* bmBitsPixel */
        *(uint32_t *)(lpv + 20) = (uint32_t)(uintptr_t)g_gdi[i].bits;  /* bmBits */
        return 24;
    }
    if (g_gdi[i].type == GDIT_BRUSH) {
        if (cb < 12) return 0;
        *(uint32_t *)(lpv + 0) = g_gdi[i].null_obj ? 1u : 0u;   /* lbStyle: BS_SOLID=0 / BS_NULL=1 */
        *(uint32_t *)(lpv + 4) = g_gdi[i].color;               /* lbColor */
        *(uint32_t *)(lpv + 8) = 0;                            /* lbHatch */
        return 12;
    }
    if (g_gdi[i].type == GDIT_PALETTE) {
        if (cb < 2) return 0;
        *(uint16_t *)lpv = (uint16_t)g_gdi[i].pal_count;        /* GetObject(hpal,2,&WORD) = entry count */
        return 2;
    }
    return 0;
}
uint32_t aret_GetObjectW(uint32_t esp) { return aret_GetObjectA(esp); }

/* FindWindowA/W(lpClassName, lpWindowName) -> HWND. Matched by window title (our
 * windows don't store a class name; a class-only query -> 0, which is also the
 * correct "no such window" for the common single-instance check). */
uint32_t aret_FindWindowA(uint32_t esp) {
    const char *cls = WCS(0); const char *title = WCS(1);
    if (cls && !title) return 0;                     /* class-only: cannot match, none found */
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) continue;
        if (title && strcmp(g_u32_win[i].title, title) != 0) continue;
        return (uint32_t)(i + 1);
    }
    return 0;
}
uint32_t aret_FindWindowW(uint32_t esp) {
    char title[256]; const char *tp = NULL;
    if (WP(1)) { u32_w2n((const uint16_t *)WP(1), title, sizeof title); tp = title; }
    if (WP(0) && !tp) return 0;
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used) continue;
        if (tp && strcmp(g_u32_win[i].title, tp) != 0) continue;
        return (uint32_t)(i + 1);
    }
    return 0;
}

/* GetTimeZoneInformation(TIME_ZONE_INFORMATION*) -> DWORD. ARET runs on a defined
 * UTC clock (tests fix TZ=UTC): zero the struct (Bias=0 => UTC, no DST) and report
 * TIME_ZONE_ID_UNKNOWN. */
uint32_t aret_GetTimeZoneInformation(uint32_t esp) {
    uint8_t *tzi = (uint8_t *)WP(0);
    if (tzi) memset(tzi, 0, 172);                    /* Bias + Std/Daylight name/date/bias */
    return 0;                                        /* TIME_ZONE_ID_UNKNOWN */
}
/* SetStdHandle(nStdHandle, hHandle) -> BOOL. Redirect a std stream (fd model). */
uint32_t aret_SetStdHandle(uint32_t esp) {
    int32_t which = WI(0);
    int slot = which == -10 ? 0 : which == -11 ? 1 : which == -12 ? 2 : -1;
    if (slot < 0) return 0;
    dup2((int)WU(1), slot);
    return 1;
}

/* ================================================================== */
/* Long-tail: FileTimeToSystemTime, GetDriveType, CharNext/Prev, coords */
/* ================================================================== */
/* FileTimeToSystemTime(const FILETIME*, SYSTEMTIME*) -> BOOL. Reverse of
 * SystemTimeToFileTime; civil date via gmtime (matches Wine). */
uint32_t aret_FileTimeToSystemTime(uint32_t esp) {
    const uint32_t *ft = (const uint32_t *)WP(0);
    uint16_t *w = (uint16_t *)WP(1);
    if (!ft || !w) return 0;
    uint64_t t = ((uint64_t)ft[1] << 32) | ft[0];
    uint32_t ms = (uint32_t)((t / 10000ull) % 1000ull);
    time_t sec = (time_t)(t / 10000000ull) - 11644473600ll;   /* 1601 -> 1970 epoch */
    struct tm tmv; gmtime_r(&sec, &tmv);
    w[0] = (uint16_t)(tmv.tm_year + 1900); w[1] = (uint16_t)(tmv.tm_mon + 1);
    w[2] = (uint16_t)tmv.tm_wday;          w[3] = (uint16_t)tmv.tm_mday;
    w[4] = (uint16_t)tmv.tm_hour;          w[5] = (uint16_t)tmv.tm_min;
    w[6] = (uint16_t)tmv.tm_sec;           w[7] = (uint16_t)ms;
    return 1;
}
/* GetDriveTypeA(lpRootPathName) -> UINT. Everything maps to the host filesystem,
 * so a drive root is DRIVE_FIXED (3) — the common case (C:). */
uint32_t aret_GetDriveTypeA(uint32_t esp) { (void)esp; return 3; }
uint32_t aret_GetDriveTypeW(uint32_t esp) { (void)esp; return 3; }

/* CharNextA/W(psz) -> the next character (single-byte / wide; at NUL, stays). */
uint32_t aret_CharNextA(uint32_t esp) { const char *p = WCS(0); return (!p || !*p) ? WU(0) : WU(0) + 1; }
uint32_t aret_CharNextW(uint32_t esp) { const uint16_t *p = (const uint16_t *)WP(0); return (!p || !*p) ? WU(0) : WU(0) + 2; }
/* CharPrevA/W(lpszStart, lpszCurrent) -> the previous character (bounded at start). */
uint32_t aret_CharPrevA(uint32_t esp) { uint32_t s = WU(0), c = WU(1); return c > s ? c - 1 : s; }
uint32_t aret_CharPrevW(uint32_t esp) { uint32_t s = WU(0), c = WU(1); return c > s + 1 ? c - 2 : s; }

/* ClientToScreen/ScreenToClient(hwnd, POINT*) -> BOOL. Client origin = the window
 * origin (no non-client area modelled), so the mapping is a translate by (x,y). */
uint32_t aret_ClientToScreen(uint32_t esp) {
    int i = u32_win_idx(WU(0)); int32_t *pt = (int32_t *)WP(1);
    if (i < 0 || !pt) return 0;
    pt[0] += g_u32_win[i].x; pt[1] += g_u32_win[i].y;
    return 1;
}
uint32_t aret_ScreenToClient(uint32_t esp) {
    int i = u32_win_idx(WU(0)); int32_t *pt = (int32_t *)WP(1);
    if (i < 0 || !pt) return 0;
    pt[0] -= g_u32_win[i].x; pt[1] -= g_u32_win[i].y;
    return 1;
}

/* ================================================================== */
/* Long-tail tail: CreateBitmap, cursor/popup, registry delete         */
/* ================================================================== */
/* CreateBitmap(w, h, planes, bpp, lpBits) -> HBITMAP. A device-dependent bitmap;
 * we back it with a 32-bit buffer (drawing is only pixel-exact for 32bpp) but
 * report the requested bpp/dims via GetObject. */
uint32_t aret_CreateBitmap(uint32_t esp) {
    int w = WI(0), h = WI(1), bpp = WI(3);
    if (w <= 0 || h <= 0) return 0;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = w; g_gdi[i].h = h; g_gdi[i].topdown = 1; g_gdi[i].bpp = bpp ? bpp : 32;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)w * h, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    return gdi_handle(i);
}

/* Cursor: a display-count model matching Wine (initial 0 with a mouse present).
 * SetCursor returns the previous cursor; the actual cursor image is display work. */
static int g_u32_cursor_show;
static uint32_t g_u32_cursor;
uint32_t aret_ShowCursor(uint32_t esp) { g_u32_cursor_show += WU(0) ? 1 : -1; return (uint32_t)g_u32_cursor_show; }
uint32_t aret_SetCursor(uint32_t esp)  { uint32_t p = g_u32_cursor; g_u32_cursor = WU(0); return p; }
uint32_t aret_GetCursor(uint32_t esp)  { (void)esp; return g_u32_cursor; }

/* ---- comctl32 socle batch 8: icons / cursors / DrawState. Resource icons (LoadIcon)
 * stay opaque tokens with no rasterised form. CreateIconIndirect keeps the caller's
 * colour/mask bitmaps in a table so GetIconInfo round-trips; per Wine, GetIconInfo on
 * an ICON reports the hotspot as the bitmap centre (cx/2,cy/2) and returns fresh bitmap
 * COPIES (only a CURSOR keeps its stored hotspot). CopyImage deep-copies a bitmap (with
 * optional resize). Drawing (DrawIcon/DrawIconEx/DrawStateW) blits the colour bitmap when
 * one is present, else is a sound no-op (an opaque token has no pixels to raster). */
static uint32_t u32_bitmap_copy(uint32_t hbm, int nw, int nh) {
    int s = gdi_idx(hbm); if (s < 0 || g_gdi[s].type != GDIT_BITMAP) return 0;
    int ow = g_gdi[s].w, oh = g_gdi[s].h;
    if (nw <= 0) nw = ow; if (nh <= 0) nh = oh;
    int i = gdi_alloc(GDIT_BITMAP); if (!i) return 0;
    g_gdi[i].w = nw; g_gdi[i].h = nh; g_gdi[i].topdown = g_gdi[s].topdown; g_gdi[i].bpp = g_gdi[s].bpp;
    g_gdi[i].bits = (uint8_t *)calloc((size_t)nw * nh, 4); g_gdi[i].owns_bits = 1;
    if (!g_gdi[i].bits) { g_gdi[i].used = 0; return 0; }
    struct gdi_obj *S = &g_gdi[s], *D = &g_gdi[i];
    for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
        uint8_t *sp = gdi_px(S, nw == ow ? x : x * ow / nw, nh == oh ? y : y * oh / nh), *dp = gdi_px(D, x, y);
        if (sp && dp) { dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3]; }
    }
    return gdi_handle(i);
}
#define U32_MAX_ICON 128
#define U32_ICON_BASE 0xCE000000u
static struct { int used, is_icon, hotx, hoty; uint32_t color, mask; } g_u32_icontab[U32_MAX_ICON];
static int u32_icon_idx(uint32_t h) {
    if ((h & 0xFF000000u) != U32_ICON_BASE) return -1;
    uint32_t i = h & 0x00FFFFFFu;
    return (i < U32_MAX_ICON && g_u32_icontab[i].used) ? (int)i : -1;
}
uint32_t aret_CreateIconIndirect(uint32_t esp) {
    const int32_t *ii = (const int32_t *)WP(0); if (!ii) return 0;   /* ICONINFO */
    int slot = -1; for (int i = 1; i < U32_MAX_ICON; i++) if (!g_u32_icontab[i].used) { slot = i; break; }
    if (slot < 0) return 0;
    g_u32_icontab[slot].used = 1; g_u32_icontab[slot].is_icon = ii[0] ? 1 : 0;
    g_u32_icontab[slot].hotx = ii[1]; g_u32_icontab[slot].hoty = ii[2];
    g_u32_icontab[slot].mask = (uint32_t)ii[3]; g_u32_icontab[slot].color = (uint32_t)ii[4];
    return U32_ICON_BASE | (uint32_t)slot;
}
uint32_t aret_GetIconInfo(uint32_t esp) {
    int i = u32_icon_idx(WU(0)); int32_t *o = (int32_t *)WP(1); if (!o || i < 0) return 0;
    o[0] = g_u32_icontab[i].is_icon;
    int cw = 16, ch = 16, ci = gdi_idx(g_u32_icontab[i].color);
    if (ci >= 0) { cw = g_gdi[ci].w; ch = g_gdi[ci].h; }
    if (g_u32_icontab[i].is_icon) { o[1] = cw / 2; o[2] = ch / 2; }   /* icon: hotspot = centre */
    else { o[1] = g_u32_icontab[i].hotx; o[2] = g_u32_icontab[i].hoty; }
    o[3] = (int32_t)u32_bitmap_copy(g_u32_icontab[i].mask, 0, 0);      /* fresh copies (Wine contract) */
    o[4] = (int32_t)u32_bitmap_copy(g_u32_icontab[i].color, 0, 0);
    return 1;
}
static uint32_t u32_icon_dup(uint32_t h) {
    int i = u32_icon_idx(h); if (i < 0) return h;                      /* opaque token: itself is a valid dup */
    int slot = -1; for (int k = 1; k < U32_MAX_ICON; k++) if (!g_u32_icontab[k].used) { slot = k; break; }
    if (slot < 0) return 0;
    g_u32_icontab[slot] = g_u32_icontab[i]; g_u32_icontab[slot].used = 1;
    g_u32_icontab[slot].color = u32_bitmap_copy(g_u32_icontab[i].color, 0, 0);   /* independent copy */
    g_u32_icontab[slot].mask = u32_bitmap_copy(g_u32_icontab[i].mask, 0, 0);
    return U32_ICON_BASE | (uint32_t)slot;
}
uint32_t aret_CopyIcon(uint32_t esp)      { return u32_icon_dup(WU(0)); }
uint32_t aret_CopyImage(uint32_t esp) {
    uint32_t h = WU(0), type = WU(1); int cx = WI(2), cy = WI(3);
    if (type == 0 /* IMAGE_BITMAP */) return u32_bitmap_copy(h, cx, cy);
    return u32_icon_dup(h);                                            /* IMAGE_ICON / IMAGE_CURSOR */
}
uint32_t aret_DestroyIcon(uint32_t esp)   { int i = u32_icon_idx(WU(0)); if (i >= 0) g_u32_icontab[i].used = 0; return 1; }
uint32_t aret_DestroyCursor(uint32_t esp) { int i = u32_icon_idx(WU(0)); if (i >= 0) g_u32_icontab[i].used = 0; return 1; }
static void u32_icon_blit(uint32_t hdc, int x, int y, int i, int cx, int cy) {
    struct gdi_obj *dst = gdi_dc_surface(hdc); if (!dst || i < 0) return;
    int ci = gdi_idx(g_u32_icontab[i].color); if (ci < 0) return;
    int ow = g_gdi[ci].w, oh = g_gdi[ci].h; if (cx <= 0) cx = ow; if (cy <= 0) cy = oh;
    for (int j = 0; j < cy; j++) for (int k = 0; k < cx; k++) {
        uint8_t *s = gdi_px(&g_gdi[ci], cx == ow ? k : k * ow / cx, cy == oh ? j : j * oh / cy), *d = gdi_px(dst, x + k, y + j);
        if (s && d) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
    }
}
uint32_t aret_DrawIcon(uint32_t esp) { u32_icon_blit(WU(0), WI(1), WI(2), u32_icon_idx(WU(3)), 0, 0); return 1; }
uint32_t aret_DrawIconEx(uint32_t esp) {
    uint32_t flags = WU(8);
    if ((flags & 0x0003) == 0x0001 /* DI_MASK without DI_IMAGE */) return 1;   /* mask-only: best-effort skip */
    u32_icon_blit(WU(0), WI(1), WI(2), u32_icon_idx(WU(3)), WI(4), WI(5)); return 1;
}
/* DrawStateW(hdc, hbrFore, lpFn, lData, wData, x, y, cx, cy, uFlags). Draws the tractable
 * primitive types (text/bitmap/icon) with the existing GDI helpers; the greyed
 * (DSS_DISABLED) and callback (DST_COMPLEX) forms are follow-ups (no headless oracle). */
uint32_t aret_DrawStateW(uint32_t esp) {
    uint32_t hdc = WU(0), lData = WU(3), wData = WU(4); int x = WI(5), y = WI(6); uint32_t flags = WU(9);
    int typ = flags & 0x1F;
    if (typ == 1 /* DST_TEXT */ || typ == 2 /* DST_PREFIXTEXT */) {
        const uint16_t *ws = (const uint16_t *)(uintptr_t)lData; int n = (int)wData;
        if (n <= 0) { n = 0; if (ws) while (ws[n]) n++; }
        uint32_t cps[1024]; int m = n < 1024 ? n : 1024;
        for (int i = 0; i < m; i++) cps[i] = ws ? ws[i] : 0;
        return u32_textout_core(hdc, x, y, cps, ws ? m : 0) ? 1 : 0;
    }
    if (typ == 4 /* DST_BITMAP */) {
        struct gdi_obj *dst = gdi_dc_surface(hdc); int s = gdi_idx(lData);
        if (dst && s >= 0 && g_gdi[s].type == GDIT_BITMAP)
            for (int j = 0; j < g_gdi[s].h; j++) for (int k = 0; k < g_gdi[s].w; k++) {
                uint8_t *sp = gdi_px(&g_gdi[s], k, j), *d = gdi_px(dst, x + k, y + j);
                if (sp && d) { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = sp[3]; }
            }
        return 1;
    }
    if (typ == 3 /* DST_ICON */) { u32_icon_blit(hdc, x, y, u32_icon_idx(lData), 0, 0); return 1; }
    aret_partial("DrawStateW: DST_COMPLEX (callback) not modelled"); return 0;
}

/* ---- comctl32 socle batch 9 (final): locale/date, polygons, heap, Uniscribe stubs.
 * Uniscribe (Script*) is NOT implemented — the shaping engine is out of scope. Instead
 * each returns a sound FAILURE so a lifted comctl32 takes its non-Uniscribe GDI text
 * path (which ARET renders bit-exactly). GetDateFormatW honours an explicit picture
 * (numeric fields bit-exact vs Wine; month/day names are English best-effort — a
 * locale-resolution caveat like gdi_uifont). */
static void u32_wcopy(uint16_t *dst, const char *s, int *pn) {
    while (*s) { if (dst) dst[*pn] = (uint16_t)(unsigned char)*s; (*pn)++; s++; }
}
static void u32_num2(uint16_t *dst, int v, int width, int *pn) {   /* v as `width`-digit, zero-padded */
    char b[16]; int n = 0; if (v < 0) v = 0;
    char t[16]; int tn = 0; do { t[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n + tn < width) { if (dst) dst[*pn + n] = '0'; n++; }
    for (int i = tn - 1; i >= 0; i--) { if (dst) dst[*pn + n] = (uint16_t)t[i]; n++; }
    *pn += n;
}
uint32_t aret_GetDateFormatW(uint32_t esp) {
    const uint16_t *st = (const uint16_t *)WP(2);          /* SYSTEMTIME (WORD fields) */
    const uint16_t *fmt = (const uint16_t *)WP(3);
    uint16_t *out = (uint16_t *)WP(4); int cch = WI(5);
    if (!st) return 0;
    int year = st[0], month = st[1], dow = st[2], day = st[3];
    static const char *mon[] = {"January","February","March","April","May","June","July",
        "August","September","October","November","December"};
    static const char *wd[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    uint16_t tmp[256]; int n = 0; uint16_t *dst = tmp;
    if (!fmt) { static const uint16_t def[] = {'M','/','d','/','y','y','y','y',0}; fmt = def; }   /* short default */
    for (int i = 0; fmt[i]; ) {
        uint16_t c = fmt[i];
        if (c == '\'') {                                    /* quoted literal */
            i++;
            while (fmt[i] && !(fmt[i] == '\'' )) { if (n < 255) dst[n++] = fmt[i]; i++; }
            if (fmt[i] == '\'') i++;
            continue;
        }
        if (c == 'y' || c == 'M' || c == 'd') {
            int run = 0; while (fmt[i] == c) { run++; i++; }
            int p = n;
            if (c == 'y') { if (run >= 3) u32_num2(dst, year, 4, &p); else u32_num2(dst, year % 100, 2, &p); }
            else if (c == 'M') {
                int mi = (month - 1 + 1200) % 12;                       /* 0-based month, guarded */
                if (run >= 4) u32_wcopy(dst, mon[mi], &p);
                else if (run == 3) { char a[4] = {mon[mi][0], mon[mi][1], mon[mi][2], 0}; u32_wcopy(dst, a, &p); }
                else u32_num2(dst, month, run, &p);
            } else {   /* 'd' */
                int di = (dow % 7 + 7) % 7;                             /* 0-based weekday (0=Sunday) */
                if (run >= 4) u32_wcopy(dst, wd[di], &p);
                else if (run == 3) { char a[4] = {wd[di][0], wd[di][1], wd[di][2], 0}; u32_wcopy(dst, a, &p); }
                else u32_num2(dst, day, run, &p);
            }
            n = p; continue;
        }
        if (n < 255) dst[n++] = c; i++;                     /* literal passthrough */
    }
    if (n > 255) n = 255; dst[n] = 0;
    if (cch == 0) return (uint32_t)(n + 1);                 /* required size incl. NUL */
    if (!out || cch < n + 1) return 0;
    for (int k = 0; k <= n; k++) out[k] = dst[k];
    return (uint32_t)(n + 1);
}
/* GetLocaleInfoW(lcid, lctype, buf, cch) — en-US/invariant defaults (locale-resolution
 * caveat like gdi_uifont). Feeds a lifted comctl32's date/time formatting; the numeric
 * request flag (LOCALE_RETURN_NUMBER, 0x20000000) writes a DWORD instead of a string. */
static const char *u32_locale_str(uint32_t t) {
    static const char *mon[] = {"January","February","March","April","May","June","July",
        "August","September","October","November","December"};
    static const char *wd[] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
    switch (t & 0xFFFF) {
        case 0x0001: return "0409";       /* ILANGUAGE */
        case 0x000E: return ".";          /* SDECIMAL */
        case 0x000F: return ",";          /* STHOUSAND */
        case 0x001D: return "/";          /* SDATE */
        case 0x001E: return ":";          /* STIME */
        case 0x001F: return "M/d/yyyy";   /* SSHORTDATE */
        case 0x0020: return "dddd, MMMM d, yyyy";  /* SLONGDATE */
        case 0x0028: return "AM";         /* S1159 */
        case 0x0029: return "PM";         /* S2359 */
        case 0x1003: return "h:mm:ss tt"; /* STIMEFORMAT */
        case 0x100C: return "6";          /* IFIRSTDAYOFWEEK (Monday=0 .. so 6=Sunday? en-US Sunday first) */
        default: break;
    }
    uint32_t lt = t & 0xFFFF;
    if (lt >= 0x002A && lt <= 0x0030) return wd[lt - 0x002A];            /* SDAYNAME1..7 (Mon..Sun) */
    if (lt >= 0x0038 && lt <= 0x0043) return mon[lt - 0x0038];           /* SMONTHNAME1..12 */
    return "";
}
uint32_t aret_GetLocaleInfoW(uint32_t esp) {
    uint32_t lctype = WU(1); uint16_t *buf = (uint16_t *)WP(2); int cch = WI(3);
    const char *s = u32_locale_str(lctype);
    if (lctype & 0x20000000u /* LOCALE_RETURN_NUMBER */) {
        if (buf && cch >= 2) { uint32_t v = (uint32_t)strtoul(s, NULL, 10); *(uint32_t *)buf = v; }
        return 2;
    }
    int len = 0; while (s[len]) len++;
    if (cch == 0) return (uint32_t)(len + 1);
    if (!buf || cch < len + 1) return 0;
    for (int i = 0; i < len; i++) buf[i] = (uint16_t)(unsigned char)s[i];
    buf[len] = 0; return (uint32_t)(len + 1);
}
uint32_t aret_LoadMenuA(uint32_t esp)     { (void)esp; return 0; }    /* no menu resource loaded (sound) */
uint32_t aret_PlayEnhMetaFile(uint32_t esp) { (void)esp; return 0; }  /* EMF playback unmodelled (sound fail) */
uint32_t aret_LocalSize(uint32_t esp) { void *p = WP(0); return p ? (uint32_t)malloc_usable_size(p) : 0; }
/* Polygon(hdc, POINT*, n): a FILLED polygon. Wine's interior rasterisation (its scanline
 * fill rule + edge rounding) is not reproduced bit-for-bit here — a naive even-odd fill
 * was MEASURED to diverge from Wine's DIB (hash 70a8e185 vs fd1adec5), i.e. it would paint
 * pixels Wine doesn't. Per the sacred principle (never a wrong output as correct) and
 * consistent with the vector-GDI section (Polygon/Ellipse/Arc = abort-sound), this stays a
 * loud abort until its fill is reproduced bit-exactly and DIB-hash verified. */
uint32_t aret_Polygon(uint32_t esp) {
    (void)esp; aret_partial("Polygon: filled-polygon rasterisation not bit-exact vs Wine (abort, not a guess)"); return 0;
}
/* PolyPolyline(hdc, POINT*, DWORD* counts, n): n independent OPEN polylines — pure pen
 * outline via the same Bresenham primitive as Polyline (already DIB-hash exact vs Wine),
 * no interior fill, so it composes soundly from a proven primitive. */
uint32_t aret_PolyPolyline(uint32_t esp) {
    GDI_MAP_GUARD(WU(0), 0);
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    struct gdi_obj *bm = gdi_dc_surface(WU(0));
    const int32_t *pt = (const int32_t *)WP(1); const uint32_t *cnt = (const uint32_t *)WP(2); int npoly = WI(3);
    if (!bm || !pt || !cnt) return 0;
    uint32_t pc; int pv = gdi_pen(d, &pc); if (pv <= 0) return pv == 0 ? 1 : 0;
    int off = 0;
    for (int p = 0; p < npoly; p++) {
        int c = (int)cnt[p];
        for (int i = 0; i + 1 < c; i++) gdi_bres(bm, pt[2*(off+i)], pt[2*(off+i)+1], pt[2*(off+i+1)], pt[2*(off+i+1)+1], pc);
        off += c;
    }
    return 1;
}
/* Uniscribe (Script*) — shaping engine out of scope. Each returns a sound FAILURE so a
 * lifted comctl32 falls back to its GDI text path (which ARET renders bit-exactly). No
 * standalone oracle: verified only in situ through the lifted-comctl32 integration. */
uint32_t aret_ScriptStringAnalyse(uint32_t esp) {
    uint32_t *pssa = (uint32_t *)WP(12); if (pssa) *pssa = 0;   /* SCRIPT_STRING_ANALYSIS out = NULL */
    return 0x80004005u;                                        /* E_FAIL -> caller uses non-USP path */
}
uint32_t aret_ScriptStringFree(uint32_t esp)              { (void)esp; return 0; }   /* S_OK (freeing NULL) */
uint32_t aret_ScriptStringOut(uint32_t esp)              { (void)esp; return 0x80004005u; }
uint32_t aret_ScriptStringCPtoX(uint32_t esp)            { (void)esp; return 0x80004005u; }
uint32_t aret_ScriptStringXtoCP(uint32_t esp)            { (void)esp; return 0x80004005u; }
uint32_t aret_ScriptStringGetLogicalWidths(uint32_t esp) { (void)esp; return 0x80004005u; }
uint32_t aret_ScriptBreak(uint32_t esp)                  { (void)esp; return 0x80004005u; }
uint32_t aret_ScriptString_pSize(uint32_t esp)           { (void)esp; return 0; }    /* NULL -> caller falls back */
uint32_t aret_ScriptString_pcOutChars(uint32_t esp)      { (void)esp; return 0; }
/* GetLastActivePopup(hWnd) -> hWnd (no popup owned -> the window itself). */
uint32_t aret_GetLastActivePopup(uint32_t esp) { return WU(0); }


/* ================================================================== */
/* advapi32 — SID / token model (structural)                          */
/* ================================================================== */
/* A SID is Revision(1) + SubAuthorityCount(1) + IdentifierAuthority(6) +
 * SubAuthority[N]*4. The structural APIs (build/compare/length/subauthorities)
 * are exact and oracle-able; token contents (the user's SID) are environment-
 * dependent, so those calls are modelled soundly but not bit-compared. */
static int u32_sid_len(const uint8_t *sid) { return 8 + 4 * sid[1]; }

uint32_t aret_AllocateAndInitializeSid(uint32_t esp) {
    const uint8_t *auth = (const uint8_t *)WP(0);    /* SID_IDENTIFIER_AUTHORITY */
    uint32_t count = WU(1) & 0xFF;
    uint32_t *pSid = (uint32_t *)WP(10);
    if (!auth || !pSid || count > 8) return 0;
    uint8_t *sid = (uint8_t *)malloc(8 + 4 * count);
    if (!sid) return 0;
    sid[0] = 1; sid[1] = (uint8_t)count;
    memcpy(sid + 2, auth, 6);
    for (uint32_t i = 0; i < count; i++) *(uint32_t *)(sid + 8 + 4 * i) = WU(2 + i);
    *pSid = (uint32_t)(uintptr_t)sid;
    return 1;
}
uint32_t aret_InitializeSid(uint32_t esp) {
    uint8_t *sid = (uint8_t *)WP(0); const uint8_t *auth = (const uint8_t *)WP(1);
    if (!sid || !auth) return 0;
    sid[0] = 1; sid[1] = (uint8_t)(WU(2) & 0xFF); memcpy(sid + 2, auth, 6);
    return 1;
}
uint32_t aret_GetLengthSid(uint32_t esp) { const uint8_t *s = (const uint8_t *)WP(0); return s ? (uint32_t)u32_sid_len(s) : 0; }
uint32_t aret_GetSidLengthRequired(uint32_t esp) { return 8 + 4 * (WU(0) & 0xFF); }
uint32_t aret_IsValidSid(uint32_t esp) { const uint8_t *s = (const uint8_t *)WP(0); return (s && s[0] == 1 && s[1] <= 15) ? 1u : 0u; }
uint32_t aret_EqualSid(uint32_t esp) {
    const uint8_t *a = (const uint8_t *)WP(0), *b = (const uint8_t *)WP(1);
    if (!a || !b) return 0;
    int la = u32_sid_len(a);
    return (la == u32_sid_len(b) && memcmp(a, b, la) == 0) ? 1u : 0u;
}
uint32_t aret_GetSidSubAuthorityCount(uint32_t esp)  { return WU(0) ? WU(0) + 1 : 0; }        /* &sid[1] */
uint32_t aret_GetSidSubAuthority(uint32_t esp)       { return WU(0) ? WU(0) + 8 + 4 * WU(1) : 0; }
uint32_t aret_GetSidIdentifierAuthority(uint32_t esp) { return WU(0) ? WU(0) + 2 : 0; }
uint32_t aret_FreeSid(uint32_t esp) { free(WP(0)); return 0; }
uint32_t aret_CopySid(uint32_t esp) {
    uint32_t cap = WU(0); uint8_t *dst = (uint8_t *)WP(1); const uint8_t *src = (const uint8_t *)WP(2);
    if (!dst || !src) return 0;
    int n = u32_sid_len(src);
    if ((uint32_t)n > cap) return 0;
    memcpy(dst, src, n);
    return 1;
}

/* Tokens: a single-user, non-elevated process model. A token handle is opaque. */
uint32_t aret_OpenProcessToken(uint32_t esp) { uint32_t *pt = (uint32_t *)WP(2); if (pt) *pt = 0x2A2A2A2Au; return 1; }
uint32_t aret_OpenThreadToken(uint32_t esp)  { uint32_t *pt = (uint32_t *)WP(3); if (pt) *pt = 0x2A2A2A2Au; return 1; }
uint32_t aret_AdjustTokenPrivileges(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_LookupPrivilegeValueA(uint32_t esp) {
    uint32_t *luid = (uint32_t *)WP(2);              /* LUID {LowPart, HighPart} */
    if (luid) { luid[0] = 0x13; luid[1] = 0; }
    return 1;
}
uint32_t aret_LookupPrivilegeValueW(uint32_t esp) { return aret_LookupPrivilegeValueA(esp); }
/* GetTokenInformation: model TokenElevation (not elevated — the common UAC check).
 * Unhandled classes return FALSE (honest: we don't model that token data). */
uint32_t aret_GetTokenInformation(uint32_t esp) {
    uint32_t cls = WU(1); uint8_t *buf = (uint8_t *)WP(2); uint32_t len = WU(3); uint32_t *ret = (uint32_t *)WP(4);
    if (cls == 20 /* TokenElevation */) { if (buf && len >= 4) *(uint32_t *)buf = 0; if (ret) *ret = 4; return 1; }
    if (cls == 18 /* TokenElevationType */) { if (buf && len >= 4) *(uint32_t *)buf = 1; if (ret) *ret = 4; return 1; } /* Default */
    return 0;                                        /* class not modelled */
}

/* ================================================================== */
/* Rect math, char case, pointer/input/path stubs                     */
/* ================================================================== */
uint32_t aret_SetRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); if (!r) return 0;
    r[0] = WI(1); r[1] = WI(2); r[2] = WI(3); r[3] = WI(4); return 1;
}
uint32_t aret_SetRectEmpty(uint32_t esp) { int32_t *r = (int32_t *)WP(0); if (!r) return 0; r[0]=r[1]=r[2]=r[3]=0; return 1; }
uint32_t aret_CopyRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *s = (const int32_t *)WP(1);
    if (!d || !s) return 0; d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; return 1;
}
uint32_t aret_IsRectEmpty(uint32_t esp) {
    const int32_t *r = (const int32_t *)WP(0);
    return (!r || r[0] >= r[2] || r[1] >= r[3]) ? 1u : 0u;
}
uint32_t aret_EqualRect(uint32_t esp) {
    const int32_t *a = (const int32_t *)WP(0), *b = (const int32_t *)WP(1);
    if (!a || !b) return 0;
    return (a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]) ? 1u : 0u;
}
uint32_t aret_PtInRect(uint32_t esp) {
    const int32_t *r = (const int32_t *)WP(0); int32_t x = WI(1), y = WI(2);
    if (!r) return 0;
    return (x >= r[0] && x < r[2] && y >= r[1] && y < r[3]) ? 1u : 0u;
}
uint32_t aret_OffsetRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); int32_t dx = WI(1), dy = WI(2);
    if (!r) return 0; r[0]+=dx; r[2]+=dx; r[1]+=dy; r[3]+=dy; return 1;
}
uint32_t aret_InflateRect(uint32_t esp) {
    int32_t *r = (int32_t *)WP(0); int32_t dx = WI(1), dy = WI(2);
    if (!r) return 0; r[0]-=dx; r[2]+=dx; r[1]-=dy; r[3]+=dy; return 1;
}
uint32_t aret_IntersectRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *a = (const int32_t *)WP(1), *b = (const int32_t *)WP(2);
    if (!d || !a || !b) return 0;
    int32_t l = a[0] > b[0] ? a[0] : b[0], t = a[1] > b[1] ? a[1] : b[1];
    int32_t r = a[2] < b[2] ? a[2] : b[2], bo = a[3] < b[3] ? a[3] : b[3];
    if (l >= r || t >= bo) { d[0]=d[1]=d[2]=d[3]=0; return 0; }
    d[0]=l; d[1]=t; d[2]=r; d[3]=bo; return 1;
}
uint32_t aret_UnionRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *a = (const int32_t *)WP(1), *b = (const int32_t *)WP(2);
    if (!d || !a || !b) return 0;
    int ae = (a[0] >= a[2] || a[1] >= a[3]), be = (b[0] >= b[2] || b[1] >= b[3]);
    if (ae && be) { d[0]=d[1]=d[2]=d[3]=0; return 0; }
    if (ae) { d[0]=b[0]; d[1]=b[1]; d[2]=b[2]; d[3]=b[3]; return 1; }
    if (be) { d[0]=a[0]; d[1]=a[1]; d[2]=a[2]; d[3]=a[3]; return 1; }
    d[0] = a[0] < b[0] ? a[0] : b[0]; d[1] = a[1] < b[1] ? a[1] : b[1];
    d[2] = a[2] > b[2] ? a[2] : b[2]; d[3] = a[3] > b[3] ? a[3] : b[3];
    return 1;
}

/* CharUpper/Lower A/W: a string pointer (in place), or — if the high word is 0 —
 * a single character to convert (returned in the low byte). */
uint32_t aret_CharUpperA(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(unsigned char)toupper((int)(p & 0xFF));
    char *s = (char *)(uintptr_t)p; for (; *s; s++) *s = (char)toupper((unsigned char)*s);
    return p;
}
uint32_t aret_CharLowerA(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(unsigned char)tolower((int)(p & 0xFF));
    char *s = (char *)(uintptr_t)p; for (; *s; s++) *s = (char)tolower((unsigned char)*s);
    return p;
}
uint32_t aret_CharUpperW(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(uint16_t)toupper((int)(p & 0xFF));
    uint16_t *s = (uint16_t *)(uintptr_t)p; for (; *s; s++) if (*s < 256) *s = (uint16_t)toupper(*s);
    return p;
}
uint32_t aret_CharLowerW(uint32_t esp) {
    uint32_t p = WU(0);
    if (p < 0x10000) return (uint32_t)(uint16_t)tolower((int)(p & 0xFF));
    uint16_t *s = (uint16_t *)(uintptr_t)p; for (; *s; s++) if (*s < 256) *s = (uint16_t)tolower(*s);
    return p;
}
uint32_t aret_CharUpperBuffA(uint32_t esp) {
    char *s = (char *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) s[i] = (char)toupper((unsigned char)s[i]);
    return n;
}
uint32_t aret_CharLowerBuffA(uint32_t esp) {
    char *s = (char *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) s[i] = (char)tolower((unsigned char)s[i]);
    return n;
}
/* Wide case-mapping over a buffer (comctl32 socle). ASCII fold in the C locale. */
uint32_t aret_CharLowerBuffW(uint32_t esp) {
    uint16_t *s = (uint16_t *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) if (s[i] < 128) s[i] = (uint16_t)tolower(s[i]);
    return n;
}
uint32_t aret_CharUpperBuffW(uint32_t esp) {
    uint16_t *s = (uint16_t *)WP(0); uint32_t n = WU(1);
    if (s) for (uint32_t i = 0; i < n; i++) if (s[i] < 128) s[i] = (uint16_t)toupper(s[i]);
    return n;
}

/* ---- comctl32 socle shims (batch 1) : simple, high-breadth, Wine-verifiable ---- */
/* CompareFileTime(a,b) -> -1/0/1 on the 64-bit FILETIME. */
uint32_t aret_CompareFileTime(uint32_t esp) {
    const uint32_t *a = (const uint32_t *)WP(0), *b = (const uint32_t *)WP(1);
    if (!a || !b) return 0;
    uint64_t va = ((uint64_t)a[1] << 32) | a[0], vb = ((uint64_t)b[1] << 32) | b[0];
    return (uint32_t)(va < vb ? -1 : va > vb ? 1 : 0);
}
/* GetDoubleClickTime() -> ms. The Windows default (measured). */
uint32_t aret_GetDoubleClickTime(uint32_t esp) { (void)esp; return 500; }
/* IsChild(hWndParent, hWnd) -> BOOL: is hWnd a descendant of hWndParent (child chain). */
uint32_t aret_IsChild(uint32_t esp) {
    uint32_t anc = WU(0), h = WU(1);
    for (int guard = 0; guard < U32_MAX_WIN; guard++) {
        int i = (h >= 1 && h <= U32_MAX_WIN && g_u32_win[h - 1].used) ? (int)h - 1 : -1;
        if (i < 0 || !(g_u32_win[i].style & 0x40000000u /*WS_CHILD*/)) return 0;
        h = g_u32_win[i].parent;
        if (h == anc) return 1;
        if (!h) return 0;
    }
    return 0;
}
/* GetObjectType(hgdiobj) -> OBJ_* (GDI object class). */
uint32_t aret_GetObjectType(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0) return 0;
    switch (g_gdi[i].type) {
        case GDIT_DC:     return 3;   /* OBJ_DC */
        case GDIT_PEN:    return 1;   /* OBJ_PEN */
        case GDIT_BRUSH:  return 2;   /* OBJ_BRUSH */
        case GDIT_FONT:   return 6;   /* OBJ_FONT */
        case GDIT_BITMAP: return 7;   /* OBJ_BITMAP */
        case GDIT_RGN:    return 8;   /* OBJ_REGION */
        default: return 0;
    }
}
/* GetBkMode(hdc) -> TRANSPARENT/OPAQUE. */
uint32_t aret_GetBkMode(uint32_t esp) { int i = gdi_idx(WU(0)); return i < 0 ? 0 : (uint32_t)g_gdi[i].bk_mode; }
/* StrCmpIW/StrCmpNIW (shlwapi, case-insensitive ordinal, ASCII fold) -> sign. */
static int u32_wcsicmp_n(const uint16_t *a, const uint16_t *b, long n) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    for (long k = 0; n < 0 || k < n; k++) {
        int ca = a[k], cb = b[k];
        int la = (ca < 128) ? tolower(ca) : ca, lb = (cb < 128) ? tolower(cb) : cb;
        if (la != lb) return la < lb ? -1 : 1;
        if (!ca) break;
    }
    return 0;
}
uint32_t aret_StrCmpIW(uint32_t esp)  { return (uint32_t)u32_wcsicmp_n((const uint16_t *)WP(0), (const uint16_t *)WP(1), -1); }
uint32_t aret_StrCmpNIW(uint32_t esp) { return (uint32_t)u32_wcsicmp_n((const uint16_t *)WP(0), (const uint16_t *)WP(1), (long)WI(2)); }
/* Monitors: a single primary monitor (the virtual-desktop invariant, like GetSystemMetrics). */
uint32_t aret_MonitorFromWindow(uint32_t esp) { (void)esp; return 0x00010001u; }
uint32_t aret_MonitorFromRect(uint32_t esp)   { (void)esp; return 0x00010001u; }
uint32_t aret_MonitorFromPoint(uint32_t esp)  { (void)esp; return 0x00010001u; }
uint32_t aret_GetMonitorInfoW(uint32_t esp) {
    uint32_t *mi = (uint32_t *)WP(1);           /* MONITORINFO: cbSize, rcMonitor[4], rcWork[4], dwFlags */
    if (!mi) return 0;
    mi[1] = 0; mi[2] = 0; mi[3] = 1024; mi[4] = 768;     /* rcMonitor */
    mi[5] = 0; mi[6] = 0; mi[7] = 1024; mi[8] = 768;     /* rcWork */
    mi[9] = 1;                                           /* MONITORINFOF_PRIMARY */
    return 1;
}
uint32_t aret_GetMonitorInfoA(uint32_t esp) { return aret_GetMonitorInfoW(esp); }
/* GetDpiForWindow(hwnd) -> 96 (our fixed logical DPI, matches GetDeviceCaps LOGPIXELS). */
uint32_t aret_GetDpiForWindow(uint32_t esp) { (void)esp; return 96; }

/* ---- comctl32 socle batch 2 : clipboard (in-memory), caret (state), IMM (no IME) ---- */
/* In-process clipboard: format -> handle, so Set/GetClipboardData round-trips (sound —
 * an empty format returns NULL like a real empty clipboard). Bounded table. */
static struct { uint32_t fmt, handle; } g_u32_clip[32];
static int g_u32_clip_n;
uint32_t aret_OpenClipboard(uint32_t esp)  { (void)esp; return 1; }
uint32_t aret_CloseClipboard(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_EmptyClipboard(uint32_t esp) { (void)esp; g_u32_clip_n = 0; return 1; }
uint32_t aret_SetClipboardData(uint32_t esp) {
    uint32_t fmt = WU(0), h = WU(1);
    for (int i = 0; i < g_u32_clip_n; i++) if (g_u32_clip[i].fmt == fmt) { g_u32_clip[i].handle = h; return h; }
    if (g_u32_clip_n < 32) { g_u32_clip[g_u32_clip_n].fmt = fmt; g_u32_clip[g_u32_clip_n].handle = h; g_u32_clip_n++; }
    return h;
}
uint32_t aret_GetClipboardData(uint32_t esp) {
    uint32_t fmt = WU(0);
    for (int i = 0; i < g_u32_clip_n; i++) if (g_u32_clip[i].fmt == fmt) return g_u32_clip[i].handle;
    return 0;
}
uint32_t aret_IsClipboardFormatAvailable(uint32_t esp) {
    uint32_t fmt = WU(0);
    for (int i = 0; i < g_u32_clip_n; i++) if (g_u32_clip[i].fmt == fmt) return 1;
    return 0;
}
/* Caret: position state (round-trips); show/hide/create/destroy are display no-ops. */
static int32_t g_u32_caret_x, g_u32_caret_y;
uint32_t aret_CreateCaret(uint32_t esp)  { (void)esp; return 1; }
uint32_t aret_DestroyCaret(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_ShowCaret(uint32_t esp)    { (void)esp; return 1; }
uint32_t aret_HideCaret(uint32_t esp)    { (void)esp; return 1; }
uint32_t aret_SetCaretPos(uint32_t esp)  { g_u32_caret_x = WI(0); g_u32_caret_y = WI(1); return 1; }
uint32_t aret_GetCaretPos(uint32_t esp)  { int32_t *p = (int32_t *)WP(0); if (p) { p[0] = g_u32_caret_x; p[1] = g_u32_caret_y; } return 1; }
/* IMM (input method): no IME present — the sound, correct state on a non-CJK setup. */
uint32_t aret_ImmGetContext(uint32_t esp)             { (void)esp; return 0; }
uint32_t aret_ImmReleaseContext(uint32_t esp)         { (void)esp; return 1; }
uint32_t aret_ImmGetCompositionStringW(uint32_t esp)  { (void)esp; return 0; }
uint32_t aret_ImmSetCompositionFontW(uint32_t esp)    { (void)esp; return 1; }
uint32_t aret_ImmSetCompositionWindow(uint32_t esp)   { (void)esp; return 1; }
/* Misc display/no-op (sound: no wrong data, only a display effect we don't model). */
uint32_t aret_NotifyWinEvent(uint32_t esp)  { (void)esp; return 0; }
uint32_t aret_TrackMouseEvent(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_KillSystemTimer(uint32_t esp) { (void)esp; return 1; }
uint32_t aret_GetLayout(uint32_t esp)       { (void)esp; return 0; }   /* LAYOUT_LTR (no RTL mirror) */

/* ---- comctl32 socle batch 3 : GDI regions (rect model) + object/rect/nav ---- */
/* Regions are modelled as their bounding rect (the rectangular subset is exact; a
 * non-rect result — OR of disjoint, DIFF/XOR — keeps the bounding box, a superset used
 * for coarser invalidation; documented, never a wrong data value). */
static uint32_t u32_make_rgn(int l, int t, int r, int b) {
    int i = gdi_alloc(GDIT_RGN); if (!i) return 0;
    if (r < l) { int x = l; l = r; r = x; } if (b < t) { int y = t; t = b; b = y; }
    g_gdi[i].rgn_l = l; g_gdi[i].rgn_t = t; g_gdi[i].rgn_r = r; g_gdi[i].rgn_b = b;
    return gdi_handle(i);
}
uint32_t aret_CreateRectRgn(uint32_t esp)         { return u32_make_rgn(WI(0), WI(1), WI(2), WI(3)); }
uint32_t aret_CreateRectRgnIndirect(uint32_t esp) { const int32_t *r = (const int32_t *)WP(0); return r ? u32_make_rgn(r[0], r[1], r[2], r[3]) : 0; }
uint32_t aret_CreateRoundRectRgn(uint32_t esp) { uint32_t h = u32_make_rgn(WI(0), WI(1), WI(2), WI(3)); int i = gdi_idx(h); if (i >= 0) g_gdi[i].rgn_complex = 1; return h; }   /* bbox; non-rect */
uint32_t aret_CreatePolygonRgn(uint32_t esp) {
    const int32_t *pts = (const int32_t *)WP(0); int n = WI(1); if (!pts || n <= 0) return u32_make_rgn(0, 0, 0, 0);
    int l = pts[0], t = pts[1], r = pts[0], b = pts[1];
    for (int i = 1; i < n; i++) { int x = pts[2*i], y = pts[2*i+1]; if (x<l)l=x; if (x>r)r=x; if (y<t)t=y; if (y>b)b=y; }
    uint32_t h = u32_make_rgn(l, t, r, b); int gi = gdi_idx(h); if (gi >= 0) g_gdi[gi].rgn_complex = 1; return h;   /* bbox; non-rect */
}
uint32_t aret_SetRectRgn(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0) return 0;
    int l = WI(1), t = WI(2), r = WI(3), b = WI(4);
    if (r < l) { int x = l; l = r; r = x; } if (b < t) { int y = t; t = b; b = y; }
    g_gdi[i].rgn_l = l; g_gdi[i].rgn_t = t; g_gdi[i].rgn_r = r; g_gdi[i].rgn_b = b; g_gdi[i].rgn_complex = 0; return 1;
}
static uint32_t u32_rgn_type(int i) {
    if (i < 0 || g_gdi[i].rgn_r <= g_gdi[i].rgn_l || g_gdi[i].rgn_b <= g_gdi[i].rgn_t) return 1;  /* NULLREGION */
    return g_gdi[i].rgn_complex ? 3 /*COMPLEXREGION*/ : 2 /*SIMPLEREGION*/;
}
uint32_t aret_GetRgnBox(uint32_t esp) {
    int i = gdi_idx(WU(0)); int32_t *rc = (int32_t *)WP(1); if (i < 0 || !rc) return 0;
    rc[0] = g_gdi[i].rgn_l; rc[1] = g_gdi[i].rgn_t; rc[2] = g_gdi[i].rgn_r; rc[3] = g_gdi[i].rgn_b;
    return u32_rgn_type(i);
}
/* rect a contains rect b? */
static int u32_rect_contains(int i, int j) {
    return g_gdi[i].rgn_l <= g_gdi[j].rgn_l && g_gdi[i].rgn_t <= g_gdi[j].rgn_t
        && g_gdi[i].rgn_r >= g_gdi[j].rgn_r && g_gdi[i].rgn_b >= g_gdi[j].rgn_b;
}
uint32_t aret_CombineRgn(uint32_t esp) {
    int d = gdi_idx(WU(0)), a = gdi_idx(WU(1)), b = gdi_idx(WU(2)); uint32_t mode = WU(3);
    if (d < 0 || a < 0) return 0;
    int l = g_gdi[a].rgn_l, t = g_gdi[a].rgn_t, r = g_gdi[a].rgn_r, bo = g_gdi[a].rgn_b, cx = g_gdi[a].rgn_complex;
    if (mode == 1 /*RGN_AND*/ && b >= 0) {
        if (g_gdi[b].rgn_l > l) l = g_gdi[b].rgn_l; if (g_gdi[b].rgn_t > t) t = g_gdi[b].rgn_t;
        if (g_gdi[b].rgn_r < r) r = g_gdi[b].rgn_r; if (g_gdi[b].rgn_b < bo) bo = g_gdi[b].rgn_b;
        if (r < l) r = l; if (bo < t) bo = t; cx = 0;               /* rect ∩ rect = rect */
    } else if (mode == 2 /*RGN_OR*/ && b >= 0) {
        /* OR is a rect only if one contains the other; otherwise COMPLEX (measured vs Wine). */
        cx = (u32_rect_contains(a, b) || u32_rect_contains(b, a)) ? 0 : 1;
        if (g_gdi[b].rgn_l < l) l = g_gdi[b].rgn_l; if (g_gdi[b].rgn_t < t) t = g_gdi[b].rgn_t;
        if (g_gdi[b].rgn_r > r) r = g_gdi[b].rgn_r; if (g_gdi[b].rgn_b > bo) bo = g_gdi[b].rgn_b;
    } else if ((mode == 3 /*XOR*/ || mode == 4 /*DIFF*/) && b >= 0) {
        cx = 1;   /* generally non-rect; keep src1 bbox (approx) */
    }   /* RGN_COPY(5) = src1 (type + bbox preserved) */
    g_gdi[d].rgn_l = l; g_gdi[d].rgn_t = t; g_gdi[d].rgn_r = r; g_gdi[d].rgn_b = bo; g_gdi[d].rgn_complex = cx;
    return u32_rgn_type(d);
}
/* GetCurrentObject(hdc, OBJ_type) -> the DC's currently selected object of that type. */
uint32_t aret_GetCurrentObject(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0) return 0;
    switch (WU(1)) { case 1: return g_gdi[i].sel_pen; case 2: return g_gdi[i].sel_brush;
                     case 6: return g_gdi[i].sel_font; case 7: return g_gdi[i].sel_bitmap; default: return 0; }
}
uint32_t aret_SetPolyFillMode(uint32_t esp) { (void)esp; return 1; }   /* ALTERNATE default (Polygon = abort-sound) */
/* SubtractRect(dst, src1, src2): src1 minus src2 when the result is a rectangle, else src1. */
uint32_t aret_SubtractRect(uint32_t esp) {
    int32_t *d = (int32_t *)WP(0); const int32_t *a = (const int32_t *)WP(1), *b = (const int32_t *)WP(2);
    if (!d || !a || !b) return 0;
    d[0] = a[0]; d[1] = a[1]; d[2] = a[2]; d[3] = a[3];
    int overlap = !(b[2] <= a[0] || b[0] >= a[2] || b[3] <= a[1] || b[1] >= a[3]);
    if (overlap) {
        if (b[0] <= a[0] && b[2] >= a[2]) {                      /* spans full width */
            if (b[1] <= a[1]) d[1] = b[3] > a[3] ? a[3] : b[3];  /* clips top */
            else if (b[3] >= a[3]) d[3] = b[1];                  /* clips bottom */
        } else if (b[1] <= a[1] && b[3] >= a[3]) {               /* spans full height */
            if (b[0] <= a[0]) d[0] = b[2] > a[2] ? a[2] : b[2];  /* clips left */
            else if (b[2] >= a[2]) d[2] = b[0];                  /* clips right */
        }
    }
    if (d[0] < d[2] && d[1] < d[3]) return 1;
    d[0] = d[1] = d[2] = d[3] = 0; return 0;
}
/* GetNextDlgTabItem / GetNextDlgGroupItem: next (or previous) child control of the dialog
 * in slot (z) order, wrapping. Tab variant skips non-tabstop; group variant stays in the
 * sibling run. Simplified to the common single-group dialog. */
static uint32_t u32_next_dlg_item(uint32_t hDlg, uint32_t cur, int prev, int tab) {
    int di = u32_win_idx(hDlg); if (di < 0) return 0;
    int kids[U32_MAX_WIN], nk = 0, curpos = -1;
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].parent == hDlg && g_u32_win[i].visible && g_u32_win[i].enabled) {
            if (tab && !(g_u32_win[i].style & 0x00010000u /*WS_TABSTOP*/)) continue;
            if ((uint32_t)(i + 1) == cur) curpos = nk;
            kids[nk++] = i;
        }
    if (nk == 0) return 0;
    if (curpos < 0) return (uint32_t)(kids[0] + 1);
    int np = prev ? (curpos - 1 + nk) % nk : (curpos + 1) % nk;
    return (uint32_t)(kids[np] + 1);
}
uint32_t aret_GetNextDlgTabItem(uint32_t esp)   { return u32_next_dlg_item(WU(0), WU(1), (int)WU(2), 1); }
uint32_t aret_GetNextDlgGroupItem(uint32_t esp) { return u32_next_dlg_item(WU(0), WU(1), (int)WU(2), 0); }

/* ---- comctl32 socle batch 4 : window nav + resources + simple constants/no-ops ---- */
uint32_t aret_SetParent(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    uint32_t old = g_u32_win[i].parent; g_u32_win[i].parent = WU(1); return old;
}
/* EnumChildWindows(hwnd, proc, lParam): call proc(child, lParam) for each child until it
 * returns FALSE. */
uint32_t aret_EnumChildWindows(uint32_t esp) {
    uint32_t parent = WU(0), proc = WU(1), lp = WU(2);
    if (!proc) return 0;
    for (int i = 0; i < U32_MAX_WIN; i++) {
        if (!g_u32_win[i].used || g_u32_win[i].parent != parent) continue;
        uint32_t frame = (esp - 64) & ~15u; uint32_t *f = (uint32_t *)(uintptr_t)frame;
        f[0] = 0; f[1] = (uint32_t)(i + 1); f[2] = lp;
        if ((uint32_t)aret_call(proc, frame, 0, 0, 0, 0, 0, 0, 0) == 0) break;
    }
    return 1;
}
/* ChildWindowFromPoint(parent, POINT): the child at the client point, else the parent. */
uint32_t aret_ChildWindowFromPoint(uint32_t esp) {
    uint32_t par = WU(0); int px = WI(1), py = WI(2);
    for (int c = 0; c < U32_MAX_WIN; c++)
        if (g_u32_win[c].used && g_u32_win[c].parent == par && g_u32_win[c].visible
            && px >= g_u32_win[c].x && px < g_u32_win[c].x + g_u32_win[c].w
            && py >= g_u32_win[c].y && py < g_u32_win[c].y + g_u32_win[c].h)
            return (uint32_t)(c + 1);
    return par;
}
/* WindowFromPoint(POINT screen): the top-level visible window under the point (0 if none). */
uint32_t aret_WindowFromPoint(uint32_t esp) {
    int x = WI(0), y = WI(1);
    for (int i = 0; i < U32_MAX_WIN; i++)
        if (g_u32_win[i].used && g_u32_win[i].visible && g_u32_win[i].parent == 0
            && x >= g_u32_win[i].x && x < g_u32_win[i].x + g_u32_win[i].w
            && y >= g_u32_win[i].y && y < g_u32_win[i].y + g_u32_win[i].h)
            return (uint32_t)(i + 1);
    return 0;
}
uint32_t aret_GetDC(uint32_t esp);   /* fwd */
uint32_t aret_GetDCEx(uint32_t esp)  { return aret_GetDC(esp); }   /* clip region/flags ignored */
uint32_t aret_SetTimer(uint32_t esp);/* fwd */
uint32_t aret_SetSystemTimer(uint32_t esp) { return aret_SetTimer(esp); }
/* LoadStringW: copy the RT_STRING entry as wide (no narrowing). */
uint32_t aret_LoadStringW(uint32_t esp) {
    uint32_t uID = WU(1); uint16_t *buf = (uint16_t *)WP(2); uint32_t cch = WU(3);
    if (!buf || cch == 0) return 0;
    const uint8_t *de = u32_rsrc_data_entry(6 /*RT_STRING*/, uID / 16 + 1);
    if (!de) { buf[0] = 0; return 0; }
    const uint16_t *p = (const uint16_t *)(uintptr_t)(aret_image_lo + *(const uint32_t *)de);
    for (uint32_t i = 0; i < uID % 16; i++) p += 1 + *p;
    uint16_t len = *p++; uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t i = 0; i < n; i++) buf[i] = p[i]; buf[n] = 0; return n;
}
uint32_t aret_FindResourceA(uint32_t esp);   /* fwd */
uint32_t aret_FindResourceW(uint32_t esp) { return aret_FindResourceA(esp); }  /* integer/atom resources */
/* InternalGetWindowText(hwnd, buf, cch) — wide, like GetWindowTextW. */
uint32_t aret_InternalGetWindowText(uint32_t esp) {
    uint32_t o = 0; u32_defproc_text(WU(0), U32_WM_GETTEXT, WU(2), WU(1), 1 /*wide*/, &o); return o;
}
/* --- Palette family. On our truecolor (32bpp) target a palette does no colour
 * remapping, but the object model + queries must match Wine bit-for-bit so palette-using
 * Win95 apps run instead of aborting at CreatePalette. Measured vs Wine (gdi_palette):
 * RealizePalette->0, GetNearestColor->identity, GetSystemPaletteEntries->0 but fills the
 * default static palette, GetPaletteEntries round-trips CreatePalette, ResizePalette->1. */

/* The 20 static system-palette colours Windows reserves (indices 0-9 and 246-255),
 * measured from Wine on a truecolor DC. {R,G,B}; all other indices are 0. */
static const uint8_t u32_syspal_static[20][3] = {
    {0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240},
    {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}
};
static void u32_syspal_entry(int idx, uint8_t out[4]) {   /* default system-palette entry idx (0..255) */
    out[0] = out[1] = out[2] = out[3] = 0;
    if (idx >= 0 && idx <= 9)          { const uint8_t *e = u32_syspal_static[idx];      out[0]=e[0]; out[1]=e[1]; out[2]=e[2]; }
    else if (idx >= 246 && idx <= 255) { const uint8_t *e = u32_syspal_static[10+idx-246]; out[0]=e[0]; out[1]=e[1]; out[2]=e[2]; }
}
/* Lazily-created DEFAULT_PALETTE (the 20 static colours) — SelectPalette returns it as the
 * "previously selected" palette when none was set (Wine returns a non-null default). */
static uint32_t u32_default_palette(void) {
    static uint32_t h = 0;
    if (!h) {
        int i = gdi_alloc(GDIT_PALETTE); if (!i) return 0;
        g_gdi[i].stock = 1; g_gdi[i].pal_count = 20;
        g_gdi[i].pal = (uint8_t *)malloc(20 * 4);
        if (g_gdi[i].pal) for (int k = 0; k < 20; k++) u32_syspal_entry(k < 10 ? k : 246 + (k - 10), g_gdi[i].pal + k * 4);
        h = gdi_handle(i);
    }
    return h;
}
uint32_t aret_CreatePalette(uint32_t esp) {
    const uint8_t *lp = (const uint8_t *)WP(0); if (!lp) return 0;
    uint32_t n = *(const uint16_t *)(lp + 2);          /* LOGPALETTE.palNumEntries (after WORD palVersion) */
    int i = gdi_alloc(GDIT_PALETTE); if (!i) return 0;
    g_gdi[i].pal_count = (int)n;
    if (n) { g_gdi[i].pal = (uint8_t *)malloc((size_t)n * 4); if (g_gdi[i].pal) memcpy(g_gdi[i].pal, lp + 4, (size_t)n * 4); }
    return gdi_handle(i);
}
uint32_t aret_GetPaletteEntries(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_PALETTE) return 0;
    uint32_t start = WU(1), count = WU(2); uint8_t *out = (uint8_t *)WP(3);
    if (count == 0) return (uint32_t)g_gdi[i].pal_count;    /* count 0 = query total (MSDN) */
    if (!out) return 0;
    uint32_t got = 0;
    for (uint32_t k = 0; k < count && (int)(start + k) < g_gdi[i].pal_count; k++) { memcpy(out + k * 4, g_gdi[i].pal + (start + k) * 4, 4); got++; }
    return got;
}
uint32_t aret_SetPaletteEntries(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_PALETTE || !g_gdi[i].pal) return 0;
    uint32_t start = WU(1), count = WU(2); const uint8_t *in = (const uint8_t *)WP(3); if (!in) return 0;
    uint32_t set = 0;
    for (uint32_t k = 0; k < count && (int)(start + k) < g_gdi[i].pal_count; k++) { memcpy(g_gdi[i].pal + (start + k) * 4, in + k * 4, 4); set++; }
    return set;
}
uint32_t aret_GetNearestPaletteIndex(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_PALETTE || g_gdi[i].pal_count <= 0 || !g_gdi[i].pal) return 0;
    uint32_t c = WU(1); int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
    int best = 0; long bestd = -1;
    for (int k = 0; k < g_gdi[i].pal_count; k++) {
        const uint8_t *e = g_gdi[i].pal + k * 4;
        long dr = r - e[0], dg = g - e[1], db = b - e[2], d = dr*dr + dg*dg + db*db;
        if (bestd < 0 || d < bestd) { bestd = d; best = k; }
    }
    return (uint32_t)best;
}
uint32_t aret_ResizePalette(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_PALETTE) return 0;
    uint32_t n = WU(1);
    uint8_t *np = (uint8_t *)calloc(n ? n : 1, 4); if (!np) return 0;
    int copy = g_gdi[i].pal_count < (int)n ? g_gdi[i].pal_count : (int)n;
    if (g_gdi[i].pal && copy > 0) memcpy(np, g_gdi[i].pal, (size_t)copy * 4);
    free(g_gdi[i].pal); g_gdi[i].pal = np; g_gdi[i].pal_count = (int)n;
    return 1;
}
uint32_t aret_RealizePalette(uint32_t esp)     { (void)esp; return 0; }  /* truecolor: 0 entries remapped */
uint32_t aret_UnrealizeObject(uint32_t esp)    { (void)esp; return 1; }
uint32_t aret_GetSystemPaletteUse(uint32_t esp){ (void)esp; return 1; }  /* SYSPAL_STATIC */
uint32_t aret_SetSystemPaletteUse(uint32_t esp){ (void)esp; return 1; }  /* prev = SYSPAL_STATIC */
uint32_t aret_GetSystemPaletteEntries(uint32_t esp) {
    uint32_t start = WU(1), count = WU(2); uint8_t *out = (uint8_t *)WP(3);
    if (out) for (uint32_t k = 0; k < count; k++) u32_syspal_entry((int)(start + k), out + k * 4);
    return 0;   /* truecolor DC has no palette -> 0 (Wine); buffer still filled with defaults */
}
uint32_t aret_SelectPalette(uint32_t esp) {
    int d = gdi_idx(WU(0)); if (d < 0 || g_gdi[d].type != GDIT_DC) return 0;
    uint32_t prev = g_gdi[d].sel_palette ? g_gdi[d].sel_palette : u32_default_palette();
    g_gdi[d].sel_palette = WU(1);
    return prev;
}
/* Simple constants / sound no-ops the socle needs. */
uint32_t aret_GetNearestColor(uint32_t esp)   { return WU(1); }         /* 32bpp: identity */
uint32_t aret_GdiGetCodePage(uint32_t esp)    { (void)esp; return 1252; }
uint32_t aret_DragDetect(uint32_t esp)        { (void)esp; return 0; }  /* no drag headless */
uint32_t aret_GetKeyNameTextW(uint32_t esp)   { uint16_t *b = (uint16_t *)WP(1); if (b && WU(2)) b[0] = 0; return 0; }
uint32_t aret_GetTextCharsetInfo(uint32_t esp){ (void)esp; return 0; }  /* ANSI_CHARSET */

/* TranslateCharsetInfo(lpSrc, lpCs, dwFlags) — the charset <-> codepage <-> font
 * signature table (gdi32). Reached through LIFTED mlang, which calls it while
 * describing a code page.
 *
 * The table below is not derived, it is the **exhaustively swept** measurement:
 * all 256 charset values, all 32 `fsCsb[0]` bits, and 46 code pages, each against
 * Wine. That sweep is also what makes embedding a table legitimate here (doc 70
 * §7): the data is version-dependent but **deterministic**, so the fixture covers
 * every cell and a Wine change turns it red instead of rotting in silence.
 *
 * Contract, as measured — the parts that would have been guessed wrong:
 *   - `fsUsb[4]` comes back **all zero**; only `fsCsb[0]` carries the bit.
 *   - For TCI_SRCFONTSIG the source is a **pointer** to fsCsb, not a value, the
 *     **lowest set bit wins** (bits 0+1 -> the bit-0 entry), and `fsCsb[1]` is
 *     ignored entirely (0/0xffffffff in the high word -> FALSE).
 *   - A bit with no entry (9-15, 22-25, 27-30) -> FALSE, and every rejection
 *     leaves the caller's CHARSETINFO **completely untouched** (proven on a
 *     poisoned buffer) and does **not** touch the last error.
 *   - DEFAULT_CHARSET (1) is rejected even though ANSI_CHARSET (0) is accepted.
 * ⚠️ TCI_SRCLOCALE returns FALSE because that is what Wine does (it is a FIXME
 * there). It is a defined FAILURE, never a fabricated success, but it is the one
 * cell of this family that Wine may not settle correctly — queued for the Windows
 * oracle (bench/winoracle), like PathIsUNCServer was. */
static const struct { uint32_t charset, acp; } u32_tci[32] = {
    /* 0 */ { 0, 1252 },   /* ANSI       */ { 238, 1250 }, /* EASTEUROPE */
            { 204, 1251 }, /* RUSSIAN    */ { 161, 1253 }, /* GREEK      */
    /* 4 */ { 162, 1254 }, /* TURKISH    */ { 177, 1255 }, /* HEBREW     */
            { 178, 1256 }, /* ARABIC     */ { 186, 1257 }, /* BALTIC     */
    /* 8 */ { 163, 1258 }, /* VIETNAMESE */ { ~0u, 0 }, { ~0u, 0 }, { ~0u, 0 },
    /*12 */ { ~0u, 0 }, { ~0u, 0 }, { ~0u, 0 }, { ~0u, 0 },
    /*16 */ { 222, 874 },  /* THAI       */ { 128, 932 },  /* SHIFTJIS   */
            { 134, 936 },  /* GB2312     */ { 129, 949 },  /* HANGUL     */
    /*20 */ { 136, 950 },  /* CHINESEBIG5*/ { 130, 1361 }, /* JOHAB      */
            { ~0u, 0 }, { ~0u, 0 },
    /*24 */ { ~0u, 0 }, { ~0u, 0 }, { 254, 65001 }, /* UTF-8 */ { ~0u, 0 },
    /*28 */ { ~0u, 0 }, { ~0u, 0 }, { ~0u, 0 }, { 2, 42 },      /* SYMBOL */
};
uint32_t aret_TranslateCharsetInfo(uint32_t esp) {
    uint32_t src = WU(0), dst = WU(1), flags = WU(2);
    int idx = -1;
    if (flags == 1) {                       /* TCI_SRCCHARSET: src IS the charset */
        for (int i = 0; i < 32; i++)
            if (u32_tci[i].charset != ~0u && u32_tci[i].charset == src) { idx = i; break; }
    } else if (flags == 2) {                /* TCI_SRCCODEPAGE: src IS the code page */
        for (int i = 0; i < 32; i++)
            if (u32_tci[i].charset != ~0u && u32_tci[i].acp == src) { idx = i; break; }
    } else if (flags == 3) {                /* TCI_SRCFONTSIG: src POINTS to fsCsb */
        uint32_t csb = *(const uint32_t *)(uintptr_t)src;
        for (int i = 0; i < 32; i++)
            if ((csb >> i) & 1u) { idx = (u32_tci[i].charset != ~0u) ? i : -1; break; }
    }
    if (idx < 0) return 0;                  /* destination left untouched: measured */
    uint32_t *o = (uint32_t *)(uintptr_t)dst;
    o[0] = u32_tci[idx].charset;
    o[1] = u32_tci[idx].acp;
    o[2] = o[3] = o[4] = o[5] = 0;          /* fsUsb[4] — zero under Wine */
    o[6] = 1u << idx;                       /* fsCsb[0] */
    o[7] = 0;                               /* fsCsb[1] */
    return 1;
}

/* IsValidLocale(Locale, dwFlags): a locale is valid iff its primary language id is in the
 * MS-LCID assigned range [0x01,0x92] (MEASURED vs Wine: 0x09/0x0C/0x50/0x91/0x92 valid,
 * 0xA0/0xFF/0x350 invalid), and the USER/SYSTEM_DEFAULT pseudo-LCIDs (0x0400/0x0800) are
 * rejected as Wine does. Out-of-scope edge (documented): custom locales (0x0C00) and any
 * unassigned holes inside the range — comctl32 never passes those. No longer "always 1". */
uint32_t aret_IsValidLocale(uint32_t esp) {
    uint32_t lcid = WU(0);
    if (lcid == 0x0400u || lcid == 0x0800u) return 0;
    uint32_t primlang = lcid & 0x03FFu;
    return (primlang >= 0x01u && primlang <= 0x92u) ? 1 : 0;
}
/* ShowScrollBar(hwnd, wBar, bShow): toggles the WS_HSCROLL/WS_VSCROLL style bit (MEASURED
 * vs Wine — GetWindowLong(GWL_STYLE) reflects it). SB_HORZ=0/SB_VERT=1/SB_CTL=2/SB_BOTH=3. */
uint32_t aret_ShowScrollBar(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    uint32_t bar = WU(1), show = WU(2);
    if (bar == 0 || bar == 3) { if (show) g_u32_win[i].style |= 0x00100000u; else g_u32_win[i].style &= ~0x00100000u; }
    if (bar == 1 || bar == 3) { if (show) g_u32_win[i].style |= 0x00200000u; else g_u32_win[i].style &= ~0x00200000u; }
    return 1;
}
/* Newly-exposed update rect after a scroll of (dx,dy) within a rect (client if scr==NULL).
 * MEASURED vs Wine: dx>0 exposes the left strip [l,t,l+dx,b], dy<0 the bottom [l,b+dy,r,b],
 * two axes give the L-shape's bounding box. */
static void u32_scroll_update(int i, int dx, int dy, const int32_t *scr, int32_t *upd) {
    int l = scr ? scr[0] : 0, t = scr ? scr[1] : 0, r = scr ? scr[2] : g_u32_win[i].w, b = scr ? scr[3] : g_u32_win[i].h;
    int have = 0, ul = 0, ut = 0, ur = 0, ub = 0;
#define U32_SU_ADD(A,C,D,E) do { int xl=(A),xt=(C),xr=(D),xb=(E); if (xr>xl && xb>xt) { \
        if (!have) { ul=xl; ut=xt; ur=xr; ub=xb; have=1; } \
        else { if(xl<ul)ul=xl; if(xt<ut)ut=xt; if(xr>ur)ur=xr; if(xb>ub)ub=xb; } } } while (0)
    if (dx > 0) U32_SU_ADD(l, t, (l+dx<r?l+dx:r), b);
    else if (dx < 0) U32_SU_ADD((r+dx>l?r+dx:l), t, r, b);
    if (dy > 0) U32_SU_ADD(l, t, r, (t+dy<b?t+dy:b));
    else if (dy < 0) U32_SU_ADD(l, (b+dy>t?b+dy:t), r, b);
#undef U32_SU_ADD
    if (upd) { if (have) { upd[0]=ul; upd[1]=ut; upd[2]=ur; upd[3]=ub; } else { upd[0]=upd[1]=upd[2]=upd[3]=0; } }
}
/* Move the client framebuffer content by (dx,dy) (the real scroll). Only when a client
 * framebuffer exists (visible SDL window); no headless oracle for the pixels -> qualitative
 * like the composite. Overlap-safe via a temp copy. */
static void u32_scroll_pixels(int i, int dx, int dy) {
#ifdef ARET_HAVE_SDL
    int bi = gdi_idx(g_u32_win[i].client_bmp); if (bi < 0) return;
    struct gdi_obj *bm = &g_gdi[bi]; if (!bm->bits || bm->bpp != 32) return;
    size_t n = (size_t)bm->w * bm->h * 4; uint8_t *tmp = (uint8_t *)malloc(n); if (!tmp) return;
    memcpy(tmp, bm->bits, n);
    for (int y = 0; y < bm->h; y++) for (int x = 0; x < bm->w; x++) {
        int sx = x - dx, sy = y - dy; uint8_t *d = gdi_px(bm, x, y);
        if (!d) continue;
        if (sx >= 0 && sx < bm->w && sy >= 0 && sy < bm->h) {
            int row = bm->topdown ? sy : (bm->h - 1 - sy); uint8_t *s = tmp + ((size_t)row * bm->w + sx) * 4;
            d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
        }
    }
    free(tmp);
#else
    (void)i; (void)dx; (void)dy;
#endif
}
/* ScrollWindowEx(hwnd, dx, dy, prcScroll, prcClip, hrgnUpdate, prcUpdate, flags) -> region
 * type. Fills prcUpdate with the exposed strip (bit-exact vs Wine on a client == window
 * child), scrolls the client framebuffer, and invalidates. ScrollWindow is the same minus
 * the out-params. */
uint32_t aret_ScrollWindowEx(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    int dx = WI(1), dy = WI(2);
    u32_scroll_update(i, dx, dy, (const int32_t *)WP(3), (int32_t *)WP(6));
    u32_scroll_pixels(i, dx, dy);
    g_u32_win[i].needs_paint = 1;
    return 2;   /* SIMPLEREGION (rect scroll) */
}
uint32_t aret_ScrollWindow(uint32_t esp) {
    int i = u32_win_idx(WU(0)); if (i < 0) return 0;
    u32_scroll_pixels(i, WI(1), WI(2));
    g_u32_win[i].needs_paint = 1;
    return 1;
}
/* GetClassLong(hwnd, nIndex): the real registered class field (window -> class registry).
 * Class-extra bytes (positive index) are not modelled -> abort sound rather than a fake 0. */
static int u32_win_class_idx(int wi) {
    const char *cn = g_u32_win[wi].classname;
    for (int i = 0; i < U32_MAX_CLASSES; i++) {
        if (!g_u32_class[i].used) continue;
        int k = 0; while (cn[k] && g_u32_class[i].name[k] == (uint16_t)(unsigned char)cn[k]) k++;
        if (cn[k] == 0 && g_u32_class[i].name[k] == 0) return i;
    }
    return -1;
}
uint32_t aret_GetClassLongW(uint32_t esp) {
    int wi = u32_win_idx(WU(0)); if (wi < 0) return 0;
    int ci = u32_win_class_idx(wi); if (ci < 0) return 0;
    int32_t idx = WI(1);
    switch (idx) {
        case -26: return g_u32_class[ci].style;               /* GCL_STYLE */
        case -24: return g_u32_class[ci].wndproc;             /* GCL_WNDPROC */
        case -20: return (uint32_t)g_u32_class[ci].cls_extra; /* GCL_CBCLSEXTRA */
        case -18: return (uint32_t)g_u32_class[ci].wnd_extra; /* GCL_CBWNDEXTRA */
        case -16: return g_u32_class[ci].hinstance;           /* GCL_HMODULE */
        case -14: return g_u32_class[ci].hicon;               /* GCL_HICON */
        case -12: return g_u32_class[ci].hcursor;             /* GCL_HCURSOR */
        case -10: return g_u32_class[ci].hbr_bg;              /* GCL_HBRBACKGROUND */
        case -8:  return g_u32_class[ci].menu_name;           /* GCL_MENUNAME */
        case -34: return g_u32_class[ci].hicon_sm;            /* GCL_HICONSM */
        default:
            if (idx >= 0) aret_partial("GetClassLong: class-extra bytes (positive index) not modelled");
            return 0;
    }
}
uint32_t aret_GetClassLongA(uint32_t esp) { return aret_GetClassLongW(esp); }
/* MapVirtualKey(uCode, uMapType): the US keyboard scan-code table (set-1 make codes),
 * MEASURED vs Wine (A->0x1E, RETURN->0x1C, SHIFT->0x2A, F1->0x3B). */
static const uint8_t u32_vk2vsc[256] = {
    [0x08]=0x0E, [0x09]=0x0F, [0x0D]=0x1C, [0x10]=0x2A, [0x11]=0x1D, [0x12]=0x38, [0x13]=0x45,
    [0x14]=0x3A, [0x1B]=0x01, [0x20]=0x39, [0x21]=0x49, [0x22]=0x51, [0x23]=0x4F, [0x24]=0x47,
    [0x25]=0x4B, [0x26]=0x48, [0x27]=0x4D, [0x28]=0x50, [0x2D]=0x52, [0x2E]=0x53,
    [0x30]=0x0B, [0x31]=0x02, [0x32]=0x03, [0x33]=0x04, [0x34]=0x05, [0x35]=0x06, [0x36]=0x07,
    [0x37]=0x08, [0x38]=0x09, [0x39]=0x0A,
    [0x41]=0x1E, [0x42]=0x30, [0x43]=0x2E, [0x44]=0x20, [0x45]=0x12, [0x46]=0x21, [0x47]=0x22,
    [0x48]=0x23, [0x49]=0x17, [0x4A]=0x24, [0x4B]=0x25, [0x4C]=0x26, [0x4D]=0x32, [0x4E]=0x31,
    [0x4F]=0x18, [0x50]=0x19, [0x51]=0x10, [0x52]=0x13, [0x53]=0x1F, [0x54]=0x14, [0x55]=0x16,
    [0x56]=0x2F, [0x57]=0x11, [0x58]=0x2D, [0x59]=0x15, [0x5A]=0x2C,
    [0x60]=0x52, [0x61]=0x4F, [0x62]=0x50, [0x63]=0x51, [0x64]=0x4B, [0x65]=0x4C, [0x66]=0x4D,
    [0x67]=0x47, [0x68]=0x48, [0x69]=0x49, [0x6A]=0x37, [0x6B]=0x4E, [0x6D]=0x4A, [0x6E]=0x53,
    [0x6F]=0x35,
    [0x70]=0x3B, [0x71]=0x3C, [0x72]=0x3D, [0x73]=0x3E, [0x74]=0x3F, [0x75]=0x40, [0x76]=0x41,
    [0x77]=0x42, [0x78]=0x43, [0x79]=0x44, [0x7A]=0x57, [0x7B]=0x58,
    [0xBA]=0x27, [0xBB]=0x0D, [0xBC]=0x33, [0xBD]=0x0C, [0xBE]=0x34, [0xBF]=0x35, [0xC0]=0x29,
    [0xDB]=0x1A, [0xDC]=0x2B, [0xDD]=0x1B, [0xDE]=0x28,
};
uint32_t aret_MapVirtualKeyW(uint32_t esp) {
    uint32_t code = WU(0) & 0xFFu, type = WU(1);
    if (type == 0) return u32_vk2vsc[code];                       /* MAPVK_VK_TO_VSC */
    if (type == 1 || type == 3) {                                 /* MAPVK_VSC_TO_VK(_EX) */
        if (!code) return 0;
        for (int vk = 0; vk < 256; vk++) if (u32_vk2vsc[vk] == code) return (uint32_t)vk;
        return 0;
    }
    if (type == 2) {                                              /* MAPVK_VK_TO_CHAR */
        if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return code;
        switch (code) { case 0x0D: return 13; case 0x20: return 32; case 0x08: return 8;
                        case 0x09: return 9; case 0x1B: return 27; default: return 0; }
    }
    return 0;
}
uint32_t aret_MapVirtualKeyA(uint32_t esp) { return aret_MapVirtualKeyW(esp); }

/* ---- comctl32 socle batch 5: DC clip regions + window regions + FillRgn/FrameRgn.
 * A DC's user clip is modelled as a rectangular bbox (clip_l..b) plus a complex flag
 * and a "set" flag (no clip = the whole surface is visible, GetClipRgn returns 0). The
 * region-type returns (NULL=1/SIMPLE=2/COMPLEX=3) are measured bit-exact vs Wine for the
 * rectangular cases; a complex clip keeps its bbox (RectVisible is then a conservative
 * "visible" superset, consistent with ARET's blits which don't honour a clip anyway). */
static uint32_t u32_clip_type(int i) {  /* type of the DC's current clip */
    if (!g_gdi[i].clip_set) return 2;   /* no clip = whole surface = SIMPLEREGION-ish; matches SelectClipRgn(NULL) */
    if (g_gdi[i].clip_r <= g_gdi[i].clip_l || g_gdi[i].clip_b <= g_gdi[i].clip_t) return 1;  /* empty = NULL */
    return g_gdi[i].clip_complex ? 3 : 2;
}
uint32_t aret_GetClipRgn(uint32_t esp) {   /* -> 1 if a clip is set (copied into hrgn), 0 if none, -1 err */
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_DC) return -1;
    if (!g_gdi[i].clip_set) return 0;
    int r = gdi_idx(WU(1)); if (r < 0) return 1;   /* NULL hrgn: just report "set" */
    g_gdi[r].rgn_l = g_gdi[i].clip_l; g_gdi[r].rgn_t = g_gdi[i].clip_t;
    g_gdi[r].rgn_r = g_gdi[i].clip_r; g_gdi[r].rgn_b = g_gdi[i].clip_b;
    g_gdi[r].rgn_complex = g_gdi[i].clip_complex;
    return 1;
}
uint32_t aret_IntersectClipRect(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_DC) return 0;
    int l = WI(1), t = WI(2), r = WI(3), b = WI(4);
    if (r < l) { int x = l; l = r; r = x; } if (b < t) { int y = t; t = b; b = y; }
    if (!g_gdi[i].clip_set) { g_gdi[i].clip_l = l; g_gdi[i].clip_t = t; g_gdi[i].clip_r = r; g_gdi[i].clip_b = b; }
    else {
        if (l > g_gdi[i].clip_l) g_gdi[i].clip_l = l; if (t > g_gdi[i].clip_t) g_gdi[i].clip_t = t;
        if (r < g_gdi[i].clip_r) g_gdi[i].clip_r = r; if (b < g_gdi[i].clip_b) g_gdi[i].clip_b = b;
    }
    g_gdi[i].clip_set = 1; g_gdi[i].clip_complex = 0;   /* rect ∩ rect = rect */
    return u32_clip_type(i);
}
uint32_t aret_ExcludeClipRect(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_DC) return 0;
    int l = WI(1), t = WI(2), r = WI(3), b = WI(4);
    if (r < l) { int x = l; l = r; r = x; } if (b < t) { int y = t; t = b; b = y; }
    if (!g_gdi[i].clip_set) { g_gdi[i].clip_set = 1; g_gdi[i].clip_complex = 1; return 3; }  /* surface minus rect = complex */
    int cl = g_gdi[i].clip_l, ct = g_gdi[i].clip_t, cr = g_gdi[i].clip_r, cb = g_gdi[i].clip_b;
    if (r <= cl || l >= cr || b <= ct || t >= cb) return u32_clip_type(i);   /* no overlap: unchanged */
    if (l <= cl && r >= cr && t <= ct && b >= cb) {                          /* fully covered: empty */
        g_gdi[i].clip_r = g_gdi[i].clip_l; g_gdi[i].clip_b = g_gdi[i].clip_t; return 1;
    }
    /* Excise a rect: result is a rectangle only if the cut removes a full edge band. */
    if (l <= cl && r >= cr) {         /* spans full width -> cuts top or bottom band */
        if (t <= ct) { g_gdi[i].clip_t = b > cb ? cb : b; g_gdi[i].clip_complex = 0; }
        else if (b >= cb) { g_gdi[i].clip_b = t < ct ? ct : t; g_gdi[i].clip_complex = 0; }
        else g_gdi[i].clip_complex = 1;
    } else if (t <= ct && b >= cb) {  /* spans full height -> cuts left or right band */
        if (l <= cl) { g_gdi[i].clip_l = r < cr ? cr : r; g_gdi[i].clip_complex = 0; }
        else if (r >= cr) { g_gdi[i].clip_r = l > cl ? cl : l; g_gdi[i].clip_complex = 0; }
        else g_gdi[i].clip_complex = 1;
    } else g_gdi[i].clip_complex = 1;  /* interior/corner cut -> complex, bbox unchanged */
    return u32_clip_type(i);
}
uint32_t aret_SelectClipRgn(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_DC) return 0;
    int r = gdi_idx(WU(1));
    if (r < 0) { g_gdi[i].clip_set = 0; return 2; }   /* SelectClipRgn(NULL) = remove clip -> SIMPLEREGION */
    g_gdi[i].clip_set = 1; g_gdi[i].clip_l = g_gdi[r].rgn_l; g_gdi[i].clip_t = g_gdi[r].rgn_t;
    g_gdi[i].clip_r = g_gdi[r].rgn_r; g_gdi[i].clip_b = g_gdi[r].rgn_b; g_gdi[i].clip_complex = g_gdi[r].rgn_complex;
    return u32_clip_type(i);
}
uint32_t aret_ExtSelectClipRgn(uint32_t esp) {
    int i = gdi_idx(WU(0)); if (i < 0 || g_gdi[i].type != GDIT_DC) return 0;
    int r = gdi_idx(WU(1)); uint32_t mode = WU(2);
    if (r < 0) { if (mode == 5 /*COPY*/) { g_gdi[i].clip_set = 0; return 2; } return u32_clip_type(i); }
    if (!g_gdi[i].clip_set || mode == 5 /*COPY*/) {   /* no clip: AND/COPY with rgn = rgn; OR = rgn */
        g_gdi[i].clip_set = 1; g_gdi[i].clip_l = g_gdi[r].rgn_l; g_gdi[i].clip_t = g_gdi[r].rgn_t;
        g_gdi[i].clip_r = g_gdi[r].rgn_r; g_gdi[i].clip_b = g_gdi[r].rgn_b; g_gdi[i].clip_complex = g_gdi[r].rgn_complex;
        return u32_clip_type(i);
    }
    if (mode == 1 /*AND*/) {
        if (g_gdi[r].rgn_l > g_gdi[i].clip_l) g_gdi[i].clip_l = g_gdi[r].rgn_l;
        if (g_gdi[r].rgn_t > g_gdi[i].clip_t) g_gdi[i].clip_t = g_gdi[r].rgn_t;
        if (g_gdi[r].rgn_r < g_gdi[i].clip_r) g_gdi[i].clip_r = g_gdi[r].rgn_r;
        if (g_gdi[r].rgn_b < g_gdi[i].clip_b) g_gdi[i].clip_b = g_gdi[r].rgn_b;
        g_gdi[i].clip_complex = 0;
    } else {   /* OR/XOR/DIFF: union bbox, generally complex */
        if (g_gdi[r].rgn_l < g_gdi[i].clip_l) g_gdi[i].clip_l = g_gdi[r].rgn_l;
        if (g_gdi[r].rgn_t < g_gdi[i].clip_t) g_gdi[i].clip_t = g_gdi[r].rgn_t;
        if (g_gdi[r].rgn_r > g_gdi[i].clip_r) g_gdi[i].clip_r = g_gdi[r].rgn_r;
        if (g_gdi[r].rgn_b > g_gdi[i].clip_b) g_gdi[i].clip_b = g_gdi[r].rgn_b;
        g_gdi[i].clip_complex = 1;
    }
    return u32_clip_type(i);
}
uint32_t aret_RectVisible(uint32_t esp) {   /* rect intersects the DC clip? no clip = always visible */
    int i = gdi_idx(WU(0)); const int32_t *r = (const int32_t *)WP(1);
    if (i < 0 || g_gdi[i].type != GDIT_DC || !r) return 0;
    if (!g_gdi[i].clip_set) return 1;
    return !(r[2] <= g_gdi[i].clip_l || r[0] >= g_gdi[i].clip_r || r[3] <= g_gdi[i].clip_t || r[1] >= g_gdi[i].clip_b);
}
uint32_t aret_SetWindowRgn(uint32_t esp) {
    int w = u32_win_idx(WU(0)); if (w < 0) return 0;
    int r = gdi_idx(WU(1));
    if (r < 0) { g_u32_win[w].wnd_rgn_set = 0; return 1; }   /* NULL = clear */
    g_u32_win[w].wnd_rgn_set = 1; g_u32_win[w].wnd_rgn_l = g_gdi[r].rgn_l; g_u32_win[w].wnd_rgn_t = g_gdi[r].rgn_t;
    g_u32_win[w].wnd_rgn_r = g_gdi[r].rgn_r; g_u32_win[w].wnd_rgn_b = g_gdi[r].rgn_b; g_u32_win[w].wnd_rgn_complex = g_gdi[r].rgn_complex;
    return 1;
}
uint32_t aret_GetWindowRgn(uint32_t esp) {   /* -> region type, or ERROR(0) if the window has none */
    int w = u32_win_idx(WU(0)); if (w < 0 || !g_u32_win[w].wnd_rgn_set) return 0;
    int r = gdi_idx(WU(1)); if (r < 0) return 0;
    g_gdi[r].rgn_l = g_u32_win[w].wnd_rgn_l; g_gdi[r].rgn_t = g_u32_win[w].wnd_rgn_t;
    g_gdi[r].rgn_r = g_u32_win[w].wnd_rgn_r; g_gdi[r].rgn_b = g_u32_win[w].wnd_rgn_b;
    g_gdi[r].rgn_complex = g_u32_win[w].wnd_rgn_complex;
    return u32_rgn_type(r);
}
uint32_t aret_FillRgn(uint32_t esp) {   /* fill a region with a brush */
    int rg = gdi_idx(WU(1)); if (rg < 0) return 0;
    if (g_gdi[rg].rgn_complex) { aret_partial("FillRgn: complex (non-rect) region not modelled"); return 0; }
    struct gdi_obj *bm = gdi_dc_surface(WU(0)); uint32_t c;
    if (!bm || !gdi_brush_color(WU(2), &c)) return bm ? 1 : 0;   /* null brush: nothing */
    for (int y = g_gdi[rg].rgn_t; y < g_gdi[rg].rgn_b; y++)
        for (int x = g_gdi[rg].rgn_l; x < g_gdi[rg].rgn_r; x++) gdi_put(bm, x, y, c);
    return 1;
}
uint32_t aret_FrameRgn(uint32_t esp) {   /* draw a border of thickness (w,h) around the region */
    int rg = gdi_idx(WU(1)); if (rg < 0) return 0;
    if (g_gdi[rg].rgn_complex) { aret_partial("FrameRgn: complex (non-rect) region not modelled"); return 0; }
    struct gdi_obj *bm = gdi_dc_surface(WU(0)); uint32_t c;
    if (!bm || !gdi_brush_color(WU(2), &c)) return bm ? 1 : 0;
    int fw = WI(3), fh = WI(4), l = g_gdi[rg].rgn_l, t = g_gdi[rg].rgn_t, r = g_gdi[rg].rgn_r, b = g_gdi[rg].rgn_b;
    for (int y = t; y < b; y++) for (int x = l; x < r; x++)
        if (x < l + fw || x >= r - fw || y < t + fh || y >= b - fh) gdi_put(bm, x, y, c);
    return 1;
}
uint32_t aret_ExtCreateRegion(uint32_t esp) {   /* region from RGNDATA (NULL transform only) */
    if (WP(0)) { aret_partial("ExtCreateRegion: XFORM transform not modelled"); return 0; }
    const uint8_t *rd = (const uint8_t *)WP(2); if (!rd) return 0;
    const int32_t *hdr = (const int32_t *)rd;      /* RGNDATAHEADER: dwSize,iType,nCount,nRgnSize,rcBound[4] */
    int32_t ncount = hdr[2];
    const int32_t *bnd = &hdr[4];                  /* rcBound */
    uint32_t h = u32_make_rgn(bnd[0], bnd[1], bnd[2], bnd[3]);
    int gi = gdi_idx(h); if (gi >= 0 && ncount > 1) g_gdi[gi].rgn_complex = 1;   /* >1 rect -> complex bbox */
    return h;
}

/* ---- Atom tables (kernel32/user32) — string<->ATOM interning, refcounted.
 * Two separate per-process tables (Global* vs local Add/Find/Delete/GetAtomName):
 * a local atom is invisible to the global table (measured vs Wine). String atoms
 * are case-INsensitive (ASCII), keep the first-added case, and number from 0xC000
 * up. The ATOM is an OPAQUE handle by spec, so the absolute value is not
 * contractual (Wine pre-seeds a few global atoms, shifting its global base — no
 * correct program depends on it); we start both at 0xC000. Integer atoms
 * (MAKEINTATOM, pointer < 0x10000) pass their 16-bit value through and format as
 * "#N". Refcount: Add ++ an existing name, Delete --, freed at 0. All returns
 * measured vs Wine (winecorpus/win32_atom): Delete returns 0 on success (!),
 * Find-miss sets ERROR_FILE_NOT_FOUND(2), a bad atom sets ERROR_INVALID_HANDLE(6),
 * and GetAtomName returns 0 (not the length) when the buffer is too small. */
#define U32_ATOM_BASE 0xC000u
#define U32_ATOM_MAX  1024
struct u32_atom { char name[256]; uint32_t refs; };
static struct u32_atom g_atom_local[U32_ATOM_MAX];
static struct u32_atom g_atom_global[U32_ATOM_MAX];

static int u32_atom_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}
/* MAKEINTATOM: an integer atom is a pointer whose high word is 0. */
static int u32_atom_is_int(const void *s) { return (uintptr_t)s < 0x10000u; }

static uint32_t u32_atom_add(struct u32_atom *t, const char *name) {
    if (u32_atom_is_int(name)) {
        uint32_t v = (uint32_t)(uintptr_t)name & 0xFFFFu;
        if (v == 0) { g_last_error = 87u /*INVALID_PARAMETER*/; return 0; }
        return v; /* integer atom: value passes through, never interned */
    }
    if (!name[0]) { g_last_error = 87u; return 0; }
    for (int i = 0; i < U32_ATOM_MAX; i++)
        if (t[i].refs && u32_atom_ieq(t[i].name, name)) { t[i].refs++; return U32_ATOM_BASE + (uint32_t)i; }
    for (int i = 0; i < U32_ATOM_MAX; i++)
        if (!t[i].refs) {
            uint32_t k = 0;
            for (; name[k] && k < sizeof t[i].name - 1; k++) t[i].name[k] = name[k];
            t[i].name[k] = 0;
            t[i].refs = 1;
            return U32_ATOM_BASE + (uint32_t)i;
        }
    g_last_error = 8u /*NOT_ENOUGH_MEMORY*/; return 0;
}
static uint32_t u32_atom_find(struct u32_atom *t, const char *name) {
    if (u32_atom_is_int(name)) { return (uint32_t)(uintptr_t)name & 0xFFFFu; }
    if (name[0])
        for (int i = 0; i < U32_ATOM_MAX; i++)
            if (t[i].refs && u32_atom_ieq(t[i].name, name)) return U32_ATOM_BASE + (uint32_t)i;
    g_last_error = 2u /*ERROR_FILE_NOT_FOUND*/; return 0;
}
static uint32_t u32_atom_del(struct u32_atom *t, uint32_t atom) {
    if (atom < U32_ATOM_BASE) return 0; /* integer atom (or 0): no-op success */
    uint32_t i = atom - U32_ATOM_BASE;
    if (i >= U32_ATOM_MAX || !t[i].refs) { g_last_error = 6u /*INVALID_HANDLE*/; return atom; }
    if (--t[i].refs == 0) t[i].name[0] = 0;
    return 0; /* success == 0 (measured) */
}
/* Full narrow name of `atom` into out[256]; returns its length, 0 on a bad atom. */
static uint32_t u32_atom_name_of(struct u32_atom *t, uint32_t atom, char out[256]) {
    if (atom >= 1 && atom < U32_ATOM_BASE)
        return (uint32_t)snprintf(out, 256, "#%u", atom); /* integer atom */
    uint32_t i = atom - U32_ATOM_BASE;
    if (atom < U32_ATOM_BASE || i >= U32_ATOM_MAX || !t[i].refs) {
        g_last_error = 6u; out[0] = 0; return 0;
    }
    uint32_t k = 0;
    for (; t[i].name[k]; k++) out[k] = t[i].name[k];
    out[k] = 0;
    return k;
}
/* Copy the atom name into a narrow buffer: full length if it fits, else fill the
 * buffer (bounded), set ERROR_MORE_DATA, and return the copied count for the
 * LOCAL table but 0 for the GLOBAL table — a measured Wine quirk (`global`). */
static uint32_t u32_atom_name_a(struct u32_atom *t, uint32_t atom, char *buf, uint32_t cch, int global) {
    char tmp[256];
    uint32_t n = u32_atom_name_of(t, atom, tmp);
    if (n == 0) { if (buf && cch) buf[0] = 0; return 0; }
    if (!buf || cch == 0) return 0;
    if (n <= cch - 1) { for (uint32_t k = 0; k <= n; k++) buf[k] = tmp[k]; return n; }
    uint32_t k = 0; for (; k < cch - 1; k++) buf[k] = tmp[k]; buf[k] = 0;
    g_last_error = 234u /*ERROR_MORE_DATA*/; return global ? 0 : k;
}
static uint32_t u32_atom_name_w(struct u32_atom *t, uint32_t atom, uint16_t *buf, uint32_t cch, int global) {
    char tmp[256];
    uint32_t n = u32_atom_name_of(t, atom, tmp);
    if (n == 0) { if (buf && cch) buf[0] = 0; return 0; }
    if (!buf || cch == 0) return 0;
    if (n <= cch - 1) {
        for (uint32_t k = 0; k < n; k++) buf[k] = (uint16_t)(unsigned char)tmp[k];
        buf[n] = 0; return n;
    }
    uint32_t k = 0; for (; k < cch - 1; k++) buf[k] = (uint16_t)(unsigned char)tmp[k]; buf[k] = 0;
    g_last_error = 234u; return global ? 0 : k;
}
/* Read a W atom argument: an integer atom keeps its pointer (checked before deref),
 * a string atom is narrowed to ASCII (exact for the measured range). */
static uint32_t u32_atom_add_w(struct u32_atom *t, const void *w) {
    if (u32_atom_is_int(w)) return u32_atom_add(t, (const char *)w);
    char n[256]; u32_w2n((const uint16_t *)w, n, sizeof n); return u32_atom_add(t, n);
}
static uint32_t u32_atom_find_w(struct u32_atom *t, const void *w) {
    if (u32_atom_is_int(w)) return u32_atom_find(t, (const char *)w);
    char n[256]; u32_w2n((const uint16_t *)w, n, sizeof n); return u32_atom_find(t, n);
}

uint32_t aret_GlobalAddAtomA(uint32_t esp)     { return u32_atom_add(g_atom_global, WS(0)); }
uint32_t aret_AddAtomA(uint32_t esp)           { return u32_atom_add(g_atom_local, WS(0)); }
uint32_t aret_GlobalAddAtomW(uint32_t esp)     { return u32_atom_add_w(g_atom_global, WP(0)); }
uint32_t aret_AddAtomW(uint32_t esp)           { return u32_atom_add_w(g_atom_local, WP(0)); }
uint32_t aret_GlobalFindAtomA(uint32_t esp)    { return u32_atom_find(g_atom_global, WS(0)); }
uint32_t aret_FindAtomA(uint32_t esp)          { return u32_atom_find(g_atom_local, WS(0)); }
uint32_t aret_GlobalFindAtomW(uint32_t esp)    { return u32_atom_find_w(g_atom_global, WP(0)); }
uint32_t aret_FindAtomW(uint32_t esp)          { return u32_atom_find_w(g_atom_local, WP(0)); }
uint32_t aret_GlobalDeleteAtom(uint32_t esp)   { return u32_atom_del(g_atom_global, WU(0)); }
uint32_t aret_DeleteAtom(uint32_t esp)         { return u32_atom_del(g_atom_local, WU(0)); }
uint32_t aret_GlobalGetAtomNameA(uint32_t esp) { return u32_atom_name_a(g_atom_global, WU(0), WS(1), WU(2), 1); }
uint32_t aret_GetAtomNameA(uint32_t esp)       { return u32_atom_name_a(g_atom_local, WU(0), WS(1), WU(2), 0); }
uint32_t aret_GlobalGetAtomNameW(uint32_t esp) { return u32_atom_name_w(g_atom_global, WU(0), (uint16_t *)WP(1), WU(2), 1); }
uint32_t aret_GetAtomNameW(uint32_t esp)       { return u32_atom_name_w(g_atom_local, WU(0), (uint16_t *)WP(1), WU(2), 0); }

/* Pointer validation: the guest runs natively with real pointers, so a non-null
 * pointer is valid (0 = "not bad"), like IsBadReadPtr. */
uint32_t aret_IsBadCodePtr(uint32_t esp)   { return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadStringPtrA(uint32_t esp){ return (uint32_t)(WU(0) == 0); }
uint32_t aret_IsBadStringPtrW(uint32_t esp){ return (uint32_t)(WU(0) == 0); }

/* Input state: headless -> no keys/buttons pressed, cursor at origin. */
uint32_t aret_GetKeyState(uint32_t esp)      { (void)esp; return 0; }
uint32_t aret_GetAsyncKeyState(uint32_t esp) { (void)esp; return 0; }
uint32_t aret_GetMessagePos(uint32_t esp)    { (void)esp; return 0; }
uint32_t aret_GetMessageTime(uint32_t esp)   { (void)esp; return (uint32_t)(mono_ns() / 1000000ull); }
/* GetWindowThreadProcessId(hwnd, *pid) -> tid. Single-process/thread model. */
uint32_t aret_GetWindowThreadProcessId(uint32_t esp) {
    uint32_t *pid = (uint32_t *)WP(1); if (pid) *pid = (uint32_t)getpid();
    return 1;   /* thread id */
}

/* ================================================================== */
/* GetClassName, fonts, IsDialogMessage                               */
/* ================================================================== */
/* GetClassNameA/W(hWnd, lpClassName, nMaxCount) -> chars copied. */
uint32_t aret_GetClassNameA(uint32_t esp) {
    char *buf = (char *)WP(1); uint32_t cch = WU(2);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *c = g_u32_win[i].classname; uint32_t len = (uint32_t)strlen(c);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = c[j];
    buf[n] = 0;
    return n;
}
uint32_t aret_GetClassNameW(uint32_t esp) {
    uint16_t *buf = (uint16_t *)WP(1); uint32_t cch = WU(2);
    if (!buf || cch == 0) return 0;
    int i = u32_win_idx(WU(0));
    if (i < 0) { buf[0] = 0; return 0; }
    const char *c = g_u32_win[i].classname; uint32_t len = (uint32_t)strlen(c);
    uint32_t n = len < cch - 1 ? len : cch - 1;
    for (uint32_t j = 0; j < n; j++) buf[j] = (uint16_t)(unsigned char)c[j];
    buf[n] = 0;
    return n;
}

/* Fonts: opaque GDI font objects (glyph rendering is display work; a program uses
 * the handle as a token — SelectObject/DeleteObject/GetObject-LOGFONT). */
/* Store LOGFONT params onto a freshly-allocated GDIT_FONT and return its handle.
 * Face name is copied (ASCII); wide names are narrowed low-byte (font names are
 * ASCII in practice). These feed the FreeType text raster (G3-text). */
static uint32_t font_make(int height, int weight, int italic, int underline, int strikeout, int quality, const char *face) {
    int i = gdi_alloc(GDIT_FONT);
    if (!i) return 0;
    g_gdi[i].lf_height = height;
    g_gdi[i].lf_weight = weight;
    g_gdi[i].lf_italic = italic;
    g_gdi[i].lf_underline = underline;
    g_gdi[i].lf_strikeout = strikeout;
    g_gdi[i].lf_quality = quality;
    if (face) { size_t n = strnlen(face, sizeof g_gdi[i].lf_face - 1); memcpy(g_gdi[i].lf_face, face, n); g_gdi[i].lf_face[n] = 0; }
    return gdi_handle(i);
}
/* CreateFontA(cHeight,cWidth,cEsc,cOrient,cWeight,bItalic,bUnderline,bStrikeOut,
 *   charset,outp,clip,quality,pitch,pszFace). height/weight/italic/underline/
 *   strikeout/quality/face feed the raster; the rest (escapement/orientation etc.)
 *   are unmodelled → if non-default the raster aborts soundly rather than render wrong. */
uint32_t aret_CreateFontA(uint32_t esp) {
    return font_make(WI(0), WI(4), (int)WU(5), (int)WU(6), (int)WU(7), (int)WU(11), WCS(13));
}
uint32_t aret_CreateFontW(uint32_t esp) {
    const uint16_t *wf = (const uint16_t *)(uintptr_t)WU(13);
    char face[64]; int n = 0;
    if (wf) { for (; n < (int)sizeof face - 1 && wf[n]; n++) face[n] = (char)(wf[n] & 0xFF); }
    face[n] = 0;
    return font_make(WI(0), WI(4), (int)WU(5), (int)WU(6), (int)WU(7), (int)WU(11), wf ? face : NULL);
}
uint32_t aret_CreateFontIndirectA(uint32_t esp) {
    const uint8_t *lf = (const uint8_t *)(uintptr_t)WU(0);
    if (!lf) return font_make(0, 0, 0, 0, 0, 0, NULL);
    int32_t height = (int32_t)(lf[0] | lf[1]<<8 | lf[2]<<16 | (uint32_t)lf[3]<<24);
    int32_t weight = (int32_t)(lf[16] | lf[17]<<8 | lf[18]<<16 | (uint32_t)lf[19]<<24);
    return font_make(height, weight, lf[20], lf[21], lf[22], lf[26], (const char *)(lf + 28));
}
uint32_t aret_CreateFontIndirectW(uint32_t esp) {
    const uint8_t *lf = (const uint8_t *)(uintptr_t)WU(0);
    if (!lf) return font_make(0, 0, 0, 0, 0, 0, NULL);
    int32_t height = (int32_t)(lf[0] | lf[1]<<8 | lf[2]<<16 | (uint32_t)lf[3]<<24);
    int32_t weight = (int32_t)(lf[16] | lf[17]<<8 | lf[18]<<16 | (uint32_t)lf[19]<<24);
    const uint16_t *wf = (const uint16_t *)(lf + 28);
    char face[64]; int n = 0;
    for (; n < (int)sizeof face - 1 && wf[n]; n++) face[n] = (char)(wf[n] & 0xFF);
    face[n] = 0;
    return font_make(height, weight, lf[20], lf[21], lf[22], lf[26], face);
}

/* Compute+store a dialog's base units (du_x,du_y) from its font — the pixels-per-
 * dialog-unit scale. Reproduces Wine's GdiGetCharDimensions EXACTLY (autonomous, no
 * Wine at runtime; the recipe is read from Wine's source, doctrine §1): select the
 * dialog font into a scratch DC, then
 *   du_x = (GetTextExtentPoint(52-letter alphabet).cx / 26 + 1) / 2   [avg char width]
 *   du_y = tmHeight.
 * Font from the template point size like Wine: lfHeight = -MulDiv(pt, LOGPIXELSY=96, 72);
 * point size 0x7FFF ("use shell/system font") or no DS_SETFONT -> DEFAULT_GUI_FONT
 * (MS Shell Dlg, -11). Requires FreeType metrics; without them a mapped dialog aborts
 * soundly (never a guessed scale). These base units drive MapDialogRect and control
 * placement, so they are as bit-exact vs Wine as the font resolves identically (same
 * fontconfig recipe -> same TTF -> same FreeType metrics; the gdi_uifont env caveat). */
static void u32_dlg_base_units(int has_font, int point_size, int weight, int italic, const char *face, int *out_bx, int *out_by, uint32_t *out_font) {
    *out_bx = 0; *out_by = 0; if (out_font) *out_font = 0;
#ifdef ARET_HAVE_FREETYPE
    int height, wt, it; const char *fc;
    if (has_font && point_size != 0x7FFF && face && face[0]) {
        height = -gdi_muldiv(point_size, 96, 72);       /* -MulDiv(pt, LOGPIXELSY, 72) */
        fc = face; wt = weight ? weight : 400; it = italic ? 1 : 0;
    } else {
        height = -11; fc = "MS Shell Dlg"; wt = 400; it = 0;   /* DEFAULT_GUI_FONT */
    }
    uint32_t hf = font_make(height, wt, it, 0, 0, 0, fc);
    int fi = gdi_idx(hf); if (fi < 0) return;
    int d = gdi_alloc(GDIT_DC);
    if (!d) { g_gdi[fi].used = 0; return; }
    u32_dc_defaults(d);
    g_gdi[d].sel_font = hf;
    int asc, desc;
    g_dc_font_quiet = 1;                 /* best-effort: unresolved font -> no units, not abort */
    FT_Face f = u32_dc_font(d, &asc, &desc);
    g_dc_font_quiet = 0;
    if (f) {
        uint32_t alpha[52]; int k = 0;
        for (int c = 'A'; c <= 'Z'; c++) alpha[k++] = (uint32_t)c;
        for (int c = 'a'; c <= 'z'; c++) alpha[k++] = (uint32_t)c;
        int ext = u32_text_width(f, alpha, 52);
        *out_bx = (ext / 26 + 1) / 2;
        *out_by = asc + desc;                           /* tmHeight */
    }
    g_gdi[d].used = 0;
    /* Keep the font alive for the caller (applied to the dialog's controls); free it
     * only if the caller does not want it or metrics failed. */
    if (out_font && f) *out_font = hf; else g_gdi[fi].used = 0;
#else
    (void)has_font; (void)point_size; (void)weight; (void)italic; (void)face;
#endif
}
/* MapDialogRect(hDlg, LPRECT) -> BOOL. Convert a rect from dialog units to pixels
 * with the dialog's stored base units, exactly like Wine (MulDiv, GDI rounding):
 * x by du_x/4, y by du_y/8. No base units (font unresolved) -> sound abort. */
uint32_t aret_MapDialogRect(uint32_t esp) {
    int i = u32_win_idx(WU(0));
    int32_t *r = (int32_t *)(uintptr_t)WU(1);
    if (i < 0 || !r) return 0;
    int bx = g_u32_win[i].du_x, by = g_u32_win[i].du_y;
    if (bx <= 0 || by <= 0) { aret_partial("MapDialogRect: dialog has no base units (font unresolved)"); return 0; }
    r[0] = gdi_muldiv(r[0], bx, 4); r[2] = gdi_muldiv(r[2], bx, 4);
    r[1] = gdi_muldiv(r[1], by, 8); r[3] = gdi_muldiv(r[3], by, 8);
    return 1;
}
/* IsDialogMessageA/W(hDlg, lpMsg) -> BOOL. Headless keyboard navigation has no
 * input to translate, so no message is consumed as a dialog message. */
/* Is `h` `anc` or a descendant of it (parent chain)? */
static int u32_is_descendant(uint32_t anc, uint32_t h) {
    for (int guard = 0; h && guard <= U32_MAX_WIN; guard++) {
        if (h == anc) return 1;
        int i = u32_win_idx(h); if (i < 0) return 0;
        h = g_u32_win[i].parent;
    }
    return 0;
}
/* IsDialogMessage(hDlg, MSG*): dialog keyboard navigation. Handles the navigation keys
 * (Tab/Shift-Tab, arrows within a group, Enter=default command, Esc=IDCANCEL) — moving
 * focus and firing WM_COMMAND like Wine — and returns TRUE for those. Any other message
 * returns FALSE so the caller's loop dispatches it normally (functionally the control
 * still receives it), which keeps us from dropping messages. MSG layout: hwnd, message,
 * wParam, lParam. Shift state is unavailable headless, so Tab goes forward (documented). */
static uint32_t u32_is_dialog_message(uint32_t esp) {
    uint32_t hDlg = WU(0);
    const uint32_t *m = (const uint32_t *)WP(1);
    if (!m || u32_win_idx(hDlg) < 0) return 0;
    uint32_t mhwnd = m[0], msg = m[1], wp = m[2];
    if (mhwnd != hDlg && !u32_is_descendant(hDlg, mhwnd)) return 0;   /* not for this dialog */
    if (msg != 0x0100u /*WM_KEYDOWN*/) return 0;
    uint32_t cur = g_u32_focus;
    if (wp == 0x09u /*VK_TAB*/) {
        uint32_t nx = u32_next_dlg_item(hDlg, cur, 0 /*shift unavailable headless -> forward*/, 1);
        if (nx) u32_set_focus(esp, nx);
        return 1;
    }
    if (wp == 0x25u || wp == 0x26u) {                                 /* VK_LEFT / VK_UP -> prev in group */
        uint32_t nx = u32_next_dlg_item(hDlg, cur, 1, 0); if (nx) u32_set_focus(esp, nx); return 1;
    }
    if (wp == 0x27u || wp == 0x28u) {                                 /* VK_RIGHT / VK_DOWN -> next in group */
        uint32_t nx = u32_next_dlg_item(hDlg, cur, 0, 0); if (nx) u32_set_focus(esp, nx); return 1;
    }
    if (wp == 0x0Du /*VK_RETURN*/) {                                  /* default push button, else IDOK */
        uint32_t id = 1u, bh = 0u;
        for (int k = 0; k < U32_MAX_WIN; k++)
            if (g_u32_win[k].used && g_u32_win[k].parent == hDlg &&
                !strcasecmp(g_u32_win[k].classname, "button") && (g_u32_win[k].style & 0xFu) == 1u) {
                id = (uint32_t)g_u32_win[k].ctrl_id; bh = (uint32_t)(k + 1); break; }
        uint32_t wpc = u32_win_wndproc(hDlg);
        if (wpc) u32_call_wndproc(esp, wpc, hDlg, 0x0111u /*WM_COMMAND*/, id & 0xFFFFu, bh);
        return 1;
    }
    if (wp == 0x1Bu /*VK_ESCAPE*/) {                                  /* IDCANCEL */
        uint32_t wpc = u32_win_wndproc(hDlg);
        if (wpc) u32_call_wndproc(esp, wpc, hDlg, 0x0111u /*WM_COMMAND*/, 2u /*IDCANCEL*/, 0);
        return 1;
    }
    return 0;
}
uint32_t aret_IsDialogMessageA(uint32_t esp) { return u32_is_dialog_message(esp); }
uint32_t aret_IsDialogMessageW(uint32_t esp) { return u32_is_dialog_message(esp); }

/* ================================================================== */
/* Windows hooks — sound stubs                                        */
/* ================================================================== */
/* Windows hooks: the proc is stored per idHook (g_u32_hook). Only WH_CBT is actually
 * delivered — HCBT_CREATEWND at window creation (u32_fire_cbt_createwnd), the hook MFC
 * uses to attach/subclass its CWnd. Other hook types are accepted and stored but not
 * fired (a sound gap, the prior stub behaviour). CallNextHookEx = no chained hook (0),
 * unhook clears the slot. Signature: SetWindowsHookEx(idHook, lpfn, hMod, dwThreadId). */
uint32_t aret_SetWindowsHookExA(uint32_t esp)   { return u32_hook_set(WI(0), WU(1)); }
uint32_t aret_SetWindowsHookExW(uint32_t esp)   { return u32_hook_set(WI(0), WU(1)); }
uint32_t aret_UnhookWindowsHookEx(uint32_t esp) {
    uint32_t h = WU(0);
    if ((h & 0xFFFF0000u) == 0x48480000u) { int idx = (int)(h & 0xFFFFu) - 1;
        if (idx >= 0 && idx < 16) g_u32_hook[idx] = 0; }
    return 1;
}
uint32_t aret_CallNextHookEx(uint32_t esp)      { (void)esp; return 0; }
