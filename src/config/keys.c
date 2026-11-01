/* ~/.minipi/.env handling for keys entered interactively in the TUI.
 * File mode 0600, directory 0700. Never printed, never persisted in
 * config.toml — the broker loads it and injects the upstream auth header. */
#include "config/keys.h"
#include "util/paths.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int mp_keys_valid_env_name(const char *s) {
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '.')) return 0;
    return 1;
}

const char *mp_keys_env_path(void) {
    static char p[4096];
    const char *dir = mp_minipi_dir();
    if (!dir) return NULL;
    snprintf(p, sizeof p, "%s/.env", dir);
    return p;
}

/* Parse KEY=VALUE with optional surrounding quotes (\"...\" or '...').
 * Returns 1 on match and copies the unquoted value. */
static int parse_line(const char *line, const char *name, char *out, size_t n) {
    while (*line == ' ' || *line == '\t') line++;
    size_t nl = strlen(name);
    if (strncmp(line, name, nl) || line[nl] != '=') return 0;
    const char *v = line + nl + 1;
    char buf[2048]; size_t k = 0;
    int quote = 0;
    if (*v == '"' || *v == '\'') { quote = *v++; }
    while (*v && (quote ? *v != quote : *v != '\r' && *v != '\n') && k + 1 < sizeof buf) {
        if (quote == '"' && *v == '\\' && v[1]) v++;
        buf[k++] = *v++;
    }
    buf[k] = 0;
    if (out && n) snprintf(out, n, "%s", buf);
    return 1;
}

int mp_keys_lookup(const char *env_name, char *out, size_t out_n) {
    if (!mp_keys_valid_env_name(env_name)) return 0;
    const char *e = getenv(env_name);
    if (e && *e) { if (out && out_n) snprintf(out, out_n, "%s", e); return 1; }
    const char *path = mp_keys_env_path();
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048]; int found = 0;
    while (fgets(line, sizeof line, f))
        if (parse_line(line, env_name, out, out_n)) { found = 1; break; }
    fclose(f);
    return found;
}

/* Quote a value for the .env line if needed. */
static void emit_value(FILE *f, const char *v) {
    int need = 0;
    for (const char *p = v; *p; p++)
        if (isspace((unsigned char)*p) || *p == '#' || *p == '"' || *p == '\'' || *p == '\\') { need = 1; break; }
    if (!need) { fputs(v, f); return; }
    fputc('"', f);
    for (const char *p = v; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

int mp_keys_save(const char *env_name, const char *value) {
    if (!mp_keys_valid_env_name(env_name) || !value) return -1;
    const char *path = mp_keys_env_path();
    if (!path) { log_error("keys: cannot save %s — %s", env_name, mp_home_error()); return -1; }
    const char *dir = mp_minipi_dir();
    if (!dir || mp_mkdir_p(dir)) { log_error("keys: mkdir failed for %s", dir ? dir : "(none)"); return -1; }

    /* Load the existing file (usually small) so unrelated providers survive. */
    char *old = NULL; size_t oldn = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
        if (sz > 0 && sz < 1024 * 1024) {
            old = malloc((size_t)sz + 1);
            if (old) { oldn = fread(old, 1, (size_t)sz, in); old[oldn] = 0; }
        }
        fclose(in);
    }
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { log_error("keys: cannot write %s: %s", tmp, strerror(errno)); free(old); return -1; }
    fchmod(fileno(f), 0600);
    int replaced = 0;
    if (old) {
        char *save = NULL;
        for (char *line = strtok_r(old, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char probe[2048];
            if (!replaced && parse_line(line, env_name, probe, sizeof probe)) {
                fprintf(f, "%s=", env_name); emit_value(f, value); fputc('\n', f);
                replaced = 1;
            } else {
                fputs(line, f); fputc('\n', f);
            }
        }
    }
    if (!replaced) { fprintf(f, "%s=", env_name); emit_value(f, value); fputc('\n', f); }
    int bad = ferror(f);
    if (fclose(f) || bad) { log_error("keys: write failed for %s", tmp); unlink(tmp); free(old); return -1; }
    if (rename(tmp, path)) { log_error("keys: rename -> %s failed: %s", path, strerror(errno)); unlink(tmp); free(old); return -1; }
    chmod(path, 0600);
    free(old);
    return 0;
}
