#include "bdiff.h"

#include <stdlib.h>
#include <string.h>

#if defined(BDIFF_HAVE_ZLIB) && BDIFF_HAVE_ZLIB
#include <zlib.h>
#define BDIFF_ZLIB_ 1
#else
#define BDIFF_ZLIB_ 0
#endif

/* ------------------------------------------------------------------ *
 *  Minimal patch for fixed-length data blocks.   Version 2 encoder.
 *
 *  Algorithm (block-aligned hash index + greedy longest match, plus
 *  the v2 record encodings described in bdiff.h):
 *    1. Partition old data into fixed, block-aligned chunks of size B.
 *    2. Index each chunk by a 32-bit FNV-1a hash.
 *    3. Scan new data L→R; at each position try the longest forward match
 *       (≥ B bytes); on a hit extend backward over pending-ADD bytes that
 *       also match the old stream.
 *    4. On emit, pick the smallest v2 opcode shape for each COPY / ADD:
 *         • COPY off∈[0,1023] len∈[1,8]  → COPY_SMALL_BOTH  (2 bytes!)
 *         • COPY len∈[1,31], any off     → COPY_SMALL_LEN   (1+N bytes)
 *         • otherwise                     → COPY_BIG         (2 varints)
 *         • ADD  len∈[1,31]               → ADD_SMALL (opcode = len itself)
 *         • otherwise                     → ADD_BIG           (1 varint)
 *         • if we also know the output XOR against old bytes and it's
 *           cheaper, emit ADDX instead of ADD (zero-run micro-coder).
 *    5. Records are then wrapped in a header. If BDIFF_HAVE_ZLIB and
 *       opts.prefer_small, the records stream is additionally passed
 *       through raw-deflate; if it shrinks by ≥10% and ≥11 bytes we
 *       keep the envelope, otherwise we ship plain records.
 *
 *  The PATCH side still accepts BOTH v1 (opcodes 0x01 ADD / 0x02 COPY
 *  with header version = 1) AND v2 patches, with the same public error
 *  codes we had before (plus the new E_TOO_BIG / E_VERSION).
 * ------------------------------------------------------------------ */

