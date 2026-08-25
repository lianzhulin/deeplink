/* Demo: patch comparison — v1 vs v2 C source → compiled ELF diff
 *
 * What changed (C-level):
 *   1. FW_VERSION   0x0100 → 0x0200   (version bump, 4B cfg field)
 *   2. MAX_DEVICES   8     → 16       (device count, 4B cfg field)
 *   3. BAUD_RATE    115200 → 921600   (baud, 4B cfg field)
 *   4. TIMEOUT_MS    3000  → 5000    (timeout, 4B cfg field)
 *   5. fw_name "v1.0" → "v2.0"       (2 bytes in .rodata string)
 *   6. checksum 0x5A → 0xA5           (1 byte in cfg struct)
 *
 * Total: ~7 bytes changed across 6 locations, old_size == new_size.
 * Some changes are contiguous (cfg struct fields), some scattered.
 */
#include "bdiff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t FW_SZ = 128u * 1024u;  /* typical firmware block */

int main(void) {
    /* Load both ELF files into 128KB buffers (pad with zeros). */
    FILE *f1 = fopen("demo_v1_nobid.elf", "rb");
    FILE *f2 = fopen("demo_v2_nobid.elf", "rb");
    if (!f1 || !f2) { fprintf(stderr, "cannot open ELF\n"); return 1; }

    uint8_t *old = (uint8_t *)calloc(1, FW_SZ);
    uint8_t *newd = (uint8_t *)calloc(1, FW_SZ);
    /* Read first 128KB of each ELF */
    size_t r1 = fread(old, 1, FW_SZ, f1);
    size_t r2 = fread(newd, 1, FW_SZ, f2);
    fclose(f1); fclose(f2);
    printf("ELF read: v1=%zu bytes, v2=%zu bytes (padded to %zu)\n\n", r1, r2, FW_SZ);

    /* Count differing bytes */
    size_t diff_bytes = 0, diff_regions = 0;
    int in_diff = 0;
    for (size_t i = 0; i < FW_SZ; i++) {
        if (old[i] != newd[i]) {
            if (!in_diff) { diff_regions++; in_diff = 1; }
            diff_bytes++;
        } else {
            in_diff = 0;
        }
    }
    printf("Binary diff: %zu bytes changed across %zu regions\n\n", diff_bytes, diff_regions);

    /* --- Mode 1: v2 opcode (compact_tight=0) --- */
    {
        bdiff_opts opts = { 16, FW_SZ, FW_SZ, 0, 1, 0 };  /* prefer_small */
        void *patch = NULL; size_t pl = 0;
        int rc = bdiff_diff(old, FW_SZ, newd, FW_SZ, &opts, &patch, &pl);
        if (rc != 0) { printf("v2 diff failed: %d\n", rc); return 1; }

        /* Verify roundtrip */
        void *res = NULL; size_t rl = 0;
        int prc = bdiff_patch_opts(old, FW_SZ, patch, pl, &opts, &res, &rl);
        int ok = (prc == 0 && rl == FW_SZ && memcmp(res, newd, FW_SZ) == 0);
        free(res);

        /* Check magic */
        int is_bdif = (pl >= 4 && ((uint8_t*)patch)[0]=='B' && ((uint8_t*)patch)[1]=='D');
        int is_bdt3 = (pl >= 4 && ((uint8_t*)patch)[0]=='B' && ((uint8_t*)patch)[1]=='D'
                      && ((uint8_t*)patch)[2]=='T');
        printf("=== Mode 1: v2 opcode (compact_tight=0, prefer_small=1) ===\n");
        printf("  patch size  : %zu bytes\n", pl);
        printf("  format      : %s\n", is_bdt3 ? "BDT3 (TIGHT)" : is_bdif ? "BDIF (v2)" : "unknown");
        printf("  roundtrip   : %s\n", ok ? "PASS" : "FAIL");
        free(patch);
    }

    printf("\n");

    /* --- Mode 2: v3 TIGHT (compact_tight=1) --- */
    {
        bdiff_opts opts = { 16, FW_SZ, FW_SZ, 0, 0, 1 };  /* compact_tight */
        void *patch = NULL; size_t pl = 0;
        int rc = bdiff_diff(old, FW_SZ, newd, FW_SZ, &opts, &patch, &pl);
        if (rc != 0) { printf("TIGHT diff failed: %d\n", rc); return 1; }

        /* Verify roundtrip */
        void *res = NULL; size_t rl = 0;
        int prc = bdiff_patch_opts(old, FW_SZ, patch, pl, &opts, &res, &rl);
        int ok = (prc == 0 && rl == FW_SZ && memcmp(res, newd, FW_SZ) == 0);
        free(res);

        int is_bdif = (pl >= 4 && ((uint8_t*)patch)[0]=='B' && ((uint8_t*)patch)[1]=='D'
                      && ((uint8_t*)patch)[2]=='I');
        int is_bdt3 = (pl >= 4 && ((uint8_t*)patch)[0]=='B' && ((uint8_t*)patch)[1]=='D'
                      && ((uint8_t*)patch)[2]=='T');
        printf("=== Mode 2: v3 TIGHT (compact_tight=1) ===\n");
        printf("  patch size  : %zu bytes\n", pl);
        printf("  format      : %s\n", is_bdt3 ? "BDT3 (TIGHT)" : is_bdif ? "BDIF (v2 fallback)" : "unknown");
        printf("  roundtrip   : %s\n", ok ? "PASS" : "FAIL");
        free(patch);
    }

    /* --- Mode 3: v2 opcode + deflate (prefer_small + ZLIB) --- */
    printf("\n");
    {
        bdiff_opts opts = { 16, FW_SZ, FW_SZ, 0, 1, 0 };
        void *patch = NULL; size_t pl = 0;
        int rc = bdiff_diff(old, FW_SZ, newd, FW_SZ, &opts, &patch, &pl);
        if (rc == 0) {
            void *res = NULL; size_t rl = 0;
            int prc = bdiff_patch_opts(old, FW_SZ, patch, pl, &opts, &res, &rl);
            int ok = (prc == 0 && rl == FW_SZ && memcmp(res, newd, FW_SZ) == 0);
            free(res);
            printf("=== Mode 3: v2 opcode + deflate envelope (prefer_small=1, ZLIB) ===\n");
            printf("  patch size  : %zu bytes\n", pl);
            printf("  roundtrip   : %s\n", ok ? "PASS" : "FAIL");
            free(patch);
        }
    }

    /* --- Theoretical bare 6B/spot estimate --- */
    printf("\n=== Theoretical: bare 6B/spot (manual count) ===\n");
    printf("  If all %zu diff bytes were 4B-aligned isolated spots:\n", diff_bytes);
    printf("  estimated spots = ceil(%zu / 4) = %zu\n", diff_bytes, (diff_bytes + 3) / 4);
    printf("  estimated size  = 7 + 6 * %zu = %zu bytes\n",
           (diff_bytes + 3) / 4, 7 + 6 * ((diff_bytes + 3) / 4));

    free(old); free(newd);
    return 0;
}
