#ifndef XZ_H_STUB
#define XZ_H_STUB
/* Minimal xz-embedded public API stub, ONLY for host syntax-checking the
 * .gnu_debugdata path in jni/lsparself.hpp under -DSBX_ENABLE_LSPLANT. Field
 * and signature shapes mirror the real linux/include/linux/xz.h. Never linked;
 * the real decoder is vendored by fetch_lsplant_deps.sh for device builds. */
#include <stdint.h>
#include <stddef.h>

enum xz_mode { XZ_SINGLE, XZ_PREALLOC, XZ_DYNALLOC };

enum xz_ret {
    XZ_OK, XZ_STREAM_END, XZ_UNSUPPORTED_CHECK, XZ_MEM_ERROR,
    XZ_MEMLIMIT_ERROR, XZ_FORMAT_ERROR, XZ_OPTIONS_ERROR,
    XZ_DATA_ERROR, XZ_BUF_ERROR
};

struct xz_buf {
    const uint8_t *in;
    size_t in_pos;
    size_t in_size;
    uint8_t *out;
    size_t out_pos;
    size_t out_size;
};

struct xz_dec;

struct xz_dec *xz_dec_init(enum xz_mode mode, uint32_t dict_max);
enum xz_ret xz_dec_run(struct xz_dec *s, struct xz_buf *b);
void xz_dec_end(struct xz_dec *s);
void xz_crc32_init(void);
void xz_crc64_init(void);

#endif /* XZ_H_STUB */
