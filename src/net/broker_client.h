#ifndef MINIPI_NET_BROKER_CLIENT_H
#define MINIPI_NET_BROKER_CLIENT_H
/* Plaintext HTTP/1.1 POST to localhost TLS broker; returns HTTP status or -1.
 * *out is malloc'd body (NUL-terminated), *out_len its length. */
#include <stddef.h>
int mp_broker_post(const char *path, const char *body, size_t body_len,
                   char **out, size_t *out_len);
#endif
