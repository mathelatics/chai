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

/* Full form: HTTP method + upstream auth style ("bearer"|"anthropic"|"none")
 * and the env-var NAME holding the key. Used by the provider router for both
 * chat completions and model listing. */
int mp_broker_request(const char *method, const char *path,
                      const char *body, size_t body_len,
                      const char *auth, const char *key_env,
                      char **out, size_t *out_len);

/* 1 if the broker accepts a loopback connection. */
int mp_broker_is_up(void);
/* Make sure a broker is reachable: if not, try to spawn a sibling
 * minipi-broker (MINIPI_BROKER_BIN overrides). Returns 0 if up afterwards.
 * Set MINIPI_NO_AUTOSTART=1 to disable spawning. */
int mp_broker_ensure(void);

/* Generic loopback HTTP POST/GET for local inference servers (LM Studio,
 * Ollama, llama.cpp). url must be http://127.0.0.1[:port] or http://localhost
 * — anything else returns -2 without connecting. Returns status or <0. */
int mp_local_http(const char *url_base, const char *method,
                  const char *body, size_t body_len, char **out, size_t *out_len);
#define mp_http_post_json_raw(base, body, out, olen) \
    mp_local_http((base), "POST", (body), strlen(body), (out), (olen))
#define mp_http_get_json_raw(base, out, olen) \
    mp_local_http((base), "GET", NULL, 0, (out), (olen))

/* Tell the broker which ENV VAR NAME holds the API key for this request
 * (sent as X-Minipi-Key-Env header; the value never leaves minipi's env).
 * Call before mp_broker_post(). Pass "" to clear. Thread-unsafe by design. */
void mp_broker_set_key_env(const char *env_name);
/* Upstream auth style for the legacy mp_broker_post path. */
void mp_broker_set_auth(const char *auth);

/* Cooperative cancellation hook: return non-zero to abort an in-flight read
 * (checked after EINTR), so Ctrl-C during a slow stream does not hang. */
typedef int (*mp_net_abort_fn)(void);
void mp_net_set_abort_check(mp_net_abort_fn fn);

/* Display string for the configured broker endpoint (never contains secrets). */
static inline const char *mp_broker_url(void) {
    static char b[64];
    const char *p = getenv("MINIPI_BROKER_PORT");
    snprintf(b, sizeof b, "http://127.0.0.1:%s", (p && *p) ? p : "8777");
    return b;
}
#endif
