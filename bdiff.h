#ifndef BDIFF_H
#define BDIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Fixed-length-block minimal patch format – revision 2.
 *
 * PUBLIC CHANGE vs v1:
 *   - The library still produces VERSION 2 patches on diff, but accepts
 *     BOTH v1 (legacy 0x01 ADD / 0x02 COPY) and v2 patches when patching.
 *   - New in v2:
 *       • immediate (embedded small value) opcodes for common tiny ADD/COPY
 *       • ADD-XOR: emit xor-diff encoded with zero-run micro coder (realistic
 *         "4 bytes, 1 changed byte" drops from 6B to 3–4B)
 *       • optional deflate wrapper applied when it shrinks the payload
 *         (detected automatically by bdiff_diff if zlib is available;
 *          gate the code with BDIFF_HAVE_ZLIB=1 on the compile line)
 *       • opts gained two hard caps: max_old_size and max_new_size so a
 *         hostile/huge input fails early with BDIFF_E_TOO_BIG instead of
 *         OOMing on a constrained firmware target (0 = unlimited).
 *
 * Wire layout (same header envelope as v1, only version bumped and new
 * opcodes appended; an unextended v1 decoder will simply reject v2 by
 * checking the version byte, which is the intended behaviour):
 *
 *   bytes 0..3   magic  'B' 'D' 'I' 'F'
 *   byte 4       version  = 2
 *   byte 5       flags  bit0 = 1  ->  a deflate envelope follows the
 *                                     header:  varint inner_size, then
 *                                     raw-deflated bytes of the records
 *                                     stream.  When 0, records follow
 *                                     immediately.
 *   varint       new_size (bytes the patch reproduces; mandatory even with
 *                         deflate envelope, used for final integrity check)
 *   [if flags & 1]
 *       varint       inner_size (uncompressed size of the records stream;
 *                                  must match total produced by records)
 *       bytes[..]    zlib-compressed records stream (raw deflate windowBits=-15)
 *   [else]
 *       records* (see below)
 *
 * Records stream (v2 opcodes):
 *
 *  ADD family -------
 *   0x00             ADD_BIG : varint(len), len raw bytes
 *   0x01..0x1F       ADD_SMALL: len = opcode, 1..31 raw bytes  (IMMEDIATE)
 *   0x20             ADDX_BIG: varint(len), varint(old_off),
 *                              varint(xdiff_enc_len), xdiff-encoded bytes
 *                              xdiff bytes are decoded into xor_mask;
 *                              output = old[old_off..old_off+len-1] XOR xor_mask
 *   0x21..0x3F       ADDX_SMALL: len = (opcode&0x1F) (1..31, IMMEDIATE)
 *                              followed by varint(old_off),
 *                              varint(xdiff_enc_len), encoded bytes
 *  COPY family -------
 *   0x40             COPY_BIG : varint(off), varint(len)       (legacy v1 shape)
 *   0x41..0x5F       COPY_SMALL_LEN: len = (opcode&0x1F)+1 (1..31, IMMEDIATE)
 *                                   then varint(off)
 *   0x60..0x7F       COPY_SMALL_BOTH: off_hi = (opcode>>3)&3 (top 2 of 6),
 *                                     len    = (opcode&7)+1 (1..8),
 *                                     then 1 byte = low 8 bits of offset
 *                                     -> total offset 10 bits (0..1023),
 *                                        len 1..8, full record 2 bytes only
 *  Reserved ---------
 *   0x80..0xFE       reserved (fail-safe: unknown opcode => E_FORMAT)
 *   0xFF             reserved escape for future extensions
 *
 * xdiff encoding (used inside ADDX):
 *   A 2-bit-tag stream identical in spirit to the micro-RLE discussed,
 *   but specialised for xor-masks (the common case is long runs of 0x00
 *   interspersed with a handful of non-zero bytes):
 *
 *   tag byte: [6-bit LEN-1 (0..63)] [2-bit MODE]
 *     mode 00 = RAW(LEN+1 bytes)
 *     mode 01 = ZEROS(LEN+1)       (output LEN+1 zero bytes; no payload)
 *     mode 10 = SAME1(LEN+1, V)    (V bytes repeated; 1 payload byte V)
 *     mode 11 = RAW1(LEN+2 bytes)  (rare escape for 2..65 raw run savings)
 *   The decoder keeps a running xor index; after consuming all tags the
 *   total produced bytes must equal the declared outer len of the ADDX.
 * ====================================================================
 */

