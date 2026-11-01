#ifndef MINIPI_CONFIG_CONFIG_H
#define MINIPI_CONFIG_CONFIG_H
/* Provider registry + config.toml load/save. The C binary NEVER holds API key
 * values — a provider stores only the NAME of the env var (key_env) whose value
 * lives in .env -> broker process env. Local inference providers (kind=openai-
 * local, e.g. LM Studio/Ollama/llama.cpp) need no key at all and talk plaintext
 * to 127.0.0.1 only (enforced). */
#include "cJSON.h"

typedef enum { MP_KIND_NEMOTRON = 0, MP_KIND_OPENAI, MP_KIND_ANTHROPIC, MP_KIND_OPENAI_LOCAL } mp_pkind;

/* Upstream auth style. The broker injects the header; the client only ever
 * sends the env-var NAME. `anthropic` uses x-api-key + anthropic-version. */
typedef enum { MP_AUTH_BEARER = 0, MP_AUTH_ANTHROPIC, MP_AUTH_X_API_KEY, MP_AUTH_NONE } mp_auth;

typedef struct {
    char name[64];
    mp_pkind kind;
    char base_url[512];   /* cloud: via broker path encoding; local: http://127.0.0.1:PORT/v1 */
    char model[192];
    char key_env[64];     /* env var NAME holding the key (broker reads it), "" if none */
    mp_auth auth;         /* upstream auth header style (bearer by default) */
} mp_provider;

typedef struct {
    mp_provider v[16];
    int n;
    char active[64];      /* name of active provider */
    char session_model[192]; /* /model override for the active provider */
    long token_budget;
    int max_turns;
    char policy_mode[32]; /* ask | auto-read | full-auto */
} mp_config;

const char *mp_kind_name(mp_pkind k);
int  mp_kind_parse(const char *s, mp_pkind *out);
const char *mp_auth_name(mp_auth a);
int  mp_auth_parse(const char *s, mp_auth *out);

/* Built-in provider presets (cloud + local), shared by /connect and the TUI.
 * Presets are compile-time constants; nothing here contains a key value. */
typedef struct {
    const char *id;        /* stable short id, e.g. "openai" */
    const char *label;     /* human label, e.g. "OpenAI" */
    const char *kind;      /* config kind string: openai | anthropic | nemotron | openai-local */
    const char *base_url;
    const char *model;     /* default model used until one is picked */
    const char *key_env;   /* env-var NAME, "" for local */
    mp_auth auth;
    int local;             /* 1 => local inference server, no key */
} mp_preset;

const mp_preset *mp_presets(int *count);
const mp_preset *mp_preset_find(const char *id_or_label);
/* 1-based index in the display order (cloud first, then local). */
const mp_preset *mp_preset_by_index(int num);
int  mp_preset_count_local(void);

/* Load ~/.minipi/config.toml (or explicit path). Missing file => defaults with
 * built-in 'nemotron' provider pre-registered. Returns 0 ok, -1 parse error
 * (logged; defaults still populated). */
int  mp_cfg_load(mp_config *c, const char *path_or_null);
/* Persist current providers + active + session model back to the config file
 * (atomic: write tmp + rename). Returns 0 ok. */
int  mp_cfg_save(const mp_config *c);
/* Where config lives (uses util/paths fallback order; NULL => fail loudly). */
const char *mp_cfg_default_path(void);

mp_provider *mp_cfg_active(mp_config *c);            /* NULL if none */
mp_provider *mp_cfg_find(mp_config *c, const char *name);
int  mp_cfg_add(mp_config *c, const char *name, const char *kind,
                const char *base_url, const char *model, const char *key_env);
/* Same as mp_cfg_add but chooses the upstream auth style explicitly (used by
 * the preset-driven interactive connect flow). */
int  mp_cfg_add_ex(mp_config *c, const char *name, const char *kind,
                   const char *base_url, const char *model, const char *key_env,
                   mp_auth auth);
/* Set the active provider's model (session override) + persist. */
int  mp_cfg_set_model(mp_config *c, const char *model);
int  mp_cfg_rm(mp_config *c, const char *name);
int  mp_cfg_use(mp_config *c, const char *name);
/* True if url is http://127.0.0.1:* or http://localhost:* (local inference). */
int  mp_url_is_loopback_http(const char *url);
#endif
