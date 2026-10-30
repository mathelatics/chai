/* config.toml provider registry: load/save + CRUD used by /connect commands. */
#include "config/config.h"
#include "util/paths.h"
#include "tomlc17.h"
#include "log.h"
#include <errno.h>
#include <unistd.h>   /* unlink */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *mp_kind_name(mp_pkind k) {
    switch (k) { case MP_KIND_OPENAI: return "openai";
                 case MP_KIND_ANTHROPIC: return "anthropic";
                 case MP_KIND_OPENAI_LOCAL: return "openai-local";
                 default: return "nemotron"; }
}
int mp_kind_parse(const char *s, mp_pkind *out) {
    if (!s) return -1;
    if (!strcmp(s, "nemotron"))  { *out = MP_KIND_NEMOTRON; return 0; }
    if (!strcmp(s, "openai"))    { *out = MP_KIND_OPENAI; return 0; }
    if (!strcmp(s, "anthropic")) { *out = MP_KIND_ANTHROPIC; return 0; }
    if (!strcmp(s, "openai-local") || !strcmp(s, "local")) { *out = MP_KIND_OPENAI_LOCAL; return 0; }
    return -1;
}

int mp_url_is_loopback_http(const char *url) {
    return url && (!strncmp(url, "http://127.0.0.1", 16) || !strncmp(url, "http://localhost", 16));
}

const char *mp_cfg_default_path(void) {
    static char p[4096];
    const char *dir = mp_minipi_dir();
    if (!dir) return NULL;
    snprintf(p, sizeof p, "%s/config.toml", dir);
    return p;
}

static void set_defaults(mp_config *c) {
    memset(c, 0, sizeof *c);
    c->token_budget = 120000; c->max_turns = 8;
    strcpy(c->policy_mode, "ask");
    mp_cfg_add(c, "nemotron", "nemotron", "https://integrate.api.nvidia.com/v1",
               "nvidia/nemotron-3.5-lightning-30b-a3b", "NVIDIA_API_KEY");
    strcpy(c->active, "nemotron");
}

static const char *tabstr(const toml_datum_t *tab, const char *key, char *buf, size_t n) {
    toml_datum_t d = toml_get(*tab, key);
    if (d.type != TOML_STRING) return NULL;
    snprintf(buf, n, "%.*s", d.u.str.len, d.u.s);
    return buf;
}

int mp_cfg_load(mp_config *c, const char *path_or_null) {
    set_defaults(c);
    char tmp[64];
    const char *em = getenv("MINIPI_MODEL");
    if (em && *em) snprintf(c->v[0].model, sizeof c->v[0].model, "%s", em);
    const char *path = path_or_null;
    static char defp[4096];
    if (!path || !*path) {
        const char *dp = mp_cfg_default_path();
        if (!dp) { log_warn("config: no HOME — using built-in defaults only (%s)", mp_home_error()); return 0; }
        snprintf(defp, sizeof defp, "%s", dp); path = defp;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) log_warn("config: cannot open %s: %s", path, strerror(errno));
        return 0; /* missing config is fine: defaults */
    }
    toml_result_t res = toml_parse_file(fp);
    fclose(fp);
    if (!res.ok) { log_error("config: %s: %s", path, res.errmsg); toml_free(res); return -1; }
    toml_datum_t top = res.toptab;
    toml_datum_t provtab = toml_get(top, "provider");
    if (provtab.type == TOML_TABLE) {
        int had_providers = 0;
        for (int i = 0; i < provtab.u.tab.size; i++) {
            toml_datum_t pv = provtab.u.tab.value[i];
            if (pv.type != TOML_TABLE) continue;
            const char *rawname = provtab.u.tab.key[i];
            if (!rawname) continue;
            char name[64];
            snprintf(name, sizeof name, "%s", rawname);
            char b1[512], b2[192], b3[64], b4[32];
            const char *bk = tabstr(&pv, "kind", b4, sizeof b4);
            const char *bu = tabstr(&pv, "base_url", b1, sizeof b1);
            const char *bm = tabstr(&pv, "model", b2, sizeof b2);
            const char *be = tabstr(&pv, "key_env", b3, sizeof b3);
            if (mp_cfg_find(c, name)) mp_cfg_rm(c, name); /* config overrides built-in default */
            if (mp_cfg_add(c, name, bk ? bk : "openai", bu ? bu : "", bm ? bm : "", be ? be : "") == 0)
                had_providers = 1;
            else log_warn("config: provider '%s' rejected (bad kind or loopback rule)", name);
        }
        if (!had_providers) { /* keep defaults */ }
    }
    char sb[64];
    if (tabstr(&top, "active", sb, sizeof sb)) snprintf(c->active, sizeof c->active, "%s", sb);
    toml_datum_t sess = toml_get(top, "session");
    if (sess.type == TOML_TABLE) {
        char b[192];
        if (tabstr(&sess, "model", b, sizeof b)) snprintf(c->session_model, sizeof c->session_model, "%s", b);
        toml_datum_t pm = toml_get(sess, "policy_mode");
        if (pm.type == TOML_STRING) snprintf(c->policy_mode, sizeof c->policy_mode, "%.*s", pm.u.str.len, pm.u.s);
    }
    toml_datum_t b = toml_get(top, "token_budget");
    if (b.type == TOML_INT64) c->token_budget = (long)b.u.int64;
    toml_datum_t mt = toml_get(top, "max_turns");
    if (mt.type == TOML_INT64 && mt.u.int64 >= 1 && mt.u.int64 <= 64) c->max_turns = (int)mt.u.int64;
    toml_free(res);
    (void)tmp;
    return 0;
}

