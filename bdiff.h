#ifndef BDIFF_H
#define BDIFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Patch container format (little-endian free, uses LEB128 varints):
 *   bytes[0..3]  = magic 'B','D','I','F'
 *   byte [4]     = version (1)
 *   varint       = new_size (total bytes the patch reproduces)
 *   records*     = each record starts with a 1-byte opcode:
 *                   0x01 ADD  : varint len, then len raw bytes
 *                   0x02 COPY : varint old_off, varint len
 *                 records are consumed until produced == new_size.
 */

#define BDIFF_VERSION 1

#define BDIFF_OK           0
#define BDIFF_E_BADARG    -1   /* invalid arguments */
#define BDIFF_E_NOMEM     -2   /* allocation failure */
#define BDIFF_E_FORMAT    -3   /* patch corrupt / inconsistent with old data */

typedef struct {
    size_t block_size;   /* hash/lookup block size; 0 => default 16 */
} bdiff_opts;

/* Produce a patch transforming old_data -> new_data.
 * On success allocates *out (caller free()s) and sets *out_len.
 * opts may be NULL for defaults. Returns BDIFF_OK or a negative code. */
int bdiff_diff(const void *old_data, size_t old_size,
               const void *new_data, size_t new_size,
               const bdiff_opts *opts,
               void **out, size_t *out_len);

/* Apply patch to old_data, reconstructing new_data.
 * On success allocates *out (caller free()s) and sets *out_len.
 * Returns BDIFF_OK or a negative code (incl. BDIFF_E_FORMAT on mismatch). */
int bdiff_patch(const void *old_data, size_t old_size,
                const void *patch, size_t patch_size,
                void **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* BDIFF_H */
