#ifndef MINIPI_CONFIG_KEYS_H
#define MINIPI_CONFIG_KEYS_H
/* Interactive API-key handoff. The TUI collects a key and writes it to
 * ~/.minipi/.env (mode 0600); config.toml only ever stores the env-var NAME.
 * The TLS broker reads that file itself, so the key never appears in argv,
 * config files or logs, and the user never has to restart the TUI. */
#include <stddef.h>

/* Path of the key file (static buffer) or NULL when no home is usable. */
const char *mp_keys_env_path(void);
/* Create/replace ENV_NAME in the key file. Value is written quoted when it
 * contains whitespace/#/quotes. Returns 0 ok, -1 on error (logged). */
int mp_keys_save(const char *env_name, const char *value);
/* Look up a key: process env first, then the key file. Returns 1 found. */
int mp_keys_lookup(const char *env_name, char *out, size_t out_n);
/* 1 if the name is a plausible env var (upper/lower/digit/_/. , starts alpha). */
int mp_keys_valid_env_name(const char *name);
#endif