int mp_cfg_save(const mp_config *c) {
    const char *dir = mp_minipi_dir();
    if (!dir) { log_error("config: save refused — %s", mp_home_error()); return -1; }
    if (mp_mkdir_p(dir)) { log_error("config: mkdir %s failed: %s", dir, strerror(errno)); return -1; }
    char path[4096], tmp[4200];
    snprintf(path, sizeof path, "%s/config.toml", dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { log_error("config: open %s failed: %s", tmp, strerror(errno)); return -1; }
    fprintf(f, "# MiniPi config (managed by /connect; keys are NEVER stored here,\n"
               "# only the NAME of the env var the broker reads).\n");
    mp_config *mm = (mp_config *)c;  /* find is non-mutating */
    mp_provider *act = mp_cfg_find(mm, c->active);
    fprintf(f, "active = \"%s\"\ntoken_budget = %ld\n\n[session]\nmodel = \"%s\"\npolicy_mode = \"%s\"\n\n",
            c->active, c->token_budget,
            c->session_model[0] ? c->session_model : (act ? act->model : ""),
            c->policy_mode);
    for (int i = 0; i < c->n; i++)
        fprintf(f, "[provider.%s]\nkind = \"%s\"\nbase_url = \"%s\"\nmodel = \"%s\"%s%s%s\n\n",
                c->v[i].name, mp_kind_name(c->v[i].kind), c->v[i].base_url, c->v[i].model,
                c->v[i].key_env[0] ? "\nkey_env = \"" : "", c->v[i].key_env, c->v[i].key_env[0] ? "\"" : "");
    if (ferror(f) || fclose(f)) { log_error("config: write failed"); unlink(tmp); return -1; }
    if (rename(tmp, path)) { log_error("config: rename -> %s failed: %s", path, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

mp_provider *mp_cfg_find(mp_config *c, const char *name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->v[i].name, name)) return &c->v[i];
    return NULL;
}
mp_provider *mp_cfg_active(mp_config *c) {
    mp_provider *p = mp_cfg_find(c, c->active);
    if (p && c->session_model[0]) { /* don't mutate; caller checks session_model first */ }
    return p;
}
int mp_cfg_add(mp_config *c, const char *name, const char *kind,
               const char *base_url, const char *model, const char *key_env) {
    if (!c || !name || !*name || strlen(name) > 63) return -1;
    mp_pkind k;
    if (mp_kind_parse(kind, &k)) return -2;
    if (mp_cfg_find(c, name)) return -3; /* duplicate */
    if (c->n >= 16) return -4;
    mp_provider *p = &c->v[c->n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->kind = k;
    snprintf(p->base_url, sizeof p->base_url, "%s", base_url ? base_url : "");
    snprintf(p->model, sizeof p->model, "%s", model ? model : "");
    snprintf(p->key_env, sizeof p->key_env, "%s", key_env ? key_env : "");
    if (k == MP_KIND_OPENAI_LOCAL && p->base_url[0] && !mp_url_is_loopback_http(p->base_url)) {
        log_error("config: openai-local provider '%s' rejected: base_url must be http://127.0.0.1 or http://localhost", name);
        c->n--; return -5;
    }
    return 0;
}
int mp_cfg_rm(mp_config *c, const char *name) {
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->v[i].name, name)) {
            memmove(&c->v[i], &c->v[i+1], sizeof(mp_provider) * (size_t)(c->n - i - 1));
            c->n--;
            if (!strcmp(c->active, name)) { strcpy(c->active, c->n ? c->v[0].name : ""); c->session_model[0] = 0; }
            return 0;
        }
    return -1;
}
int mp_cfg_use(mp_config *c, const char *name) {
    mp_provider *p = mp_cfg_find(c, name);
    if (!p) return -1;
    snprintf(c->active, sizeof c->active, "%s", name);
    c->session_model[0] = 0;
    return 0;
}
