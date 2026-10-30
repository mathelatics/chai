#ifndef MINIPI_CONFIG_CONFIG_H
#define MINIPI_CONFIG_CONFIG_H
/* Provider registry + config.toml load/save. The C binary NEVER holds API key
 * values — a provider stores only the NAME of the env var (key_env) whose value
 * lives in .env -> broker process env. Local inference providers (kind=openai-
 * local, e.g. LM Studio/Ollama/llama.cpp) need no key at all and talk plaintext
 * to 127.0.0.1 only (enforced). */
#include "cJSON.h"

typedef enum { MP_KIND_NEMOTRON = 0, MP_KIND_OPENAI, MP_KIND_ANTHROPIC, MP_KIND_OPENAI_LOCAL } mp_pkind;

typedef struct {
    char name[64];
    mp_pkind kind;
    char base_url[512];   /* cloud: via broker path encoding; local: http://127.0.0.1:PORT/v1 */
    char model[192];
    char key_env[64];     /* env var NAME holding the key (broker reads it), "" if none */
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
int  mp_cfg_rm(mp_config *c, const char *name);
int  mp_cfg_use(mp_config *c, const char *name);
/* True if url is http://127.0.0.1:* or http://localhost:* (local inference). */
int  mp_url_is_loopback_http(const char *url);
#endif
