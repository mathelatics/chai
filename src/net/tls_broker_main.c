/* minipi-broker — pure-C TLS boundary replacing tools/tls_broker.py.
 *
 * Design (docs/ARCHITECTURE.md §7):
 *   - The `minipi` client binary stays MIT-only and speaks plaintext HTTP/1.1
 *     to this broker over 127.0.0.1 only.
 *   - This broker is the ONLY component that performs TLS (OpenSSL). It is a
 *     separate executable so the core library never links OpenSSL.
 *   - API keys live ONLY in the broker's environment. The client advertises
 *     the env-var NAME via X-Minipi-Key-Env; the broker resolves it locally,
 *     injects "Authorization: Bearer <key>" upstream, and STRIPS any
 *     client-supplied Authorization header. Values are never logged.
 *   - Upstream routing: client POSTs absolute-form URI
 *       http://127.0.0.1:<port>/<host><upstream-path>
 *     Host must match the allow-list. Everything else -> 403.
 *   - Streaming: upstream SSE chunks are relayed to the client as they
 *     arrive (chunked transfer-encoding), so tokens render live.
 *
 * Usage:  MINIPI_BROKER_PORT=8777 ./build/minipi-broker &
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

/* ------------------------------------------------------------------ utils */
static void die(const char *m) { fprintf(stderr, "broker: %s: %s\n", m, strerror(errno)); exit(1); }

static int cfg_int(const char *env, int dflt) {
    const char *v = getenv(env);
    return (v && *v) ? atoi(v) : dflt;
}

/* Env var name validation: C identifier, upper/digit/underscore, starts alpha. */
static int valid_env_name(const char *s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.')) return 0;
    return 1;
}

/* Header value sanitization: reject CR/LF (header injection guard). */
static int safe_header_value(const char *s) {
    for (const char *p = s; *p; p++) if (*p == '\r' || *p == '\n') return 0;
    return 1;
}

/* ------------------------------------------------------------- allow-list */
typedef struct { char host[128]; } allow_t;
static allow_t g_allow[64];
static int g_nallow = 0;

static int host_allowed(const char *h) {
    for (int i = 0; i < g_nallow; i++)
        if (!strcasecmp(g_allow[i].host, h)) return 1;
    return 0;
}

static void load_allowlist(void) {
    /* defaults mirror tools/tls_broker.py */
    static const char *d[] = {
        "integrate.api.nvidia.com", "api.openai.com", "api.anthropic.com",
        "generativelanguage.googleapis.com", "api.groq.com" };
    for (size_t i = 0; i < sizeof d / sizeof d[0] && g_nallow < 64; i++)
        snprintf(g_allow[g_nallow++].host, 128, "%s", d[i]);
    const char *extra = getenv("MINIPI_ALLOWED_HOSTS"); /* comma-separated */
    if (extra) {
        char buf[2048]; snprintf(buf, sizeof buf, "%s", extra);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            while (*tok == ' ') tok++;
            if (*tok && g_nallow < 64) snprintf(g_allow[g_nallow++].host, 128, "%s", tok);
        }
    }
}

/* ------------------------------------------------------------ buffered io */
typedef struct { int fd; SSL *ssl; char buf[16384]; size_t len, pos; } conn;
static int rd_fill(conn *c) {
    c->pos = 0; c->len = 0;
    ssize_t n = c->ssl ? SSL_read(c->ssl, c->buf, sizeof c->buf)
                       : (ssize_t)read(c->fd, c->buf, sizeof c->buf);
    if (n <= 0) return 0;
    c->len = (size_t)n; return 1;
}
static int rd_byte(conn *c, char *out) {
    if (c->pos >= c->len && !rd_fill(c)) return 0;
    *out = c->buf[c->pos++]; return 1;
}
/* read until \r\n\r\n into line buffer (cap-guarded). returns 0 on EOF. */
static int rd_headers(conn *c, char *hdr, size_t cap) {
    size_t n = 0; char prev = 0, p2 = 0, p3 = 0;
    for (;;) {
        char ch;
        if (!rd_byte(c, &ch)) return 0;
        if (n + 1 >= cap) return 0;
        hdr[n++] = ch; hdr[n] = 0;
        if (prev == '\r' && p2 == '\n' && p3 == '\r' && ch == '\n') break;
        p3 = p2; p2 = prev; prev = ch;
    }
    return 1;
}
static int wr_all(int fd, const char *b, size_t n) {
    size_t off = 0;
    while (off < n) { ssize_t w = write(fd, b + off, n - off); if (w <= 0) return 0; off += (size_t)w; }
    return 1;
}
static int wr_conn(conn *c, const char *b, size_t n) {
    if (c->ssl) return SSL_write(c->ssl, b, (int)n) > 0;
    return wr_all(c->fd, b, n);
}

