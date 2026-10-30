#ifndef MINIPI_NET_BROKER_CLIENT_H
#define MINIPI_NET_BROKER_CLIENT_H
/* Plaintext HTTP/1.1 POST to localhost TLS broker; returns HTTP status or -1.
 * *out is malloc'd body (NUL-terminated), *out_len its length. */
#include <stddef.h>
#include <stdio.h>    /* snprintf for mp_broker_url */
#include <stdlib.h>   /* getenv for mp_broker_url */
#include <string.h>   /* strlen for the raw-HTTP macros below */
int mp_broker_post(const char *path, const char *body, size_t body_len,
                   char **out, size_t *out_len);

/* Generic loopback HTTP POST/GET for local inference servers (LM Studio,
 * Ollama, llama.cpp). url must be http://127.0.0.1[:port] or http://localhost
 * — anything else returns -2 without connecting. Returns status or <0. */
int mp_local_http(const char *url_base, const char *path, const char *method,
                  const char *body, size_t body_len, char **out, size_t *out_len);
#define mp_http_post_json_raw(base, path, body, out, olen) \
    mp_local_http((base), (path), "POST", (body), strlen(body), (out), (olen))
#define mp_http_get_json_raw(base, path, out, olen) \
    mp_local_http((base), (path), "GET", NULL, 0, (out), (olen))

/* Display string for the configured broker endpoint (never contains secrets). */
static inline const char *mp_broker_url(void) {
    static char b[64];
    const char *p = getenv("MINIPI_BROKER_PORT");
    snprintf(b, sizeof b, "http://127.0.0.1:%s", (p && *p) ? p : "8777");
    return b;
}
#endif
