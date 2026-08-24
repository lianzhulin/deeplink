#include "bdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-tests for the fixed-length-block patch algorithm.
 * Build: make test   (or: gcc -O2 -Wall -Wextra -std=c99 -Werror
 *                          bdiff.c test_bdiff.c -o bdiff_test)
 * Exit code 0 = all passed, 1 = some failed.
 */

static int g_fail = 0, g_pass = 0;

static void check(int cond, const char *desc) {
    if (cond) { g_pass++; }
    else { printf("  FAIL: %s\n", desc); g_fail++; }
}

/* deterministic LCG so tests are reproducible */
static uint32_t rng_st = 0x12345678u;
static uint8_t rnd(void) {
    rng_st = rng_st * 1664525u + 1013904223u;
    return (uint8_t)(rng_st >> 24);
}
static void fill_rnd(uint8_t *p, size_t n, uint32_t seed) {
    rng_st = seed;
    for (size_t i = 0; i < n; ++i) p[i] = rnd();
}

#define S(x) ((size_t)(x))

/* diff old->new, patch back, verify equality. returns 1 on success.
 * On success also reports patch length via *plen (if non-NULL). */
static int roundtrip(const uint8_t *old, size_t os,
                     const uint8_t *newd, size_t ns,
                     const bdiff_opts *opts, size_t *plen) {
    void *patch = NULL; size_t pl = 0;
    int r = bdiff_diff(old, os, newd, ns, opts, &patch, &pl);
    if (r != BDIFF_OK) { free(patch); return 0; }

    void *res = NULL; size_t rl = 0;
    r = bdiff_patch(old, os, patch, pl, &res, &rl);
    if (r != BDIFF_OK) { free(patch); free(res); return 0; }

    int ok = (rl == ns);
    if (ok && ns) ok = (memcmp(res, newd, ns) == 0);
    if (plen) *plen = pl;
    free(patch); free(res);
    return ok;
}

int main(void) {
    static uint8_t old[4096], newd[4096];
    size_t pl;

    /* T1: identical data -> patch tiny (mostly one COPY) */
    fill_rnd(old, 4096, 1);
    memcpy(newd, old, 4096);
    check(roundtrip(old, 4096, newd, 4096, NULL, &pl), "T1 identical roundtrip");
    check(pl < S(4096), "T1 identical patch smaller than data");
    printf("T1 identical:            patch=%zu bytes (data=4096)\n", pl);

    /* T2: single byte change at a block boundary */
    memcpy(newd, old, 4096); newd[2048] ^= 0xFF;
    check(roundtrip(old, 4096, newd, 4096, NULL, &pl), "T2 1-byte change roundtrip");
    check(pl < S(200), "T2 1-byte change patch minimal (<200)");
    printf("T2 1-byte change:        patch=%zu bytes\n", pl);

    /* T3: empty old -> everything must be ADD */
    fill_rnd(newd, 4096, 2);
    check(roundtrip(NULL, 0, newd, 4096, NULL, &pl), "T3 empty-old roundtrip");
    check(pl >= S(4096 + 5) && pl < S(4096 + 20), "T3 all-ADD patch ~ data+header");
    printf("T3 empty old:            patch=%zu bytes\n", pl);

    /* T4: empty new -> header only */
    check(roundtrip(old, 4096, NULL, 0, NULL, &pl), "T4 empty-new roundtrip");
    check(pl <= S(8), "T4 empty-new patch ~ header only");
    printf("T4 empty new:            patch=%zu bytes\n", pl);

    /* T5: ~1% scattered modifications */
    memcpy(newd, old, 4096);
    for (int i = 0; i < 40; ++i) {
        size_t pos = ((size_t)rnd() | ((size_t)rnd() << 8)) % 4096;
        newd[pos] ^= 0xAA;
    }
    check(roundtrip(old, 4096, newd, 4096, NULL, &pl), "T5 scattered mods roundtrip");
    check(pl < S(4096 / 2), "T5 scattered mods patch < half data");
    printf("T5 scattered mods:      patch=%zu bytes\n", pl);

    /* T6: corrupt magic rejected */
    {
        uint8_t bad[6] = { 'X', 'X', 'X', 'X', 1, 0 };
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch(old, 4096, bad, 6, &res, &rl);
        check(r == BDIFF_E_FORMAT, "T6 bad magic rejected");
        free(res);
    }

    /* T7: patch applied to a too-small old buffer must fail safely */
    {
        void *patch = NULL; size_t pl2 = 0;
        memcpy(newd, old, 4096); newd[2048] ^= 0xFF;
        int dr = bdiff_diff(old, 4096, newd, 4096, NULL, &patch, &pl2);
        check(dr == BDIFF_OK, "T7 diff prepared");
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch(old, 100, patch, pl2, &res, &rl); /* old too small */
        check(r != BDIFF_OK, "T7 wrong-old-size rejected");
        free(res); free(patch);
    }

    /* T8: shifted insertion (X + repeated block) -> COPY + small ADD */
    {
        static uint8_t o2[256], n2[257];
        memset(o2, 'A', 256);
        n2[0] = 'X';
        memcpy(n2 + 1, o2, 256);
        check(roundtrip(o2, 256, n2, 257, NULL, &pl), "T8 shifted insertion roundtrip");
        check(pl < S(30), "T8 shifted insertion patch minimal (<30)");
        printf("T8 shifted insertion:    patch=%zu bytes\n", pl);
    }

    /* T9: two separate edits -> two COPY regions */
    memcpy(newd, old, 4096); newd[100] ^= 0xFF; newd[3000] ^= 0xFF;
    check(roundtrip(old, 4096, newd, 4096, NULL, &pl), "T9 two-region roundtrip");
    check(pl < S(300), "T9 two-region patch minimal (<300)");
    printf("T9 two regions:          patch=%zu bytes\n", pl);

    /* T10: custom block size = 64 */
    {
        bdiff_opts opts = { 64 };
        memcpy(newd, old, 4096); newd[2048] ^= 0xFF;
        check(roundtrip(old, 4096, newd, 4096, &opts, &pl), "T10 block=64 roundtrip");
        check(pl < S(200), "T10 block=64 patch minimal (<200)");
        printf("T10 block=64:            patch=%zu bytes\n", pl);
    }

    /* T11: single byte change at a non-boundary offset */
    memcpy(newd, old, 4096); newd[2050] ^= 0xFF;
    check(roundtrip(old, 4096, newd, 4096, NULL, &pl), "T11 non-boundary change roundtrip");
    printf("T11 non-boundary change: patch=%zu bytes\n", pl);

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
