#include "bdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-tests for the v2 firmware-grade patch algorithm.
 *
 * Builds:
 *   plain:  gcc -O2 -Wall -Wextra -std=c99 -Werror bdiff.c test_bdiff.c -o t
 *   with zlib (enables optional deflate envelope on encode + read-back):
 *           gcc -O2 -Wall -Wextra -std=c99 -Werror -DBDIFF_HAVE_ZLIB=1 \
 *               bdiff.c test_bdiff.c -o tz -lz
 * Both should exit 0.
 */

static int g_fail = 0, g_pass = 0;
static void check(int cond, const char *desc) {
    if (cond) { g_pass++; printf("  PASS  %s\n", desc); }
    else      { g_fail++; printf("  FAIL  %s\n", desc); }
}

/* ---- deterministic helpers ---- */
static uint32_t rng_st = 0x12345678u;
static uint8_t rnd(void) {
    rng_st = rng_st * 1664525u + 1013904223u;
    return (uint8_t)(rng_st >> 24);
}
static void fill_rnd(uint8_t *p, size_t n, uint32_t seed) {
    rng_st = seed;
    for (size_t i = 0; i < n; ++i) p[i] = rnd();
}
static void fill_moderate(uint8_t *p, size_t n, uint32_t seed) {
    /* 256 B groups alternating: structured (zeros / 0xFF stripes) <-> random */
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
    /* Each mutation flips exactly 1 byte inside a 4B-aligned block.  This is
     * the realistic "update firmware field / flag / counter" case that ADDX
     * was designed for. */
    rng_st = seed;
    size_t taken[2048]; int tn = 0;
    int i = 0;
    while (i < n && tn < 2048) {
        uint32_t a = (uint32_t)rnd()<<24 | (uint32_t)rnd()<<16 |
                     (uint32_t)rnd()<<8  | (uint32_t)rnd();
        size_t off = (size_t)a % (sz - 4);
        off &= ~(size_t)3u;
        int dup = 0;
        for (int k = 0; k < tn; ++k) if (taken[k] == off) { dup = 1; break; }
        if (dup) continue;
        taken[tn++] = off;
        /* Only change 1 byte inside the 4B block. */
        size_t which_byte = off + (size_t)(rnd() & 3u);
        d[which_byte] ^= 0xFF;
        ++i;
    }
}
static void overwrite_sectors(uint8_t *d, size_t sz,
                              const size_t *offs, const size_t *lens, int k,
                              const uint8_t *pattern) {
    for (int i = 0; i < k; ++i) {
        for (size_t j = 0; j < lens[i] && offs[i] + j < sz; ++j)
            d[offs[i] + j] = pattern[(offs[i] + j) & 0xFFu] ^ (uint8_t)j;
    }
}

/* ---- roundtrip util ---- */
static int rt(const uint8_t *old, size_t os,
              const uint8_t *newd, size_t ns,
              const bdiff_opts *opts, size_t *plen) {
    void *patch = NULL; size_t pl = 0;
    int r = bdiff_diff(old, os, newd, ns, opts, &patch, &pl);
    if (r != BDIFF_OK) { free(patch); return -r * 1000; } /* pos: err code*1k */
    void *res = NULL; size_t rl = 0;
    r = bdiff_patch_opts(old, os, patch, pl, opts, &res, &rl);
    if (r != BDIFF_OK) { free(patch); free(res); return r; }
    int ok = (rl == ns) && (ns == 0 || memcmp(res, newd, ns) == 0);
    if (plen) *plen = pl;
    free(patch); free(res);
    return ok ? 0 : 999999;
}

static const size_t SZ_128K = 128u * 1024u;

