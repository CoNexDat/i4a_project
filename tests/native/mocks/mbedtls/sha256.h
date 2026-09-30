#pragma once
#include <stddef.h>
typedef struct { int unused; } mbedtls_sha256_context;
void mbedtls_sha256_init(mbedtls_sha256_context *ctx);
void mbedtls_sha256_free(mbedtls_sha256_context *ctx);
int mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224);
int mbedtls_sha256_update(mbedtls_sha256_context *ctx, const unsigned char *data, size_t size);
int mbedtls_sha256_finish(mbedtls_sha256_context *ctx, unsigned char digest[32]);