/* ----------------------------- varint ----------------------------- */
static size_t vi_enc(uint64_t v, uint8_t *buf) {
    size_t n = 0;
    while (v >= 0x80u) { buf[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    buf[n++] = (uint8_t)v;
    return n;
}
static int vi_dec(const uint8_t *p, const uint8_t *end,
                 uint64_t *out, size_t *n) {
    uint64_t v = 0; size_t k = 0; int s = 0;
    while (k < (size_t)10) {
        if (p + k >= end) return -1;
        uint8_t b = p[k];
        v |= (uint64_t)(b & 0x7fu) << s;
        ++k;
        if (!(b & 0x80u)) { *out = v; *n = k; return 0; }
        s += 7;
    }
    return -1; /* varint longer than 10 bytes = malformed */
}
static size_t vi_size(uint64_t v) __attribute__((unused));
static size_t vi_size(uint64_t v) {
    size_t n = 0;
    do { n++; v >>= 7; } while (v);
    return n;
}

/* ------------------------------ hash ------------------------------ */
static uint32_t hash_block(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* -------------------------- output buffer ------------------------- */
typedef struct { uint8_t *buf; size_t len, cap; } obuf;

static int ob_reserve(obuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return 0;
    size_t ncap = o->cap ? o->cap : 64;
    while (ncap < o->len + extra) {
        if (ncap > ((size_t)-1) / 2) return -1;
        ncap *= 2;
    }
    uint8_t *nb = (uint8_t *)realloc(o->buf, ncap);
    if (!nb) return -1;
    o->buf = nb; o->cap = ncap;
    return 0;
}
static int ob_put(obuf *o, const uint8_t *p, size_t n) {
    if (!n) return 0;
    if (ob_reserve(o, n)) return -1;
    memcpy(o->buf + o->len, p, n);
    o->len += n;
    return 0;
}
static int ob_u8(obuf *o, uint8_t v) {
    if (ob_reserve(o, 1)) return -1;
    o->buf[o->len++] = v;
    return 0;
}
static int ob_vi(obuf *o, uint64_t v) {
    uint8_t tmp[10];
    size_t n = vi_enc(v, tmp);
    return ob_put(o, tmp, n);
}

/* --------------------------- hash index --------------------------- */
typedef struct { uint32_t h; size_t off; } entry;

static int cmp_entry(const void *a, const void *b) {
    uint32_t ha = ((const entry *)a)->h;
    uint32_t hb = ((const entry *)b)->h;
    if (ha < hb) return -1;
    if (ha > hb) return 1;
    return 0;
}

/* Longest forward match at new[np]. Returns length >= bs on hit. */
static size_t find_longest(const entry *e, size_t ne,
                           const uint8_t *old, size_t old_size, size_t bs,
                           const uint8_t *newd, size_t new_size, size_t np,
                           size_t *out_off) {
    if (np + bs > new_size) return 0;
    uint32_t h = hash_block(newd + np, bs);
    size_t lo = 0, hi = ne;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e[mid].h < h) lo = mid + 1; else hi = mid;
    }
    size_t best_len = 0, best_off = 0;
    for (size_t i = lo; i < ne && e[i].h == h; ++i) {
        size_t o = e[i].off;
        if (o + bs > old_size) continue;
        if (memcmp(old + o, newd + np, bs) != 0) continue;
        size_t ext = bs;
        while (o + ext < old_size && np + ext < new_size &&
               old[o + ext] == newd[np + ext])
            ++ext;
        if (ext > best_len) { best_len = ext; best_off = o; }
    }
    if (best_len >= bs) { *out_off = best_off; return best_len; }
    return 0;
}

/* --------------- xdiff (xor zero-run micro coder) ----------------- */

/* Encode xor_diff[0..len-1] into xout using the 2-bit tag format described
 * in bdiff.h.  Returns the number of encoded bytes written. Returns 0 on
 * allocation failure. Caller frees *out on success. */
static size_t xdiff_encode(const uint8_t *x, size_t len, uint8_t **out) {
    /* Worst case: mode 0 (RAW1) for every 64-byte run = 1 tag + 64 bytes
     * per 64 bytes; smaller runs may still hit +1 byte total if they're
     * truly random non-zero. Over-allocate len+ceil(len/63)+1 to fit all.*/
    size_t cap = len + 1 + (len + 62) / 63 + 8;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) { *out = NULL; return 0; }
    size_t w = 0; /* bytes written so far */
    size_t i = 0;
    while (i < len) {
        /* Step 1: classify current run. */
        uint8_t v = x[i];
        size_t rl = 1;
        while (rl < 65 && i + rl < len && x[i + rl] == v) rl++;
        /* If len >= 2 and v == 0 or v == anything same-byte long: ZEROS /
         * SAME1 are the win. Otherwise fall back to RAW for 1..64 bytes. */
        if (rl >= 2) {
            size_t L = (rl > 64) ? 64 : rl; /* limit 6 bits of len-1 */
            if (v == 0x00) {
                /* ZEROS mode = 01 */
                b[w++] = (uint8_t)(((L - 1) << 2) | 0x01);
            } else {
                /* SAME1 mode = 10 */
                b[w++] = (uint8_t)(((L - 1) << 2) | 0x02);
                b[w++] = v;
            }
            i += L;
            continue;
        }
        /* Run of 1 (or of a single non-repeating byte). Look ahead and see
         * how many consecutive bytes we should issue as a RAW run. RAW mode
         * 00 encodes 1..64 bytes with tag + bytes; overhead 1 byte. */
        size_t raw_l = 0;
        while (raw_l < 64 && i + raw_l < len) {
            uint8_t vv = x[i + raw_l];
            /* Peek 1 ahead: can we actually enter a same-byte run starting
             * here? If yes, stop raw run before it. */
            size_t same = 1;
            while (same < 64 && i + raw_l + same < len &&
                   x[i + raw_l + same] == vv) same++;
            if (same >= 2) break; /* close RAW, next loop codes the run */
            raw_l++;
        }
        if (raw_l == 0) raw_l = 1; /* safe fallback */
        if (raw_l > 64) raw_l = 64;
        b[w++] = (uint8_t)(((raw_l - 1) << 2) | 0x00); /* RAW mode 00 */
        memcpy(b + w, x + i, raw_l);
        w += raw_l;
        i += raw_l;
    }
    *out = b;
    return w;
}

/* Returns 0 on success, -1 on format error. Caller passes xor_out (of
 * exactly expect_len bytes). Sets *consumed to bytes of enc read. */
