#include "bdiff.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 *  Minimal patch for fixed-length data blocks.
 *
 *  Algorithm (block-aligned hash index + greedy longest match):
 *    1. Partition old data into fixed, block-aligned chunks of size B.
 *    2. Index each chunk by a 32-bit FNV-1a hash, stored in a
 *       hash-sorted array so equal hashes are contiguous.
 *    3. Scan new data left-to-right. At every position compute the
 *       hash of the B bytes ahead; on a hit, verify the bytes and
 *       pick the old offset that yields the longest forward match.
 *    4. A match of length >= B is emitted as COPY(old_off, len);
 *       otherwise the byte is accumulated into a pending ADD region.
 *    5. When a COPY is emitted, extend it backward over any pending
 *       ADD bytes that also match the old stream, shrinking the patch.
 *
 *  Sizes/offsets are LEB128 varints, so small values cost 1 byte,
 *  keeping the patch as small as possible for typical edits.
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
    return -1; /* value too long / malformed */
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

/* Longest forward match for new data at position np.
 * Returns match length (>= bs) on success and sets *out_off;
 * returns 0 when no usable match exists. */
static size_t find_longest(const entry *e, size_t ne,
                           const uint8_t *old, size_t old_size, size_t bs,
                           const uint8_t *newd, size_t new_size, size_t np,
                           size_t *out_off) {
    if (np + bs > new_size) return 0;
    uint32_t h = hash_block(newd + np, bs);

    /* binary search: first entry whose hash >= h */
    size_t lo = 0, hi = ne;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e[mid].h < h) lo = mid + 1; else hi = mid;
    }

    size_t best_len = 0, best_off = 0;
    for (size_t i = lo; i < ne && e[i].h == h; ++i) {
        size_t o = e[i].off;
        if (o + bs > old_size) continue;
        if (memcmp(old + o, newd + np, bs) != 0) continue; /* hash collision */
        size_t ext = bs;
        while (o + ext < old_size && np + ext < new_size &&
               old[o + ext] == newd[np + ext])
            ++ext;
        if (ext > best_len) { best_len = ext; best_off = o; }
    }
    if (best_len >= bs) { *out_off = best_off; return best_len; }
    return 0;
}

/* =============================== diff ============================== */
int bdiff_diff(const void *old_data, size_t old_size,
               const void *new_data, size_t new_size,
               const bdiff_opts *opts,
               void **out, size_t *out_len) {
    if (!out || !out_len) return BDIFF_E_BADARG;
    if (old_size && !old_data) return BDIFF_E_BADARG;
    if (new_size && !new_data) return BDIFF_E_BADARG;

    size_t bs = (opts && opts->block_size) ? opts->block_size : 16;
    if (bs == 0) bs = 16;

    const uint8_t *old  = (const uint8_t *)old_data;
    const uint8_t *newd = (const uint8_t *)new_data;
    obuf o = {0, 0, 0};
    entry *e = NULL;
    size_t ne = 0;

    /* header */
    static const uint8_t hdr[5] = { 'B', 'D', 'I', 'F', BDIFF_VERSION };
    if (ob_put(&o, hdr, 5)) goto oom;
    if (ob_vi(&o, (uint64_t)new_size)) goto oom;

    /* build hash index over block-aligned positions */
    if (old_size >= bs) {
        ne = old_size / bs;
        e = (entry *)malloc(ne * sizeof(entry));
        if (!e) goto oom;
        for (size_t i = 0; i < ne; ++i) {
            e[i].off = i * bs;
            e[i].h   = hash_block(old + e[i].off, bs);
        }
        qsort(e, ne, sizeof(entry), cmp_entry);
    }

    size_t np = 0, add_start = 0;
    while (np < new_size) {
        size_t off;
        size_t m = find_longest(e, ne, old, old_size, bs,
                                newd, new_size, np, &off);
        if (m >= bs) {
            /* backward extension: absorb matching pending-ADD bytes */
            size_t maxb = np - add_start;
            if (maxb > off) maxb = off;
            size_t b = 0;
            while (b < maxb && newd[np - b - 1] == old[off - b - 1]) ++b;

            size_t add_len = (np - b) - add_start;
            if (add_len) {
                if (ob_u8(&o, 0x01)) goto oom;
                if (ob_vi(&o, (uint64_t)add_len)) goto oom;
                if (ob_put(&o, newd + add_start, add_len)) goto oom;
            }
            if (ob_u8(&o, 0x02)) goto oom;
            if (ob_vi(&o, (uint64_t)(off - b))) goto oom;
            if (ob_vi(&o, (uint64_t)(m + b))) goto oom;

            np += m;
            add_start = np;
        } else {
            np++;
        }
    }
    if (np > add_start) {            /* flush trailing ADD */
        if (ob_u8(&o, 0x01)) goto oom;
        if (ob_vi(&o, (uint64_t)(np - add_start))) goto oom;
        if (ob_put(&o, newd + add_start, np - add_start)) goto oom;
    }

    free(e);
    *out = o.buf; *out_len = o.len;
    return BDIFF_OK;

oom:
    free(e);
    free(o.buf);
    *out = NULL; *out_len = 0;
    return BDIFF_E_NOMEM;
}