/* --------------------------------------------------------------- openssl */
static SSL_CTX *make_client_ctx(void) {
    const SSL_METHOD *m = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(m);
    if (!ctx) { fprintf(stderr, "broker: SSL_CTX_new failed\n"); exit(1); }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* system trust store: try common ca-certificates paths */
    if (!SSL_CTX_set_default_verify_paths(ctx))
        SSL_CTX_load_verify_locations(ctx, "/etc/ssl/certs/ca-certificates.crt", NULL);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    return ctx;
}

/* connect to host:port via getaddrinfo (IPv4/IPv6), plain TCP */
static int tcp_connect(const char *host, int port) {
    char pstr[8]; snprintf(pstr, sizeof pstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, pstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ---------------------------------------------------------------- relay */
/* Copy upstream body to the client. If chunked_relay != 0, re-frame as HTTP
 * chunked encoding (for streaming SSE). Returns 0 on failure. */
static int relay_body(conn *up, int client_fd, int chunked_relay, long content_len) {
    char rbuf[16384];
    long remaining = content_len; /* -1 => until-close */
    int got_any = 0;
    for (;;) {
        ssize_t n;
        if (up->ssl) n = SSL_read(up->ssl, rbuf, sizeof rbuf);
        else        n = read(up->fd, rbuf, sizeof rbuf);
        if (n < 0) { if (errno == EINTR) continue; return got_any; }
        if (n == 0) return 1;
        got_any = 1;
        if (chunked_relay) {
            char frame[32];
            int fl = snprintf(frame, sizeof frame, "%zX\r\n", (size_t)n);
            if (!wr_all(client_fd, frame, (size_t)fl)) return 0;
            if (!wr_all(client_fd, rbuf, (size_t)n)) return 0;
            if (!wr_all(client_fd, "\r\n", 2)) return 0;
        } else {
            if (!wr_all(client_fd, rbuf, (size_t)n)) return 0;
        }
        if (remaining >= 0) { remaining -= n; if (remaining <= 0) { if (chunked_relay) wr_all(client_fd, "0\r\n\r\n", 5); return 1; } }
    }
}

/* ------------------------------------------------------- per-client serve */
#define HDR_CAP 16384
static void serve_client(int cfd) {
    conn cli = { .fd = cfd, .ssl = NULL, .len = 0, .pos = 0 };
    char hdr[HDR_CAP];
    if (!rd_headers(&cli, hdr, sizeof hdr)) return;

    /* parse request line: METHOD SP TARGET SP HTTP/1.1 */
    char method[8], target[2048];
    if (sscanf(hdr, "%7s %2047s", method, target) != 2) return;
    if (strcmp(method, "POST") && strcmp(method, "GET")) return;

    /* find header end */
    char *hend = strstr(hdr, "\r\n\r\n");
    size_t hlen = hend ? (size_t)(hend - hdr) + 4 : strlen(hdr);

    /* collect headers of interest */
    char key_env[128] = "";
    long clen = 0;
    int streaming_hint = 0;
    {
        char *p = strchr(hdr, '\n') + 1;
        while (p && p < hdr + hlen - 2 && !(p[0] == '\r' && p[1] == '\n')) {
            char *eol = strstr(p, "\r\n"); if (!eol || eol >= hdr + hlen) break;
            size_t ln = (size_t)(eol - p);
            char hv[512]; if (ln >= sizeof hv) ln = sizeof hv - 1;
            memcpy(hv, p, ln); hv[ln] = 0;
            if (!strncasecmp(hv, "X-Minipi-Key-Env:", 17)) {
                const char *v = hv + 17; while (*v == ' ') v++;
                snprintf(key_env, sizeof key_env, "%.127s", v);
            } else if (!strncasecmp(hv, "Content-Length:", 15)) {
                clen = atol(hv + 15);
            }
            p = eol + 2;
        }
    }

    /* read body (if any) exactly clen bytes from what remains in cli buffer+socket */
    char *body = NULL; size_t blen = 0;
    if (clen > 0) {
        body = malloc((size_t)clen + 1);
        size_t have = cli.len - cli.pos;              /* leftover already buffered */
        size_t need = (size_t)clen;
        if (have > need) have = need;
        memcpy(body, cli.buf + cli.pos, have);
        cli.pos += have;
        blen = have;
        while (blen < need) {
            if (!rd_fill(&cli)) break;
            size_t take = cli.len; if (blen + take > need) take = need - blen;
            memcpy(body + blen, cli.buf, take); blen += take;
        }
        body[blen] = 0;
        if ((long)blen != clen) { free(body); return; }
        if (blen && strstr(body, "\"stream\":true")) streaming_hint = 1;
    }

    /* parse target: expect http://127.0.0.1:PORT/<host><path> or /<host><path> */
    const char *tp = target;
    if (!strncmp(tp, "http://", 7)) {
        tp += 7;
        tp = strchr(tp, '/');
        if (!tp) return;
        tp++; /* skip leading slash -> now at <host>... */
    }
    char uphost[256], uppath[1800];
    const char *slash = strchr(tp, '/');
    if (slash) {
        size_t hl = (size_t)(slash - tp); if (hl >= sizeof uphost) return;
        memcpy(uphost, tp, hl); uphost[hl] = 0;
        snprintf(uppath, sizeof uppath, "%s", slash);
    } else { snprintf(uphost, sizeof uphost, "%s", tp); uppath[0] = 0; strcat(uppath, "/"); }
    /* strip :port from host if present */
    int upport = 443;
    char *colon = strchr(uphost, ':');
    if (colon) { *colon = 0; upport = atoi(colon + 1); }

    if (!host_allowed(uphost)) {
        const char *msg = "{\"error\":\"host not allowed\"}";
        char resp[256];
        int rl = snprintf(resp, sizeof resp, "HTTP/1.1 403 Forbidden\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(msg));
        wr_all(cfd, resp, (size_t)rl); wr_all(cfd, msg, strlen(msg));
        free(body); return;
    }

    /* resolve API key from broker env by NAME (never accept raw values) */
    const char *auth_hdr = NULL;
    char authval[1024];
    if (key_env[0]) {
        if (!valid_env_name(key_env)) { free(body); return; }
        const char *kv = getenv(key_env);
        if (kv && *kv && safe_header_value(kv)) {
            snprintf(authval, sizeof authval, "Bearer %s", kv);
            auth_hdr = authval;
        }
    }

    /* --- connect upstream with TLS --- */
    int ufd = tcp_connect(uphost, upport);
    if (ufd < 0) {
        char e[256]; int l = snprintf(e, sizeof e, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 24\r\nConnection: close\r\n\r\n{\"error\":\"connect failed\"}");
        wr_all(cfd, e, (size_t)l); free(body); return;
    }
    SSL_CTX *ctx = make_client_ctx();
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, ufd);
    SSL_set_tlsext_host_name(ssl, uphost);
    SSL_set1_host(ssl, uphost);
    if (SSL_connect(ssl) <= 0) {
        unsigned long e = ERR_get_error();
        char eb[512]; ERR_error_string_n(e, eb, sizeof eb);
        fprintf(stderr, "broker: TLS handshake to %s failed: %s\n", uphost, eb);
        char msg[128]; int l = snprintf(msg, sizeof msg, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 22\r\nConnection: close\r\n\r\n{\"error\":\"tls failed\"}");
        wr_all(cfd, msg, (size_t)l);
        SSL_free(ssl); SSL_CTX_free(ctx); close(ufd); free(body); return;
    }
    conn up = { .fd = ufd, .ssl = ssl, .len = 0, .pos = 0 };

    /* build upstream request (strip client Authorization; inject ours) */
    char ureq[8192];
    int ul = snprintf(ureq, sizeof ureq,
        "%s %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\nUser-Agent: minipi-broker/1.0\r\n"
        "%s%s%sContent-Type: application/json\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
        method, uppath, uphost,
        auth_hdr ? "Authorization: " : "", auth_hdr ? auth_hdr : "", auth_hdr ? "\r\n" : "",
        body ? (long)blen : 0L);
    if (ul < 0 || (size_t)ul >= sizeof ureq) { goto cleanup; }
    if (!wr_conn(&up, ureq, (size_t)ul)) goto cleanup;
    if (body && blen && !wr_conn(&up, body, blen)) goto cleanup;

    /* read upstream response headers */
    char uhdr[HDR_CAP];
    if (!rd_headers(&up, uhdr, sizeof uhdr)) goto cleanup;
    int ustatus = 0; sscanf(uhdr, "HTTP/%*s %d", &ustatus);

    /* determine framing for the client */
    long uclen = -1;
    int up_chunked = 0;
    {
        char *p = strchr(uhdr, '\n') + 1;
        while (p && *p && !(p[0] == '\r' && p[1] == '\n')) {
            char *eol = strstr(p, "\r\n"); if (!eol) break;
            size_t ln = (size_t)(eol - p);
            char hv[256]; if (ln >= sizeof hv) ln = sizeof hv - 1;
            memcpy(hv, p, ln); hv[ln] = 0;
            if (!strncasecmp(hv, "Content-Length:", 15)) uclen = atol(hv + 15);
            else if (!strncasecmp(hv, "Transfer-Encoding:", 16) && strcasestr(hv + 16, "chunked")) up_chunked = 1;
            p = eol + 2;
        }
    }
    /* If we don't know total length (upstream used close-delimiting) but we
     * want to stream to the client, re-frame as chunked. */
    int chunked_relay = (streaming_hint || up_chunked) && !(uclen >= 0 && !streaming_hint);
    if (up_chunked) {
        /* decode upstream chunked ourselves then re-frame; simplest: pass
         * through raw since our client also understands chunked. Detect by
         * skipping re-framing. */
        chunked_relay = 0; /* passthrough */
    }

    {
        char cresp[512];
        int cl = snprintf(cresp, sizeof cresp,
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n"
            "Connection: close\r\n%s\r\n",
            ustatus, (ustatus == 200) ? "OK" : (ustatus == 403) ? "Forbidden" : "Error",
            chunked_relay ? "Transfer-Encoding: chunked" : "");
        wr_all(cfd, cresp, (size_t)cl);
    }

    if (up_chunked) {
        /* Decode upstream chunked framing into a flat stream, then re-frame
         * chunked toward the client (flush per upstream chunk => live SSE). */
        char rbuf[16384];
        long total = 0;
        for (;;) {
            /* read one chunk-size line from buffered conn */
            char szline[32]; size_t sn = 0; int done = 0;
            while (sn + 1 < sizeof szline) {
                char ch; if (!rd_byte(&up, &ch)) { done = 1; break; }
                if (ch == '\r') continue;
                if (ch == '\n') break;
                szline[sn++] = ch;
            }
            if (done) break;
            szline[sn] = 0;
            long csz = strtol(szline, NULL, 16);
            if (csz <= 0) break;
            long rd = 0;
            while (rd < csz) {
                if (up.pos >= up.len && !rd_fill(&up)) { done = 1; break; }
                size_t take = up.len - up.pos;
                if (take > (size_t)(csz - rd)) take = (size_t)(csz - rd);
                if (take > sizeof rbuf) take = sizeof rbuf;
                memcpy(rbuf, up.buf + up.pos, take); up.pos += take;
                char frame[32]; int fl = snprintf(frame, sizeof frame, "%zX\r\n", take);
                if (!wr_all(cfd, frame, (size_t)fl)) { done = 1; break; }
                if (!wr_all(cfd, rbuf, take)) { done = 1; break; }
                if (!wr_all(cfd, "\r\n", 2)) { done = 1; break; }
                rd += (long)take; total += (long)take;
            }
            if (done) break;
            /* skip trailing CRLF after chunk data */
            char c1, c2; if (rd_byte(&up, &c1) && c1 == '\r') rd_byte(&up, &c2);
        }
        wr_all(cfd, "0\r\n\r\n", 5);
        (void)total;
    } else {
        relay_body(&up, cfd, chunked_relay, uclen);
    }

cleanup:
    SSL_shutdown(ssl);
    SSL_free(ssl); SSL_CTX_free(ctx); close(ufd);
    free(body);
}

/* ----------------------------------------------------------------- main */
int main(void) {
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    load_allowlist();
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

    int port = cfg_int("MINIPI_BROKER_PORT", 8777);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) die("socket");
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr); /* loopback ONLY */
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0) die("bind (127.0.0.1 only)");
    if (listen(lfd, 64) != 0) die("listen");
    fprintf(stderr, "broker: listening on http://127.0.0.1:%d (C/OpenSSL, %d allowed hosts)\n", port, g_nallow);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        pid_t pid = fork();
        if (pid == 0) { close(lfd); serve_client(cfd); close(cfd); _exit(0); }
        close(cfd);
    }
    return 0;
}