static int xdiff_decode(const uint8_t *enc, size_t enc_len, size_t expect_len,
                        uint8_t *xor_out, size_t *consumed) {
    size_t i = 0, p = 0;
    while (i < enc_len && p < expect_len) {
        uint8_t tag = enc[i++];
        unsigned mode = tag & 3u;
        unsigned Lm1  = tag >> 2;       /* 6 bits, 0..63 */
        size_t L = (size_t)Lm1 + 1;
        if (p + L > expect_len) return -1;
        switch (mode) {
            case 0: /* RAW(L) */
                if (i + L > enc_len) return -1;
                memcpy(xor_out + p, enc + i, L);
                i += L; p += L;
                break;
            case 1: /* ZEROS(L) */
                memset(xor_out + p, 0, L);
                p += L;
                break;
            case 2: /* SAME1(L, V) */
                if (i + 1 > enc_len) return -1;
                memset(xor_out + p, enc[i++], L);
                p += L;
                break;
            case 3: /* RAW1 = L+2 bytes raw (escape) */
                L += 1; /* L+2 raw total minus 1 we already counted */
                if (p + L > expect_len) return -1;
                if (i + L > enc_len) return -1;
                memcpy(xor_out + p, enc + i, L);
                i += L; p += L;
                break;
        }
    }
    if (p != expect_len) return -1;
    *consumed = i;
    return 0;
}

/* -------------------- raw-deflate wrapper (zlib) ------------------ */
#if BDIFF_ZLIB_
static int z_wrap(const uint8_t *in, size_t in_len,
                  uint8_t **out, size_t *out_len) {
    uLongf dst_len = compressBound((uLong)in_len);
    uint8_t *dst = (uint8_t *)malloc(dst_len ? dst_len : 1);
    if (!dst) return -1;
    z_stream s; memset(&s, 0, sizeof s);
    s.next_in  = (Bytef *)in;  s.avail_in  = (uInt)in_len;
    s.next_out = dst;          s.avail_out = (uInt)dst_len;
    if (deflateInit2(&s, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(dst); return -1;
    }
    int r = deflate(&s, Z_FINISH);
    deflateEnd(&s);
    if (r != Z_STREAM_END) { free(dst); return -1; }
    dst_len = s.total_out;
    *out = dst; *out_len = (size_t)dst_len;
    return 0;
}
static int z_unwrap(const uint8_t *in, size_t in_len,
                    uint8_t **out, size_t out_len) {
    uint8_t *dst = (uint8_t *)malloc(out_len ? out_len : 1);
    if (!dst) return -1;
    z_stream s; memset(&s, 0, sizeof s);
    s.next_in = (Bytef *)in;  s.avail_in  = (uInt)in_len;
    s.next_out = dst;         s.avail_out = (uInt)out_len;
    if (inflateInit2(&s, -15) != Z_OK) { free(dst); return -1; }
    int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END || s.total_out != out_len) { free(dst); return -1; }
    *out = dst;
    return 0;
}
#endif /* BDIFF_ZLIB_ */

/* --------------- best-shape emitters (records stream) ------------- */

/* ------------------------------------------------------------------ *
 *  ADD emitter: decides ADD/ADDX + small/big shape, writes the record.
 *    new_bytes/new_len          pending ADD region (new buffer bytes).
 *    old_buf / old_size         old buffer (for ADDX XOR reference).
 *    corresponding_old_offset   old offset aligned 1:1 with the ADD
 *                               region when it's "mostly same bytes as
 *                               old but a few changed".  Pass SIZE_MAX
 *                               if unknown → ADDX is skipped.
 *  Picks the smallest of:
 *      {ADD_SMALL, ADD_BIG, ADDX_SMALL, ADDX_BIG}
 *  and appends it to o.  Returns 0 on success, -1 on OOM.
 * ------------------------------------------------------------------ */