/* ============================== patch ============================= */
int bdiff_patch(const void *old_data, size_t old_size,
                const void *patch, size_t patch_size,
                void **out, size_t *out_len) {
    if (!out || !out_len) return BDIFF_E_BADARG;
    if (patch_size && !patch) return BDIFF_E_BADARG;
    if (old_size && !old_data) return BDIFF_E_BADARG;

    const uint8_t *p   = (const uint8_t *)patch;
    const uint8_t *end = p + patch_size;
    const uint8_t *old = (const uint8_t *)old_data;

    if (end - p < 5) return BDIFF_E_FORMAT;
    if (!(p[0] == 'B' && p[1] == 'D' && p[2] == 'I' && p[3] == 'F'))
        return BDIFF_E_FORMAT;
    if (p[4] != BDIFF_VERSION) return BDIFF_E_FORMAT;
    p += 5;

    uint64_t nsize; size_t k;
    if (vi_dec(p, end, &nsize, &k)) return BDIFF_E_FORMAT;
    p += k;

    obuf o = {0, 0, 0};
    uint64_t produced = 0;
    while (produced < nsize) {
        if (p >= end) goto fmt;
        uint8_t op = *p++;

        if (op == 0x01) {                       /* ADD */
            uint64_t len; size_t k2;
            if (vi_dec(p, end, &len, &k2)) goto fmt;
            p += k2;
            if (len > nsize - produced) goto fmt;
            if ((size_t)(end - p) < (size_t)len) goto fmt;
            if (ob_put(&o, p, (size_t)len)) goto oom;
            p += (size_t)len;
            produced += len;
        } else if (op == 0x02) {                /* COPY */
            uint64_t off, len; size_t k2;
            if (vi_dec(p, end, &off, &k2)) goto fmt;
            p += k2;
            if (vi_dec(p, end, &len, &k2)) goto fmt;
            p += k2;
            if (len > nsize - produced) goto fmt;
            if (len > old_size) goto fmt;
            if (off > old_size - len) goto fmt;        /* no overflow */
            if (ob_put(&o, old + (size_t)off, (size_t)len)) goto oom;
            produced += len;
        } else {
            goto fmt;
        }
    }
    if (produced != nsize) goto fmt;

    *out = o.buf; *out_len = o.len;
    return BDIFF_OK;

fmt:
    free(o.buf);
    *out = NULL; *out_len = 0;
    return BDIFF_E_FORMAT;
oom:
    free(o.buf);
    *out = NULL; *out_len = 0;
    return BDIFF_E_NOMEM;
}
