/* Regression for Phase 2 — pruning by reachability (`--function`). The binary
   has two independent features driven by `main`:

     feature_a -> {helper_add, helper_mul}   (its transitive closure)
     feature_b -> {helper_sub}

   Transpiling with `--function feature_a` must emit ONLY feature_a's closure
   (feature_a + helper_add + helper_mul), drive it from `main`, and print
   "FEATURE_A: 42" — feature_b, helper_sub and the original `main` are pruned.
   The helpers are `noinline` so they survive as distinct recovered functions.

   Build:
     i686-w64-mingw32-gcc -m32 -nostdlib -O1 -fno-builtin \
       -fno-asynchronous-unwind-tables -fno-stack-protector \
       -e _mainCRTStartup@0 prune_closure.c -o prune_closure.exe \
       -lmsvcrt -lkernel32 */
#include <windows.h>
__declspec(dllimport) int printf(const char *, ...);

/* mingw emits a `__main` call at the top of `main`; with -nostdlib provide a
   no-op so the freestanding binary links. */
void __main(void) {}

/* --- feature_a and its leaf callees (its closure) --- */
__attribute__((noinline)) int helper_add(int a, int b) { return a + b; }
__attribute__((noinline)) int helper_mul(int a, int b) { return a * b; }

__attribute__((noinline)) int feature_a(void) {
    int s = helper_add(20, 22);   /* 42 */
    int p = helper_mul(s, 1);
    printf("FEATURE_A: %d\n", p);
    return p;
}

/* --- feature_b: reached only from main, must be pruned away --- */
__attribute__((noinline)) int helper_sub(int a, int b) { return a - b; }

__attribute__((noinline)) int feature_b(void) {
    int d = helper_sub(100, 58);  /* 42 */
    printf("FEATURE_B: %d\n", d);
    return d;
}

int main(void) {
    feature_a();
    feature_b();
    ExitProcess(0);
    return 0;
}

void __stdcall mainCRTStartup(void) {
    main();
    ExitProcess(0);
}