static int emit_add_by_offset(obuf *o,
                              const uint8_t *new_bytes, size_t new_len,
                              const uint8_t *old_buf, size_t old_size,
                              size_t corresponding_old_offset) {
    /* --- Option A: plain ADD --- */
    uint8_t hdr_a[11]; size_t hdr_a_len;
    if (new_len >= 1 && new_len <= 31) {
        hdr_a[0] = (uint8_t)new_len;     /* ADD_SMALL */
        hdr_a_len = 1;
    } else {
        hdr_a[0] = 0x00;                 /* ADD_BIG   */
        hdr_a_len = 1 + vi_enc((uint64_t)new_len, hdr_a + 1);
    }
    size_t size_a = hdr_a_len + new_len;

    /* --- Option B: ADDX (skip if no old alignment, or size doesn't win) --- */
    int try_b = 0;
    if (corresponding_old_offset != (size_t)-1 && old_buf != NULL &&
        old_size >= new_len &&
        corresponding_old_offset <= old_size - new_len) {
        try_b = 1;
    }

    size_t size_b = (size_t)-1;
    uint8_t *hdr_b = NULL; size_t hdr_b_len = 0;
    uint8_t *xdiff_enc = NULL; size_t xdiff_enc_len = 0;

    if (try_b) {
        /* Compute xor-diff. */
        uint8_t *tmp = (uint8_t *)malloc(new_len ? new_len : 1);
        if (!tmp) goto no_b;
        for (size_t k = 0; k < new_len; ++k)
            tmp[k] = new_bytes[k] ^ old_buf[corresponding_old_offset + k];
        xdiff_enc_len = xdiff_encode(tmp, new_len, &xdiff_enc);
        free(tmp);
        if (!xdiff_enc_len) {
            free(xdiff_enc); xdiff_enc = NULL;
            goto no_b;
        }
        /* Build header shape: ADDX_SMALL if len 1..31 else ADDX_BIG. */
        hdr_b = (uint8_t *)malloc(1 + 10 + 10 + 1);
        if (!hdr_b) goto no_b;
        size_t w = 0;
        if (new_len >= 1 && new_len <= 31) {
            hdr_b[w++] = (uint8_t)(0x20 | (uint8_t)new_len); /* ADDX_SMALL */
        } else {
            hdr_b[w++] = 0x20;                              /* ADDX_BIG   */
            w += vi_enc((uint64_t)new_len, hdr_b + w);
        }
        w += vi_enc((uint64_t)corresponding_old_offset, hdr_b + w);
        w += vi_enc((uint64_t)xdiff_enc_len, hdr_b + w);
        hdr_b_len = w;
        size_b = hdr_b_len + xdiff_enc_len;
    }
no_b:

    /* Pick smaller.  Break ties in favour of raw ADD (simpler decoder,
     * same size anyway). */
    int pick_b = try_b && size_b < size_a;

    if (!pick_b) {
        if (ob_put(o, hdr_a, hdr_a_len)) goto oom;
        if (new_len && ob_put(o, new_bytes, new_len)) goto oom;
        free(hdr_b); free(xdiff_enc);
        return 0;
    } else {
        if (ob_put(o, hdr_b, hdr_b_len)) goto oom;
        if (xdiff_enc_len && ob_put(o, xdiff_enc, xdiff_enc_len)) goto oom;
        free(hdr_b); free(xdiff_enc);
        return 0;
    }
oom:
    free(hdr_b); free(xdiff_enc);
    return -1;
}

/* Emit a COPY record in the smallest v2 shape that fits.
 * Returns 0 on success, -1 on OOM. */
static int emit_copy(obuf *o, size_t off, size_t len) {
    /* COPY_SMALL_BOTH: off∈[0,1023], len∈[1,8] => 2 bytes total. */
    if (len >= 1 && len <= 8 && off <= 1023) {
        uint8_t b[2];
        b[0] = (uint8_t)(0x60 | (((off >> 8) & 3) << 3) | ((len - 1) & 7));
        b[1] = (uint8_t)(off & 0xFFu);
        return ob_put(o, b, 2);
    }
    /* COPY_SMALL_LEN: len∈[1,31], any off => 1 + vi_size((uint64_t)off) bytes. */
    if (len >= 1 && len <= 31) {
        if (ob_u8(o, (uint8_t)(0x40 | (uint8_t)len))) return -1;
        return ob_vi(o, (uint64_t)off);
    }
    /* COPY_BIG. */
    if (ob_u8(o, 0x40)) return -1;
    if (ob_vi(o, (uint64_t)off)) return -1;
    return ob_vi(o, (uint64_t)len);
}

