/* CPUID feature-dispatch fallback — regression guard for the clamp in __ix_cpuid
 * (src/emit/mod.rs). ARET advertises, via cpuid, only the instruction families the
 * lifter models COMPLETELY; a feature that is advertised but unmodelled is a §0 trap
 * (the program dispatches into it and hits an unmodelled instruction -> abort). Here
 * the BMI2 family (bzhi/pdep/pext) is exercised through function-multiversioning: the
 * `target("bmi2")` variants exist in the binary (gcc emits the real bzhi/pdep/pext) but
 * are called ONLY when __builtin_cpu_supports("bmi2") is true — which reads the gcc CPU
 * model populated by a cpuid constructor. Under ARET the clamp makes that false, so the
 * SCALAR fallback runs (never executing an unmodelled instruction); under Wine (the
 * oracle, on the real host) the BMI2 path runs. Both compute the SAME values, so the
 * output is byte-identical — UNLESS the clamp regresses to advertise BMI2, in which case
 * ARET would run bzhi/pdep/pext and abort. Deterministic, no OS-API, no DLL. */
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>

/* --- scalar equivalents (exact) --- */
static uint32_t bzhi_scalar(uint32_t x, uint32_t n) { return n >= 32 ? x : (x & ((1u << n) - 1)); }
static uint32_t pext_scalar(uint32_t x, uint32_t m) {
    uint32_t r = 0, k = 0;
    for (uint32_t b = 0; b < 32; b++) if (m & (1u << b)) { if (x & (1u << b)) r |= (1u << k); k++; }
    return r;
}
static uint32_t pdep_scalar(uint32_t x, uint32_t m) {
    uint32_t r = 0, k = 0;
    for (uint32_t b = 0; b < 32; b++) if (m & (1u << b)) { if (x & (1u << k)) r |= (1u << b); k++; }
    return r;
}

/* --- BMI2 variants: compiled with the bmi2 target so gcc emits the real instructions,
 *     but reached only behind __builtin_cpu_supports("bmi2"). --- */
__attribute__((target("bmi2"))) static uint32_t bzhi_hw(uint32_t x, uint32_t n) { return _bzhi_u32(x, n); }
__attribute__((target("bmi2"))) static uint32_t pext_hw(uint32_t x, uint32_t m) { return _pext_u32(x, m); }
__attribute__((target("bmi2"))) static uint32_t pdep_hw(uint32_t x, uint32_t m) { return _pdep_u32(x, m); }

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    int bmi2 = __builtin_cpu_supports("bmi2");
    printf("start\n");
    /* NB: the dispatch DECISION legitimately differs (ARET clamps BMI2, the host has it),
     * so it is NOT printed — only the numeric result, which is identical on both paths.
     * The guard is the abort: if the clamp regressed, ARET would run bzhi/pdep/pext here. */
    uint32_t acc = 0x12345678u;
    for (uint32_t i = 1; i <= 24; i++) {
        uint32_t n = (i * 7u) & 31u;
        uint32_t m = 0x0F0F0F0Fu ^ (i * 0x01010101u);
        uint32_t bz = bmi2 ? bzhi_hw(acc, n) : bzhi_scalar(acc, n);
        uint32_t pe = bmi2 ? pext_hw(acc, m) : pext_scalar(acc, m);
        uint32_t pd = bmi2 ? pdep_hw(acc, m) : pdep_scalar(acc, m);
        acc = (acc * 1103515245u + 12345u) ^ (bz + (pe << 1) + (pd << 2));
    }
    printf("acc=%08x\n", acc);
    printf("done\n");
    return 0;
}
