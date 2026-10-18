/* sim/shims/mbedtls/md.h — host 桩，只覆盖固件校验用到的 SHA-256。 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MBEDTLS_MD_SHA256 = 6,
} mbedtls_md_type_t;

typedef struct mbedtls_md_info_t {
    int type;
} mbedtls_md_info_t;

typedef struct mbedtls_md_context_t {
    int dummy;
} mbedtls_md_context_t;

void mbedtls_md_init(mbedtls_md_context_t *ctx);
void mbedtls_md_free(mbedtls_md_context_t *ctx);
const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t md_type);
int mbedtls_md_setup(mbedtls_md_context_t *ctx, const mbedtls_md_info_t *md_info, int hmac);
int mbedtls_md_starts(mbedtls_md_context_t *ctx);
int mbedtls_md_update(mbedtls_md_context_t *ctx, const unsigned char *input, size_t ilen);
int mbedtls_md_finish(mbedtls_md_context_t *ctx, unsigned char *output);

#ifdef __cplusplus
}
#endif
