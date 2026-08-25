/* Bench N=1..12 patch sizes over 20 random layouts on moderate 128KB.
 * Compile with BENCH_MODE ∈ {PLAIN, PLAIN_TIGHT, ZLIB}:
 *   -D BENCH_MODE=PLAIN or PLAIN_TIGHT or ZLIB (with -DBDIFF_HAVE_ZLIB=1 if ZLIB).
 * Writes CSV: "#N,trial,mode,bytes"
 */
#include "bdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef BENCH_MODE
#define BENCH_MODE PLAIN
#endif

#define PLAIN       0
#define PLAIN_TIGHT 1
#define ZLIB        2

static uint32_t rng_st = 0x12345678u;
static uint8_t rnd(void) {
    rng_st = rng_st * 1664525u + 1013904223u;
    return (uint8_t)(rng_st >> 24);
}
static void fill_moderate(uint8_t *p, size_t n, uint32_t seed) {
    for (size_t i = 0; i < n; ++i) {
        size_t g = i / 256;
        if ((g & 1u) == 0) {
            size_t off = i % 256;
            p[i] = (off < 128) ? 0x00 : 0xFF;
        } else {
            uint32_t x = (uint32_t)i * 2654435761u + seed;
            x ^= x >> 13; x *= 0xAE413251u; x ^= x >> 17;
            p[i] = (uint8_t)x;
        }
    }
}
static void mutate_N_4B_1byte_each(uint8_t *d, size_t sz, int n, uint32_t seed) {
    rng_st = seed;
    size_t taken[4096]; int tn = 0; int i = 0;
    while (i < n && tn < 4096) {
        uint32_t a = (uint32_t)rnd()<<24 | (uint32_t)rnd()<<16 |
                     (uint32_t)rnd()<<8  | (uint32_t)rnd();
        size_t off = (size_t)a % (sz - 4);
        off &= ~(size_t)3u;
        int dup = 0;
        for (int k = 0; k < tn; ++k) if (taken[k] == off) { dup = 1; break; }
        if (dup) continue;
        taken[tn++] = off;
        size_t which_byte = off + (size_t)(rnd() & 3u);
        d[which_byte] ^= 0xFF;
        ++i;
    }
}

static const size_t SZ_128K = 128u * 1024u;
static const int TRIALS = 20;

#define XSTR(x) #x
#define STR(x) XSTR(x)

int main(void) {
    uint8_t *old  = (uint8_t *)malloc(SZ_128K);
    uint8_t *newd = (uint8_t *)malloc(SZ_128K);
    bdiff_opts opts = { 16, SZ_128K, SZ_128K, 0, 1,
#if BENCH_MODE==PLAIN_TIGHT
        1
#else
        0
#endif
    };
    printf("#N,trial,mode,bytes\n");
    for (int N = 1; N <= 12; ++N) {
        for (int t = 0; t < TRIALS; ++t) {
            fill_moderate(old, SZ_128K, (uint32_t)t * 131u + 7u);
            memcpy(newd, old, SZ_128K);
            mutate_N_4B_1byte_each(newd, SZ_128K, N, (uint32_t)t * 211u + (uint32_t)N * 17u);
            void *patch = NULL; size_t pl = 0;
            int r = bdiff_diff(old, SZ_128K, newd, SZ_128K, &opts, &patch, &pl);
            if (r != BDIFF_OK) { pl = (size_t)-1; }
            if (pl != (size_t)-1) {
                void *res = NULL; size_t rl = 0;
                int pr = bdiff_patch_opts(old, SZ_128K, patch, pl, &opts, &res, &rl);
                if (pr != BDIFF_OK || rl != SZ_128K || memcmp(res, newd, SZ_128K) != 0) {
                    fprintf(stderr, "RT FAIL: N=%d trial=%d\n", N, t);
                    pl = (size_t)-1;
                }
                free(res);
            }
            free(patch);
            printf("%d,%d,%s,%zu\n", N, t, STR(BENCH_MODE), pl);
        }
    }
    free(old); free(newd);
    return 0;
}