/* =============================== diff ============================== */
int bdiff_diff(const void *old_data, size_t old_size,
               const void *new_data, size_t new_size,
               const bdiff_opts *opts_in,
               void **out, size_t *out_len) {
    if (!out || !out_len) return BDIFF_E_BADARG;
    if (old_size && !old_data) return BDIFF_E_BADARG;
    if (new_size && !new_data) return BDIFF_E_BADARG;

    bdiff_opts opts;
    if (opts_in) opts = *opts_in;
    else         memset(&opts, 0, sizeof opts);
    if (!opts.block_size) opts.block_size = 16;

    if (opts.max_old_size && old_size > opts.max_old_size) return BDIFF_E_TOO_BIG;
    if (opts.max_new_size && new_size > opts.max_new_size) return BDIFF_E_TOO_BIG;

    const uint8_t *old  = (const uint8_t *)old_data;
    const uint8_t *newd = (const uint8_t *)new_data;
    const size_t bs = opts.block_size;
    obuf records = {0, 0, 0};
    entry *e = NULL;
    size_t ne = 0;
    int rc = BDIFF_OK;

    /* ---- build hash index over block-aligned positions ---- */
    if (old_size >= bs) {
        ne = old_size / bs;
        e = (entry *)malloc(ne * sizeof(entry));
        if (!e) { rc = BDIFF_E_NOMEM; goto done; }
        for (size_t i = 0; i < ne; ++i) {
            e[i].off = i * bs;
            e[i].h   = hash_block(old + e[i].off, bs);
        }
        qsort(e, ne, sizeof(entry), cmp_entry);
    }

    /* ---- main scan + emit into RECORDS stream ---- */
    size_t np = 0;                 /* current scan position in new */
    size_t add_start = 0;          /* start of pending ADD region in new */
    size_t last_copy_old_end = 0;  /* old offset just after last COPY,
                                      used to hint ADDX correspondence */
    (void)last_copy_old_end;

    while (np < new_size) {
        size_t off = 0;
        size_t m = find_longest(e, ne, old, old_size, bs,
                                newd, new_size, np, &off);
        if (m >= bs) {
            /* Backward extension: absorb matching pending-ADD bytes. */
            size_t maxb = np - add_start;
            if (maxb > off) maxb = off;
            size_t b = 0;
            while (b < maxb && newd[np - b - 1] == old[off - b - 1]) ++b;

            size_t add_len = (np - b) - add_start;
            if (add_len) {
                /* Correspondence: the pending ADD bytes started at
                 * add_start in new.  If they align with old bytes at
                 * `(off - b) - add_len` (i.e. right before the extended
                 * COPY), we can try ADDX. */
                size_t corr = (size_t)-1;
                size_t copy_new_start_at = off - b; /* numeric old_off */
                if (copy_new_start_at >= add_len) {
                    corr = copy_new_start_at - add_len;
                }
                if (emit_add_by_offset(&records,
                                       newd + add_start, add_len,
                                       old, old_size, corr)) {
                    rc = BDIFF_E_NOMEM; goto done;
                }
            }
            if (emit_copy(&records, off - b, m + b)) {
                rc = BDIFF_E_NOMEM; goto done;
            }
            np += m;
            add_start = np;
            last_copy_old_end = (off - b) + (m + b);
        } else {
            np++;
        }
    }
    if (np > add_start) {
        size_t add_len = np - add_start;
        /* Hint: if the last copy ended at some old position X and the
         * new position (add_start) immediately follows the copy end in
         * new space, the correspondence is simply X (linear flow). We
         * can't easily recover that without storing "new position after
         * last copy", so for now only hint if the bytes actually match
         * line-by-line by using the last COPY's OLD end only when it's
         * still a valid correspondence.  Simplest: if last_copy_old_end
         * + add_len <= old_size, then treat the trailing ADD as "old at
         * last_copy_old_end" — often correct (e.g. COPY followed by 4
         * bytes appended at the end of a firmware block). */
        size_t corr = (size_t)-1;
        if (old && last_copy_old_end + add_len <= old_size) {
            corr = last_copy_old_end;
        }
        if (emit_add_by_offset(&records, newd + add_start, add_len,
                               old, old_size, corr)) {
            rc = BDIFF_E_NOMEM; goto done;
        }
    }

    /* --- Final assembly: header (6 B) + varint(new_size) + either
     *     plain records OR raw-deflate envelope (BDIFF_HAVE_ZLIB &&
     *     prefer_small && strictly smaller). */
    {
        obuf fin = {0,0,0};
        uint8_t hdr[6]; size_t hl = 0;
        hdr[hl++] = 'B'; hdr[hl++] = 'D'; hdr[hl++] = 'I'; hdr[hl++] = 'F';
        hdr[hl++] = BDIFF_VERSION;
        hdr[hl++] = 0; /* flags; overwritten below if wrapping */

        /* payload / payload_len describe what follows hdr + new_size. */
        uint8_t *payload = records.buf;
        size_t   payload_len = records.len;
        uint8_t *zfree = NULL;
#if BDIFF_ZLIB_
        if (opts.prefer_small && records.len > 50) {
            uint8_t *zbuf = NULL; size_t zlen = 0;
            if (z_wrap(records.buf, records.len, &zbuf, &zlen) == 0) {
                /* Header overhead for envelope = flags(1 already counted)
                 * + vi(inner_size) ~ 1..3 bytes.  Require strict win: */
                size_t env_total = zlen + vi_size((uint64_t)records.len);
                if (env_total + 2 < records.len) { /* strictly smaller */
                    hdr[5] = 1; /* flags: bit0 deflate */
                    /* Prepend inner_size varint to the payload. */
                    uint8_t vbuf[10]; size_t vlen = vi_enc((uint64_t)records.len, vbuf);
                    uint8_t *combo = (uint8_t *)malloc(vlen + zlen);
                    if (combo) {
                        memcpy(combo, vbuf, vlen);
                        memcpy(combo + vlen, zbuf, zlen);
                        free(zbuf);
                        zfree = combo;
                        payload = combo;
                        payload_len = vlen + zlen;
                    } else {
                        free(zbuf);
                    }
                } else {
                    free(zbuf);
                }
            }
        }
#endif
        if (ob_put(&fin, hdr, 6)) { rc = BDIFF_E_NOMEM; free(zfree); /* leak fin ? */; goto done_free_fin; }
        if (ob_vi(&fin, (uint64_t)new_size)) { rc = BDIFF_E_NOMEM; free(zfree); goto done_free_fin; }
        if (payload_len && ob_put(&fin, payload, payload_len)) { rc = BDIFF_E_NOMEM; free(zfree); goto done_free_fin; }

        free(zfree); zfree = NULL;

        if (opts.max_patch_size && fin.len > opts.max_patch_size) {
            rc = BDIFF_E_TOO_BIG;
            goto done_free_fin;
        }

        *out = fin.buf; *out_len = fin.len;
        rc = BDIFF_OK;
        free(records.buf); records.buf = NULL;
        free(e); return rc;
done_free_fin:
        free(fin.buf);
        free(zfree);
    }

done:
    free(records.buf);
    free(e);
    *out = NULL; *out_len = 0;
    return rc;
}

