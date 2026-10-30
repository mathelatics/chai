#ifndef MINIPI_UTIL_SHA256_H
#define MINIPI_UTIL_SHA256_H
/* FIPS 180-4 SHA-256, in-house (public-domain reference algorithm). Used for
 * session checkpoint hashes and tool-output fingerprints. */
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t st[8]; uint64_t len; unsigned char buf[64]; size_t buflen; } mp_sha256;
void mp_sha256_init(mp_sha256 *h);
void mp_sha256_update(mp_sha256 *h, const void *data, size_t n);
void mp_sha256_final(mp_sha256 *h, unsigned char out[32]);
void mp_sha256_hex(const void *data, size_t n, char out_hex[65]);
#endif