#define BDIFF_VERSION        2
#define BDIFF_VERSION_V1     1  /* still accepted on patch */

/* Error codes (superset of v1). */
#define BDIFF_OK            0
#define BDIFF_E_BADARG     -1   /* NULL / contradicting args */
#define BDIFF_E_NOMEM      -2   /* malloc/realloc failed */
#define BDIFF_E_FORMAT     -3   /* patch corrupt / unknown opcode /
                                   inner length mismatch / bad xdiff tag */
#define BDIFF_E_TOO_BIG    -4   /* old/new size exceeds the configured cap */
#define BDIFF_E_VERSION    -5   /* version byte is not 1 or 2 */

typedef struct {
    size_t block_size;      /* hash/lookup block size; 0 => default 16 */
    size_t max_old_size;    /* 0 = no cap; else reject diff/patch inputs
                               > this bound with BDIFF_E_TOO_BIG          */
    size_t max_new_size;    /* 0 = no cap; counterpart for new buffer.     */
    size_t max_patch_size;  /* 0 = no cap; if non-zero, bdiff_diff fails
                               with BDIFF_E_TOO_BIG whenever the encoded
                               patch (including any deflate envelope) would
                               exceed this bound.  bdiff_patch also rejects
                               patches larger than this bound.             */
    int    prefer_small;    /* 0 = balanced (good size, modest CPU);
                               1 = spend a little more CPU on the diff side
                               to try ADDX + deflate envelope + pick the
                               smallest encoding for each ADD.             */
} bdiff_opts;

/* Produce a patch transforming old_data -> new_data.
 *
 * On success allocates *out via malloc() and sets *out_len; the caller
 * must free(*out).
 *
 * opts may be NULL for defaults (block=16, no size caps, balanced effort).
 *
 * Returns BDIFF_OK on success or a negative BDIFF_E_* on failure.
 * On failure *out == NULL and *out_len == 0.
 */
int bdiff_diff(const void *old_data, size_t old_size,
               const void *new_data, size_t new_size,
               const bdiff_opts *opts,
               void **out, size_t *out_len);

/* Apply patch to old_data, reconstructing new_data.
 *
 * Accepts both v1 patches (legacy opcode-only ADD 0x01 / COPY 0x02) and
 * v2 patches (full opcode set above, optional deflate envelope).
 *
 * On success allocates *out via malloc() and sets *out_len; the caller
 * must free(*out).  Returns BDIFF_OK on success.
 *
 * Size caps from opts are NOT applied on the patch side; the caller that
 * wants them should pass opts to diff-side or check sizes manually.
 * (bdiff_patch does respect any positive max_old_size/max_new_size that
 * the caller passes via opts – pass NULL to keep the previous
 * unconstrained behaviour.)
 */
int bdiff_patch_opts(const void *old_data, size_t old_size,
                     const void *patch,    size_t patch_size,
                     const bdiff_opts *opts,
                     void **out, size_t *out_len);

/* Legacy-compatible convenience entry point: no extra options. */
static inline int bdiff_patch(const void *old_data, size_t old_size,
                              const void *patch,    size_t patch_size,
                              void **out, size_t *out_len) {
    return bdiff_patch_opts(old_data, old_size, patch, patch_size, NULL,
                            out, out_len);
}

#ifdef __cplusplus
}
#endif
#endif /* BDIFF_H */