/* ============================== patch ============================= */

/* Small helper for the records-stream decoder: decodes a single ADDX
 * record, writing xor(old[off..off+len-1], xor_mask_decoded) into out.
 * Returns 0 on success, -1 on format / bounds errors.  Advances *pp.*/
static int decode_addx(uint8_t op,
                       const uint8_t **pp, const uint8_t *end,
                       const uint8_t *old, size_t old_size,
                       obuf *out, uint64_t *produced, uint64_t nsize) {
    uint64_t len;
    if (op == 0x20) {   /* ADDX_BIG */
        uint64_t v; size_t k;
        if (vi_dec(*pp, end, &v, &k)) return -1;
        *pp += k; len = v;
    } else {            /* ADDX_SMALL: len = op & 0x1F */
        len = (uint64_t)(op & 0x1Fu);
        if (len == 0) return -1;
    }
    uint64_t off, enclen; size_t k;
    if (vi_dec(*pp, end, &off, &k))    return -1;
    *pp += k;
    if (vi_dec(*pp, end, &enclen, &k)) return -1;
    *pp += k;
    if (len > nsize - *produced) return -1;
    if (off > (uint64_t)old_size || len > (uint64_t)old_size ||
        off > (uint64_t)old_size - (size_t)len) return -1;
    if ((uint64_t)(end - *pp) < enclen) return -1;

    uint8_t *mask = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
    if (!mask) return -2; /* callers treat -2 as OOM path, below map to NOMEM */
    size_t consumed = 0;
    if (xdiff_decode(*pp, (size_t)enclen, (size_t)len, mask, &consumed)) {
        free(mask); return -1;
    }
    if (consumed != (size_t)enclen) {
        free(mask); return -1; /* trailing bytes inside ADDX are corrupt */
    }
    *pp += (size_t)enclen;

    /* output: old[off..off+len-1] XOR mask */
    uint8_t *row = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
    if (!row) { free(mask); return -2; }
    for (uint64_t i = 0; i < len; ++i) {
        row[(size_t)i] = old[(size_t)(off + i)] ^ mask[(size_t)i];
    }
    int r = ob_put(out, row, (size_t)len);
    free(row); free(mask);
    if (r) return -2;
    *produced += len;
    return 0;
}

/* Decode the records stream between rec and rec_end into the output
 * buffer out, tracking produced bytes against the declared nsize.
 * Returns 0 on success, -1 format, -2 OOM. */