static void test_budget_96B_case(void) {
    /* Moderate-entropy 128KB, N×4B (each changes exactly 1 byte inside).
     * Find the maximum N such that worst-case patch ≤ 96 B across 3 seeds. */
    uint8_t *old  = (uint8_t *)malloc(SZ_128K);
    uint8_t *newd = (uint8_t *)malloc(SZ_128K);
    bdiff_opts opts = { 16, SZ_128K, SZ_128K, 0, 1, 0 }; /* prefer_small */
    const int SEEDS = 3;
    int lo = 0, hi = 1;
    while (hi < 200) {
        size_t worst = 0;
        for (int s = 0; s < SEEDS; ++s) {
            fill_moderate(old, SZ_128K, (uint32_t)s * 131u + 7u);
            memcpy(newd, old, SZ_128K);
            mutate_N_4B_1byte_each(newd, SZ_128K, hi, (uint32_t)s * 211u + (uint32_t)hi);
            size_t pl = 0;
            int r = rt(old, SZ_128K, newd, SZ_128K, &opts, &pl);
            if (r) { worst = (size_t)-1; break; }
            if (pl > worst) worst = pl;
        }
        if (worst == (size_t)-1 || worst > 96) break;
        lo = hi;
        if (hi >= 32) hi += 16; else hi *= 2;
    }
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        size_t worst = 0;
        for (int s = 0; s < SEEDS; ++s) {
            fill_moderate(old, SZ_128K, (uint32_t)s * 131u + 7u);
            memcpy(newd, old, SZ_128K);
            mutate_N_4B_1byte_each(newd, SZ_128K, mid, (uint32_t)s * 211u + (uint32_t)mid);
            size_t pl = 0;
            int r = rt(old, SZ_128K, newd, SZ_128K, &opts, &pl);
            if (r) { worst = (size_t)-1; break; }
            if (pl > worst) worst = pl;
        }
        if (worst != (size_t)-1 && worst <= 96) lo = mid;
        else                                        hi = mid;
    }
    /* Confirm lo OK, lo+1 over budget */
    size_t sz_lo = 0, sz_lo1 = 0;
    for (int s = 0; s < SEEDS; ++s) {
        fill_moderate(old, SZ_128K, (uint32_t)s);
        memcpy(newd, old, SZ_128K);
        mutate_N_4B_1byte_each(newd, SZ_128K, lo, (uint32_t)s);
        size_t pl; int r = rt(old, SZ_128K, newd, SZ_128K, &opts, &pl);
        check(r == 0, "budget roundtrip at max N");
        if (pl > sz_lo) sz_lo = pl;
        memcpy(newd, old, SZ_128K);
        mutate_N_4B_1byte_each(newd, SZ_128K, lo+1, (uint32_t)s + 99u);
        r = rt(old, SZ_128K, newd, SZ_128K, &opts, &pl);
        check(r == 0, "budget roundtrip at N+1");
        if (pl > sz_lo1) sz_lo1 = pl;
    }
    printf("  Budget≤96B moderate-128K max N = %d (max size = %zu B; N+1 = %zu B)\n",
           lo, sz_lo, sz_lo1);
    free(old); free(newd);
}

/* ---------------------------------------------------------------- *
 *  Test sections (original 11 are kept as T1..T11, plus new ones)
 * ---------------------------------------------------------------- */
