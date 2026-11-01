/* Provider router: one place that knows how each configured provider is
 * reached. Keeps repl.c and the one-shot agent loop from duplicating host
 * splitting, auth selection and wire-format dispatch. */
#include "providers/router.h"
#include "providers/provider.h"
#include "net/broker_client.h"
#include "cJSON.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

int mp_url_split_base(const char *url, char *host, size_t hn, char *path, size_t pn) {
    if (!url || !host || !path) return -1;
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *e = strchr(p, '/');
    size_t hl = e ? (size_t)(e - p) : strlen(p);
    if (!hl || hl >= hn) return -1;
    memcpy(host, p, hl); host[hl] = 0;
    /* strip :port (broker always connects on 443) */
    char *colon = strchr(host, ':');
    if (colon) *colon = 0;
    const char *pfx = e ? e : "/";
    snprintf(path, pn, "%s", pfx);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
    return 0;
}

mp_reply *mp_provider_stream(const mp_provider *prov, const char *model,
                             const mp_ctx *ctx, int enable_thinking, long max_tokens,
                             mp_stream_fn on_delta, void *ud, const cJSON *tools) {
    if (!prov) return NULL;
    const char *m = (model && *model) ? model : prov->model;
    if (prov->kind == MP_KIND_OPENAI_LOCAL) {
        /* Loopback-only; nemotron.c re-checks the URL before connecting. */
        return mp_nemotron_stream_ex(m, ctx, enable_thinking, max_tokens, 0.2, 0.95,
                                     on_delta, ud, tools, prov->base_url, NULL);
    }

    char host[256], upfx[300];
    if (mp_url_split_base(prov->base_url, host, sizeof host, upfx, sizeof upfx)) {
        log_error("provider: bad base_url for '%s': %s", prov->name, prov->base_url);
        return NULL;
    }
    if (mp_broker_ensure()) {
        log_error("provider: TLS broker not reachable on 127.0.0.1:%s and auto-start failed"
                  " (start ./build/minipi-broker manually)", getenv("MINIPI_BROKER_PORT") ? getenv("MINIPI_BROKER_PORT") : "8777");
        return NULL;
    }
    mp_broker_set_key_env(prov->key_env[0] ? prov->key_env : "");
    mp_broker_set_auth(mp_auth_name(prov->auth));

    if (prov->kind == MP_KIND_ANTHROPIC || prov->auth == MP_AUTH_ANTHROPIC) {
        return mp_anthropic_stream_ex(m, ctx, enable_thinking, max_tokens, 0.2, 0.95,
                                      on_delta, ud, tools, prov->key_env, host, upfx);
    }
    mp_nemotron_set_upstream_path(upfx);
    return mp_nemotron_stream_ex(m, ctx, enable_thinking, max_tokens, 0.2, 0.95,
                                 on_delta, ud, tools, NULL, host);
}

void mp_model_list_free(mp_model_list *l) {
    if (!l) return;
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int parse_models(const char *resp, mp_model_list *l) {
    if (!resp) return -1;
    cJSON *j = cJSON_Parse(resp);
    if (!j) return -1;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(j, "data");
    if (!cJSON_IsArray(data)) data = cJSON_GetObjectItemCaseSensitive(j, "models");
    int rc = -1;
    if (cJSON_IsArray(data)) {
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, data) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
            if (!cJSON_IsString(id)) id = cJSON_GetObjectItemCaseSensitive(m, "name");
            if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
            if (l->n == l->cap) {
                int nc = l->cap ? l->cap * 2 : 32;
                char **nv = realloc(l->v, (size_t)nc * sizeof *nv);
                if (!nv) break;
                l->v = nv; l->cap = nc;
            }
            l->v[l->n++] = strdup(id->valuestring);
        }
        rc = 0;
    }
    cJSON_Delete(j);
    return rc;
}

int mp_provider_list_models(const mp_provider *prov, mp_model_list *l) {
    if (!prov || !l) return -1;
    memset(l, 0, sizeof *l);
    char *resp = NULL; size_t rlen = 0;
    int st = -1;
    if (prov->kind == MP_KIND_OPENAI_LOCAL) {
        /* path is now part of the url_base; e.g. http://127.0.0.1:18811/v1/models */
        st = mp_local_http(prov->base_url, "GET", NULL, 0, &resp, &rlen);
    } else {
        char host[256], upfx[300];
        if (mp_url_split_base(prov->base_url, host, sizeof host, upfx, sizeof upfx)) return -1;
        if (mp_broker_ensure()) return -1;
        char path[600];
        snprintf(path, sizeof path, "/%s%s/models", host, upfx);
        st = mp_broker_request("GET", path, NULL, 0,
                               mp_auth_name(prov->auth),
                               prov->key_env[0] ? prov->key_env : "", &resp, &rlen);
    }
    if (st != 200 || !resp) {
        log_warn("models: %s returned status %d%s", prov->name, st,
                 st == 401 || st == 403 ? " (check the API key)" : "");
        free(resp);
        return -2;
    }
    int rc = parse_models(resp, l);
    free(resp);
    return rc;
}