static int decode_records(const uint8_t *rec, const uint8_t *rec_end,
                          const uint8_t *old, size_t old_size,
                          obuf *out, uint64_t nsize) {
    const uint8_t *p = rec;
    uint64_t produced = 0;
    while (produced < nsize) {
        if (p >= rec_end) return -1;
        uint8_t op = *p++;

        /* Dispatch opcodes: 0x00 = ADD_BIG; 0x01..0x1F = ADD_SMALL. */
        if (op == 0x00) {
            /* ADD_BIG (v2). */
            uint64_t len; size_t k;
            if (vi_dec(p, rec_end, &len, &k)) return -1;
            p += k;
            if (len > nsize - produced) return -1;
            if ((uint64_t)(rec_end - p) < len) return -1;
            if (ob_put(out, p, (size_t)len)) return -2;
            p += (size_t)len; produced += len;
            continue;
        }
        if (op >= 1 && op <= 0x1F) {
            /* ADD_SMALL (v2): opcode itself IS the len (1..31). */
            size_t len = (size_t)op;
            if ((uint64_t)len > nsize - produced) return -1;
            if ((size_t)(rec_end - p) < len) return -1;
            if (ob_put(out, p, len)) return -2;
            p += len; produced += len;
            continue;
        }

        if (op >= 0x21 && op <= 0x3F) {
            /* ADDX_SMALL. */
            int r = decode_addx(op, &p, rec_end, old, old_size, out, &produced, nsize);
            if (r == -1) return -1;
            if (r == -2) return -2;
            continue;
        }
        if (op == 0x20) {
            int r = decode_addx(0x20, &p, rec_end, old, old_size, out, &produced, nsize);
            if (r == -1) return -1;
            if (r == -2) return -2;
            continue;
        }

        if (op >= 0x60 && op <= 0x7F) {
            /* COPY_SMALL_BOTH: 2-byte record. */
            if (p >= rec_end) return -1;
            uint8_t lo = *p++;
            uint64_t off = (uint64_t)(((op >> 3) & 3u) << 8) | (uint64_t)lo;
            uint64_t len = (uint64_t)((op & 7u) + 1);
            if (len > nsize - produced) return -1;
            if (len > (uint64_t)old_size) return -1;
            if (off > (uint64_t)old_size - (size_t)len) return -1;
            if (ob_put(out, old + (size_t)off, (size_t)len)) return -2;
            produced += len;
            continue;
        }
        if (op >= 0x41 && op <= 0x5F) {
            /* COPY_SMALL_LEN: len = (op&0x1F); then varint off */
            uint64_t len = (uint64_t)(op & 0x1Fu);
            uint64_t off; size_t k;
            if (vi_dec(p, rec_end, &off, &k)) return -1;
            p += k;
            if (len > nsize - produced) return -1;
            if (len > (uint64_t)old_size) return -1;
            if (off > (uint64_t)old_size - (size_t)len) return -1;
            if (ob_put(out, old + (size_t)off, (size_t)len)) return -2;
            produced += len;
            continue;
        }
        if (op == 0x40) {
            /* COPY_BIG */
            uint64_t off, len; size_t k;
            if (vi_dec(p, rec_end, &off, &k)) return -1;
            p += k;
            if (vi_dec(p, rec_end, &len, &k)) return -1;
            p += k;
            if (len > nsize - produced) return -1;
            if (len > (uint64_t)old_size) return -1;
            if (off > (uint64_t)old_size - (size_t)len) return -1;
            if (ob_put(out, old + (size_t)off, (size_t)len)) return -2;
            produced += len;
            continue;
        }

        /* unknown opcode (0x00, 0x80..0xFE, 0xFF, etc.): fail */
        return -1;
    }
    if (produced != nsize) return -1;
    (void)p;
    return 0;
}

/* Legacy v1 patch decoder.  Accepts only: header v1 + records 0x01 ADD /
 * 0x02 COPY.  Returns 0 on success, -1 format, -2 OOM. */
static int decode_v1(const uint8_t *p, const uint8_t *end,
                     const uint8_t *old, size_t old_size,
                     obuf *out, uint64_t nsize) {
    uint64_t produced = 0;
    while (produced < nsize) {
        if (p >= end) return -1;
        uint8_t op = *p++;
        if (op == 0x01) {
            uint64_t len; size_t k;
            if (vi_dec(p, end, &len, &k)) return -1;
            p += k;
            if (len > nsize - produced) return -1;
            if ((uint64_t)(end - p) < len) return -1;
            if (ob_put(out, p, (size_t)len)) return -2;
            p += (size_t)len; produced += len;
        } else if (op == 0x02) {
            uint64_t off, len; size_t k;
            if (vi_dec(p, end, &off, &k)) return -1;
            p += k;
            if (vi_dec(p, end, &len, &k)) return -1;
            p += k;
            if (len > nsize - produced) return -1;
            if (len > (uint64_t)old_size) return -1;
            if (off > (uint64_t)old_size - (size_t)len) return -1;
            if (ob_put(out, old + (size_t)off, (size_t)len)) return -2;
            produced += len;
        } else {
            return -1;
        }
    }
    if (produced != nsize) return -1;
    return 0;
}