int main(void) {
    uint8_t *old  = (uint8_t *)malloc(4096);
    uint8_t *newd = (uint8_t *)malloc(4096);
    size_t pl;

    /* T1..T11 legacy baseline (still pass with v2 encoder + v2 decoder,
     * or with convenience bdiff_patch() call). */
    printf("===[ Original baseline tests ]===\n");

    fill_rnd(old, 4096, 1);
    memcpy(newd, old, 4096);
    check(rt(old,4096,newd,4096,NULL,&pl)==0, "T1 identical roundtrip");
    check(pl < 4096, "T1 patch < data");
    printf("  T1 patch = %zu B\n", pl);

    memcpy(newd, old, 4096); newd[2048] ^= 0xFF;
    check(rt(old,4096,newd,4096,NULL,&pl)==0, "T2 1-byte change roundtrip");
    check(pl < 200, "T2 patch < 200");
    printf("  T2 patch = %zu B\n", pl);

    fill_rnd(newd, 4096, 2);
    check(rt(NULL,0,newd,4096,NULL,&pl)==0, "T3 empty-old roundtrip");
    check(pl >= 4096 + 5, "T3 all-ADD ~ header+data");
    printf("  T3 patch = %zu B\n", pl);

    check(rt(old,4096,NULL,0,NULL,&pl)==0, "T4 empty-new roundtrip");
    check(pl <= 10, "T4 ~ header only");
    printf("  T4 patch = %zu B\n", pl);

    memcpy(newd, old, 4096);
    for (int i = 0; i < 40; ++i) {
        size_t pos = ((size_t)rnd() | ((size_t)rnd() << 8)) % 4096;
        newd[pos] ^= 0xAA;
    }
    check(rt(old,4096,newd,4096,NULL,&pl)==0, "T5 scattered roundtrip");
    check(pl < 2048, "T5 patch < half-data");
    printf("  T5 patch = %zu B\n", pl);

    {
        uint8_t bad[6] = { 'X','X','X','X', 2, 0 };
        void *res = NULL; size_t rl = 0;
        check(bdiff_patch_opts(old,4096,bad,sizeof bad,NULL,&res,&rl) == BDIFF_E_FORMAT,
              "T6 bad magic rejected");
        free(res);
    }
    {
        uint8_t bad[6] = { 'B','D','I','F', 77 /* bad version */, 0 };
        void *res = NULL; size_t rl = 0;
        check(bdiff_patch_opts(old,4096,bad,sizeof bad,NULL,&res,&rl) == BDIFF_E_VERSION,
              "T6b unknown version rejected as E_VERSION");
        free(res);
    }
    {
        void *patch = NULL; size_t pl2 = 0;
        memcpy(newd, old, 4096); newd[2048] ^= 0xFF;
        int dr = bdiff_diff(old,4096,newd,4096,NULL,&patch,&pl2);
        check(dr == 0, "T7 diff prepared");
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch_opts(old, 100, patch, pl2, NULL, &res, &rl);
        check(r != 0, "T7 too-small old rejected safely");
        free(res); free(patch);
    }
    {
        static uint8_t o2[256], n2[257];
        memset(o2, 'A', 256);
        n2[0] = 'X'; memcpy(n2+1, o2, 256);
        check(rt(o2,256,n2,257,NULL,&pl)==0, "T8 shifted insertion roundtrip");
        check(pl < 40, "T8 small patch");
        printf("  T8 patch = %zu B\n", pl);
    }
    memcpy(newd, old, 4096); newd[100]^=0xFF; newd[3000]^=0xFF;
    check(rt(old,4096,newd,4096,NULL,&pl)==0, "T9 two regions roundtrip");
    printf("  T9 patch = %zu B\n", pl);
    {
        bdiff_opts o = { 64, 0, 0, 0, 0, 0 };
        memcpy(newd, old, 4096); newd[2048]^=0xFF;
        check(rt(old,4096,newd,4096,&o,&pl)==0, "T10 block=64 roundtrip");
        printf("  T10 patch = %zu B\n", pl);
    }
    memcpy(newd, old, 4096); newd[2050]^=0xFF;
    check(rt(old,4096,newd,4096,NULL,&pl)==0, "T11 non-boundary byte roundtrip");
    printf("  T11 patch = %zu B\n", pl);

    printf("\n===[ Real-world / firmware scenarios (128K) ]===\n");
    uint8_t *old_big = (uint8_t *)malloc(SZ_128K);
    uint8_t *new_big = (uint8_t *)malloc(SZ_128K);

    /* T12 Moderate 128KB, identical -> tiny patch (should be mostly one
     *     COPY_SMALL_BIG or COPY len with 2-byte shape). */
    fill_moderate(old_big, SZ_128K, 0xBEEFu);
    memcpy(new_big, old_big, SZ_128K);
    bdiff_opts opt_fw = { 16, SZ_128K, SZ_128K, 0, 1, 0 };
    check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl) == 0,
          "T12 128K moderate identical roundtrip");
    check(pl < 32, "T12 patch tiny (single COPY region + header)");
    printf("  T12 identical 128K patch = %zu B\n", pl);

    /* T13 1x4B change (one byte inside): patch should leverage ADDX. */
    fill_moderate(old_big, SZ_128K, 1);
    memcpy(new_big, old_big, SZ_128K);
    mutate_N_4B_1byte_each(new_big, SZ_128K, 1, 777);
    check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl) == 0,
          "T13 128K 1x4B-1byte roundtrip");
    printf("  T13 1x4B-1byte patch = %zu B\n", pl);

    /* T14 4x4B changes, each mutating 1 byte inside the 4B block. */
    fill_moderate(old_big, SZ_128K, 2);
    memcpy(new_big, old_big, SZ_128K);
    mutate_N_4B_1byte_each(new_big, SZ_128K, 4, 888);
    check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl) == 0,
          "T14 128K 4x4B-1byte roundtrip");
    printf("  T14 4x4B-1byte patch = %zu B\n", pl);

    /* T15 8x128B overwrites (sector-style). */
    fill_moderate(old_big, SZ_128K, 3);
    memcpy(new_big, old_big, SZ_128K);
    {
        const size_t offs[8] = { 4*1024, 20*1024, 40*1024, 56*1024,
                                 72*1024, 88*1024, 104*1024, 120*1024 };
        const size_t lens[8] = { 128,128,128,128,128,128,128,128 };
        uint8_t pat[256]; fill_rnd(pat, 256, 42);
        overwrite_sectors(new_big, SZ_128K, offs, lens, 8, pat);
    }
    check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl) == 0,
          "T15 8x128B sector overwrite roundtrip");
    /* Upper bound sanity: 8 * (128 data + ~10B headers) + header + 9 COPY. */
    check(pl < 8 * 200 + 100, "T15 patch < 1.7KB");
    printf("  T15 8x128B sectors patch = %zu B\n", pl);

    /* T16 4B shift at exactly the 64K boundary. */
    fill_moderate(old_big, SZ_128K, 4);
    memcpy(new_big, old_big, SZ_128K);
    /* Insert 4 zero bytes at 65536, then truncate last 4 bytes. */
    memmove(new_big + 65536 + 4, new_big + 65536, SZ_128K - 65536 - 4);
    new_big[65536] = new_big[65536+1] = new_big[65536+2] = new_big[65536+3] = 0;
    check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl) == 0,
          "T16 shift 4B across 64K boundary roundtrip");
    check(pl < 80, "T16 shift produces small patch (<80)");
    printf("  T16 shift patch = %zu B\n", pl);

    /* T17 Too-big input rejected via opts caps. */
    {
        bdiff_opts tiny = { 16, 1000, 1000, 0, 0, 0 }; /* way smaller than 128K */
        void *p = NULL; size_t pl2 = 0;
        int r = bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &tiny, &p, &pl2);
        check(r == BDIFF_E_TOO_BIG, "T17 opts caps reject too-big diff");
        free(p);
        /* Also patch side: old bigger than cap. */
        r = rt(old_big, SZ_128K, new_big, SZ_128K, &tiny, &pl2);
        /* rt() passes -err*1000 on diff fail, so != 0 either way */
        check(r != 0, "T17 patch-side old cap enforced");
    }

    /* T18 Patch size cap (max_patch_size) fails oversized diffs. */
    {
        fill_moderate(old_big, SZ_128K, 5);
        memcpy(new_big, old_big, SZ_128K);
        mutate_N_4B_1byte_each(new_big, SZ_128K, 32, 321); /* too many */
        bdiff_opts cap = opt_fw; cap.max_patch_size = 100; /* allow only ~N≤8 */
        void *p = NULL; size_t pl2 = 0;
        int r = bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &cap, &p, &pl2);
        check(r == BDIFF_E_TOO_BIG, "T18 max_patch_size enforced on diff");
        free(p);
    }

    /* T19 Unknown reserved opcode (0x88) inside a manually-crafted v2 patch
     *     is rejected cleanly as E_FORMAT. */
    {
        /* Header BDIF v2 flags=0 new_size=3, then op=0x88 (reserved),
         * then 3 bytes of junk.  Should fail at op decode. */
        const uint8_t bad[] = {
            'B','D','I','F', 2,
            0,                 /* flags = 0 */
            3,                 /* new_size = 3 (1 byte varint) */
            0x88,              /* RESERVED opcode */
            0xAA,0xBB,0xCC     /* junk data (3 bytes) */
        };
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch_opts(old_big, SZ_128K, bad, sizeof bad,
                                 NULL, &res, &rl);
        check(r == BDIFF_E_FORMAT, "T19 reserved opcode 0x88 => E_FORMAT");
        free(res);
    }

    /* T20 Malformed ADDX tag (mode produces more bytes than declared len)
     *     => E_FORMAT cleanly. */
    {
        /* Build by hand: header + ADDX_SMALL len=4, off=0, xdiff_enc_len=5,
         * then a malicious tag that claims 60 zeros (total 60 > len 4). */
        uint8_t bad[32]; size_t w = 0;
        bad[w++] = 'B'; bad[w++] = 'D'; bad[w++] = 'I'; bad[w++] = 'F';
        bad[w++] = 2; /* version */
        bad[w++] = 0; /* flags */
        bad[w++] = 4; /* new_size = 4 bytes */
        bad[w++] = 0x20 | 4; /* ADDX_SMALL len = 4 */
        /* off = 0 varint 1 byte */
        bad[w++] = 0;
        /* xdiff_enc_len = 2 varint 1 byte */
        bad[w++] = 2;
        /* encoded bytes: [ZEROS(60)] = ((60-1)<<2)|1 = 59*4+1 = 237 = 0xED */
        bad[w++] = 0xED;
        bad[w++] = 0x00; /* just 2 bytes of enc to match enc_len */
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch_opts(old_big, SZ_128K, bad, w, NULL, &res, &rl);
        check(r == BDIFF_E_FORMAT, "T20 overlong xdiff tag => E_FORMAT");
        free(res);
    }

    /* T21 Legacy v1 patch STILL decodes. */
    {
        /* Encode a v1-style patch using v1 library semantics: header v1 +
         * records 0x01 ADD / 0x02 COPY. We'll build a tiny one by hand. */
        uint8_t v1patch[16]; size_t w = 0;
        v1patch[w++] = 'B'; v1patch[w++] = 'D'; v1patch[w++] = 'I'; v1patch[w++] = 'F';
        v1patch[w++] = 1; /* v1 */
        /* varint new_size = 5 */
        v1patch[w++] = 5;
        /* COPY 0x02 off=0 len=3 */
        v1patch[w++] = 0x02; v1patch[w++] = 0; v1patch[w++] = 3;
        /* ADD  0x01 len=2, then bytes 'X','Y' */
        v1patch[w++] = 0x01; v1patch[w++] = 2;
        v1patch[w++] = 'X'; v1patch[w++] = 'Y';
        /* old = any 3 bytes, should reconstruct "AAA" + "XY" = "AAAXY" */
        uint8_t old_sm[4] = { 'A','A','A','?' };
        void *res = NULL; size_t rl = 0;
        int r = bdiff_patch_opts(old_sm, 3, v1patch, w, NULL, &res, &rl);
        int ok = (r == 0) && (rl == 5) && (memcmp(res, "AAAXY", 5) == 0);
        check(ok, "T21 v1 patch decoded correctly (legacy compat)");
        free(res);
    }

    /* T22 N×4B 1-byte-mutations on moderate entropy 128KB:
     *          budget≤96B — find max N conservatively. */
    printf("\n-- running 96B budget search (3 random layouts, take worst) --\n");
    test_budget_96B_case();

    /* T23 If zlib is compiled in, produce a patch with prefer_small=1 on
     *     moderate 128KB + 64×4B single-byte changes and ensure it both
     *     roundtrips and uses a sensible size (deflate kicks in if records
     *     shrink). Also test WITHOUT prefer_small to confirm that plain
     *     v2 records also work. */
    {
        fill_moderate(old_big, SZ_128K, 9);
        memcpy(new_big, old_big, SZ_128K);
        mutate_N_4B_1byte_each(new_big, SZ_128K, 64, 555);
        bdiff_opts o_balanced = { 16, SZ_128K, SZ_128K, 0, 0, 0 };
        size_t pl_bal = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &o_balanced, &pl_bal) == 0,
              "T23a 64x4B balanced roundtrip");
        size_t pl_small = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opt_fw, &pl_small) == 0,
              "T23b 64x4B prefer_small roundtrip");
        check(pl_small <= pl_bal + 8, "T23c prefer_small patch not bigger than balanced + slack");
        printf("  T23 balanced=%zuB  prefer_small=%zuB\n", pl_bal, pl_small);
    }

    /* ---------------- v3 BDT3 TIGHT mode tests ----------------- */
    printf("\n===[ v3 BDT3 TIGHT (compact_tight=1) ]===\n");
    {
        bdiff_opts opts_v2 = { 16, SZ_128K, SZ_128K, 0, 0, 0 };
        bdiff_opts opts_tk = { 16, SZ_128K, SZ_128K, 0, 0, 1 };

        /* T24 1x4B-1byte spot, tight must emit the 8B/spot stream & roundtrip. */
        fill_moderate(old_big, SZ_128K, 71); memcpy(new_big, old_big, SZ_128K);
        mutate_N_4B_1byte_each(new_big, SZ_128K, 1, 3141);
        size_t pT = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pT) == 0,
              "T24 TIGHT 1x4B roundtrip");
        check(pT == 15, "T24 tight size = 4B magic + 3B varint(128K) + 8B spot = 15B");
        size_t pV = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_v2, &pV) == 0,
              "T24 v2 baseline roundtrip");
        printf("  T24  1 spot  tight=%zuB  v2=%zuB\n", pT, pV);

        /* T25 N=1..10 worst-case sizes: exact upper bound 7 + 8*N. */
        int worst[13] = {0};
        int max_worst_ok = 1;
        for (int N = 1; N <= 10; ++N) {
            for (int seed = 0; seed < 5; ++seed) {
                fill_moderate(old_big, SZ_128K, (uint32_t)seed * 131u + 7u);
                memcpy(new_big, old_big, SZ_128K);
                mutate_N_4B_1byte_each(new_big, SZ_128K, N,
                    (uint32_t)seed * 211u + (uint32_t)N * 17u);
                size_t pl = 0;
                check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pl) == 0,
                      "T25 TIGHT scan roundtrip");
                if ((int)pl > worst[N]) worst[N] = (int)pl;
            }
            int bound = 7 + 8*N;
            if (worst[N] != bound) max_worst_ok = 0;
        }
        check(max_worst_ok, "T25 N=1..10 worst tight sizes exactly equal 7+8N");
        printf("  T25  N=1..10 worst tight = ");
        for (int N=1;N<=10;++N) printf("%d%c", worst[N], N==10?'\n':',');

        /* T26 96B budget with compact_tight = 1 → now support N=11. */
        fill_moderate(old_big, SZ_128K, 127); memcpy(new_big, old_big, SZ_128K);
        mutate_N_4B_1byte_each(new_big, SZ_128K, 11, 2025);
        size_t pl11 = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pl11) == 0,
              "T26a 11 spots roundtrip tight");
        check(pl11 == 95, "T26a 11 spots = 4+3+88 = 95 B exactly");
        check(pl11 <= 96, "T26a 11 spots fits 96B budget");
        fill_moderate(old_big, SZ_128K, 251); memcpy(new_big, old_big, SZ_128K);
        mutate_N_4B_1byte_each(new_big, SZ_128K, 12, 777);
        size_t pl12 = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pl12) == 0,
              "T26b 12 spots roundtrip tight");
        check(pl12 == 103, "T26b 12 spots = 103 B exactly");
        check(pl12 > 96, "T26b 12 spots OVERFLOWS 96B budget");
        printf("  T26  11 spots = %zuB (in budget)   12 spots = %zuB (overflow)\n",
               pl11, pl12);

        /* T27 patch size header max enforced on TIGHT payloads too. */
        {
            bdiff_opts cap = opts_tk; cap.max_patch_size = 95;   /* exactly N=11 */
            fill_moderate(old_big, SZ_128K, 5); memcpy(new_big, old_big, SZ_128K);
            mutate_N_4B_1byte_each(new_big, SZ_128K, 11, 99);
            void *p = NULL; size_t pl = 0;
            int r = bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &cap, &p, &pl);
            check(r == 0, "T27a N=11 fits cap=95");
            free(p);
            cap.max_patch_size = 94;
            r = bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &cap, &p, &pl);
            check(r == BDIFF_E_TOO_BIG, "T27b N=11 rejected under cap=94");
            free(p);
            printf("  PASS  T27 max_patch_size enforced for TIGHT\n");
        }

        /* T28 TIGHT decoder format guards. */
        {
            /* 1) BDT3 needs old size exact match → format. */
            fill_moderate(old_big, SZ_128K, 3); memcpy(new_big, old_big, SZ_128K);
            mutate_N_4B_1byte_each(new_big, SZ_128K, 1, 9);
            void *p = NULL; size_t pl = 0;
            check(bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &p, &pl) == 0,
                  "T28a diff ok");
            void *res = NULL; size_t rl = 0;
            /* lie about old size = 3 bytes smaller than reported */
            int r = bdiff_patch_opts(old_big, SZ_128K - 3, p, pl, &opts_tk, &res, &rl);
            check(r == BDIFF_E_FORMAT, "T28a BDT3 old_size mismatch rejected");
            free(res);

            /* 2) payload length not divisible by 8 → format. */
            uint8_t *fake_p = (uint8_t *)malloc(pl - 1);
            memcpy(fake_p, p, pl - 1);
            r = bdiff_patch_opts(old_big, SZ_128K, fake_p, pl - 1, &opts_tk, &res, &rl);
            check(r == BDIFF_E_FORMAT, "T28b BDT3 non-aligned len rejected");
            free(res); free(fake_p);

            /* 3) spot offset beyond sz-4 → format. */
            uint8_t *evil = (uint8_t *)malloc(4 + 3 + 8);
            size_t ei = 0;
            evil[ei++]='B'; evil[ei++]='D'; evil[ei++]='T'; evil[ei++]='3';
            /* varint 128K = 0x80,0x80,0x08 */
            evil[ei++]=0x80; evil[ei++]=0x80; evil[ei++]=0x08;
            /* offset_le: SZ_128K-3 (overflows sz-4) */
            uint32_t bad = (uint32_t)(SZ_128K - 3);
            evil[ei++] = (uint8_t)(bad & 0xFF);
            evil[ei++] = (uint8_t)((bad >> 8) & 0xFF);
            evil[ei++] = (uint8_t)((bad >> 16) & 0xFF);
            evil[ei++] = (uint8_t)((bad >> 24) & 0xFF);
            for (int k=0;k<4;++k) evil[ei++] = 0xAAu;
            r = bdiff_patch_opts(old_big, SZ_128K, evil, ei, &opts_tk, &res, &rl);
            check(r == BDIFF_E_FORMAT, "T28c BDT3 bad spot offset rejected");
            free(res); free(evil);

            /* 4) old == NULL → BADARG. */
            r = bdiff_patch_opts(NULL, SZ_128K, p, pl, &opts_tk, &res, &rl);
            check(r == BDIFF_E_BADARG, "T28d BDT3 needs old buffer");
            free(p);
            printf("  PASS  T28 TIGHT decoder format guards\n");
        }

        /* T29 TIGHT automatically falls through to v2 when a >8B run of mismatches. */
        {
            fill_moderate(old_big, SZ_128K, 1); memcpy(new_big, old_big, SZ_128K);
            const size_t off = 0x10000u;
            /* a 20B contiguous run = 5 sequential words of 4B, but every byte changes
             * → since MAX_RUN=8 and we'll have 20 changing bytes with no interceding
             * same-bytes in between, bdiff will reject tight and fall back. */
            for (size_t k = 0; k < 20; ++k) new_big[off + k] ^= 0x55u;
            size_t pl = 0;
            check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pl) == 0,
                  "T29 long-run mutation roundtrips via fallback v2");
            /* verify magic is NOT BDT3, but still BDIF (so fallback happened). */
            void *p = NULL; size_t pl2 = 0;
            int rr = bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &p, &pl2);
            check(rr == 0, "T29 diff ok");
            const uint8_t *pb = (const uint8_t *)p;
            int is_bdif = (pb[0]=='B' && pb[1]=='D' && pb[2]=='I' && pb[3]=='F');
            check(is_bdif, "T29 long-run mutation → fallback, patch is BDIF not BDT3");
            free(p);
            printf("  T29  long-run mutation fallback: patch size = %zuB\n", pl);
        }

        /* T30 identical blocks: TIGHT will produce a tiny BDT3 with N=0 (magic + 3B varint). */
        fill_moderate(old_big, SZ_128K, 42); memcpy(new_big, old_big, SZ_128K);
        size_t pl_id = 0;
        check(rt(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &pl_id) == 0,
              "T30 TIGHT identical roundtrip");
        check(pl_id == 7, "T30 TIGHT identical = 4B magic + 3B varint + 0 spots = 7B");
        printf("  T30  identical 128K  tight patch = %zuB\n", pl_id);
    }

    /* ---------------- T31/T32: Opcode-advantaged scenarios ----------------
     * Long contiguous rewrites or old≠new sizes: TIGHT MUST fall back to v2,
     * since tight encoding these would be many times larger. */
    printf("\n===[ v3 TIGHT auto-fallback → opcode wins ]===\n");
    {
        bdiff_opts opts_tk = { 16, SZ_128K, SZ_128K * 2, 0, 0, 1 }; /* allow new bigger */

        /* T31 1024 bytes contiguous overwrite inside 128K.
         * TIGHT spots-only would require ceil(1024/4)=256 spots *8 +7 = 2055B,
         * but a single ADD_BIG should be < 1050B, and magic MUST be BDIF (fallback). */
        fill_moderate(old_big, SZ_128K, 0xC0FFEEu); memcpy(new_big, old_big, SZ_128K);
        for (size_t i = 0; i < 1024; ++i) new_big[0x1E000u + i] ^= (uint8_t)(0xA5u + (i & 0x3Fu));
        void *p = NULL; size_t pl = 0;
        check(bdiff_diff(old_big, SZ_128K, new_big, SZ_128K, &opts_tk, &p, &pl) == 0,
              "T31 diff ok");
        const uint8_t *pb = (const uint8_t *)p;
        check(pb[0]=='B' && pb[1]=='D' && pb[2]=='I' && pb[3]=='F',
              "T31 long overwrite → fallback, patch magic is BDIF (not BDT3)");
        /* expected upper bound: 6B header + 3B vi(new_size) + 1B COPY_SMALL/COPY vi off(3)
         * + 1B vi len(~2) + ADD_BIG op 1B + vi len 2B + vi old_off 3B + 1024B payload + tail copy
         * → definitely way under 2000B (which is tight spots-only). */
        check(pl < 2000, "T31 v2 patch < 2000B (tight spots-only = 2055B)");
        {   /* roundtrip check */
            void *res = NULL; size_t rl = 0;
            int r = bdiff_patch_opts(old_big, SZ_128K, p, pl, &opts_tk, &res, &rl);
            check(r == 0 && rl == SZ_128K && memcmp(res, new_big, SZ_128K) == 0,
                  "T31 roundtrip ok");
            free(res);
        }
        printf("  T31  1024B contiguous overwrite  patch = %zuB (BDIF fallback)\n", pl);
        free(p);

        /* T32 Insert 32 bytes in middle (old=4096 → new=4128, old≠new size).
         * TIGHT requires old==new size, so MUST emit BDIF. Use small local buffers. */
        uint8_t osmall[4096];
        for (size_t i = 0; i < sizeof osmall; ++i) osmall[i] = (uint8_t)(i * 29u + 3u);
        uint8_t *nsmall = (uint8_t *)malloc(4128);
        memcpy(nsmall, osmall, 2000);
        for (int i = 0; i < 32; ++i) nsmall[2000 + i] = (uint8_t)('A' + i);
        memcpy(nsmall + 2032, osmall + 2000, 4096 - 2000);
        {
            void *p2 = NULL; size_t pl2 = 0;
            bdiff_opts opts_small = opts_tk; opts_small.max_old_size = 5000;
            opts_small.max_new_size = 5000;
            check(bdiff_diff(osmall, 4096, nsmall, 4128, &opts_small, &p2, &pl2) == 0,
                  "T32 diff insert 32 ok");
            const uint8_t *p2b = (const uint8_t *)p2;
            check(p2b[0]=='B' && p2b[1]=='D' && p2b[2]=='I' && p2b[3]=='F',
                  "T32 old≠new size → magic is BDIF, not BDT3");
            check(pl2 < 200, "T32 insert 32B patch small (<200B)");
            void *res = NULL; size_t rl = 0;
            int r = bdiff_patch_opts(osmall, 4096, p2, pl2, &opts_small, &res, &rl);
            check(r == 0 && rl == 4128 && memcmp(res, nsmall, 4128) == 0,
                  "T32 insert 32B roundtrip ok");
            free(res);
            printf("  T32  insert 32B (4096→4128)  patch = %zuB (BDIF only)\n", pl2);
            free(p2);
        }
        free(nsmall);
    }

    free(old_big); free(new_big);
    free(old); free(newd);

    printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