int bdiff_patch_opts(const void *old_data, size_t old_size,
                     const void *patch,    size_t patch_size,
                     const bdiff_opts *opts_in,
                     void **out, size_t *out_len) {
    if (!out || !out_len) return BDIFF_E_BADARG;
    if (patch_size && !patch) return BDIFF_E_BADARG;
    if (old_size && !old_data) return BDIFF_E_BADARG;

    bdiff_opts opts;
    if (opts_in) opts = *opts_in;
    else         memset(&opts, 0, sizeof opts);

    if (opts.max_old_size && old_size > opts.max_old_size) return BDIFF_E_TOO_BIG;
    if (opts.max_patch_size && patch_size > opts.max_patch_size) return BDIFF_E_TOO_BIG;

    const uint8_t *p   = (const uint8_t *)patch;
    const uint8_t *end = p + patch_size;
    const uint8_t *old = (const uint8_t *)old_data;

    if ((size_t)(end - p) < 5) return BDIFF_E_FORMAT;
    if (!(p[0] == 'B' && p[1] == 'D' && p[2] == 'I' && p[3] == 'F'))
        return BDIFF_E_FORMAT;
    uint8_t version = p[4];
    if (version != BDIFF_VERSION_V1 && version != BDIFF_VERSION)
        return BDIFF_E_VERSION;
    p += 5;

    obuf o = {0, 0, 0};
    int rc = BDIFF_OK;
    uint8_t *decompressed = NULL; /* only used when v2 + envelope */

    if (version == BDIFF_VERSION_V1) {
        /* v1 header: 5 bytes (BDIF + version=1) then varint new_size,
         * then legacy 0x01/0x02 records. No flags byte. */
        uint64_t nsize; size_t k;
        if (vi_dec(p, end, &nsize, &k)) { rc = BDIFF_E_FORMAT; goto bail; }
        p += k;
        if (opts.max_new_size && nsize > (uint64_t)opts.max_new_size) {
            rc = BDIFF_E_TOO_BIG; goto bail;
        }
        int dr = decode_v1(p, end, old, old_size, &o, nsize);
        if (dr == -1) { rc = BDIFF_E_FORMAT; goto bail; }
        if (dr == -2) { rc = BDIFF_E_NOMEM;  goto bail; }
    } else {
        /* v2: 5 byte magic/version, then flags byte, then new_size. */
        if ((size_t)(end - p) < 1) { rc = BDIFF_E_FORMAT; goto bail; }
        uint8_t flags = *p++;
        uint64_t nsize; size_t k;
        if (vi_dec(p, end, &nsize, &k)) { rc = BDIFF_E_FORMAT; goto bail; }
        p += k;
        if (opts.max_new_size && nsize > (uint64_t)opts.max_new_size) {
            rc = BDIFF_E_TOO_BIG; goto bail;
        }

        const uint8_t *rec; const uint8_t *rec_end;
        if (flags & 1u) {
            /* Decompress envelope. */
            uint64_t inner_size; size_t kk;
            if (vi_dec(p, end, &inner_size, &kk)) { rc = BDIFF_E_FORMAT; goto bail; }
            p += kk;
            if (opts.max_new_size && inner_size > 256ull * 1024ull * 1024ull) {
                /* Safety cap: we don't actually know a bound so pick one. */
                rc = BDIFF_E_TOO_BIG; goto bail;
            }
#if BDIFF_ZLIB_
            size_t dc_len = (size_t)inner_size;
            if (z_unwrap(p, (size_t)(end - p), &decompressed, dc_len)) {
                rc = BDIFF_E_FORMAT; goto bail;
            }
            rec = decompressed; rec_end = decompressed + dc_len;
#else
            /* Build without zlib can't read deflate envelopes. */
            rc = BDIFF_E_FORMAT; goto bail;
#endif
        } else {
            rec = p; rec_end = end;
        }

        int dr = decode_records(rec, rec_end, old, old_size, &o, nsize);
        free(decompressed); decompressed = NULL;
        if (dr == -1) { rc = BDIFF_E_FORMAT; goto bail; }
        if (dr == -2) { rc = BDIFF_E_NOMEM;  goto bail; }
    }

    *out = o.buf; *out_len = o.len;
    return BDIFF_OK;

bail:
    free(decompressed);
    free(o.buf);
    *out = NULL; *out_len = 0;
    return rc;
}
