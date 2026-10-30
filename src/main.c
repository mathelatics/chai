/* MiniPi entry point: config (tomlc17) -> logging (log.c) -> agent + TUI. */
#include "agent/loop.h"
#include "tui/app.h"
#include "providers/provider.h"
#include "session/compact.h"
#include "tomlc17.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
typedef struct { char model[128]; char base_url[512]; long token_budget; int max_turns; } minipi_cfg;
static int load_config(minipi_cfg *c, const char *path) {
    strcpy(c->model, "gpt-4o-mini"); c->base_url[0] = 0;
    c->token_budget = 120000; c->max_turns = 8;
    char cfgpath[4096];
    if (path && *path) snprintf(cfgpath, sizeof cfgpath, "%s", path);
    else snprintf(cfgpath, sizeof cfgpath, "%s/.minipi/config.toml", getenv("HOME") ? getenv("HOME") : ".");
    FILE *fp = fopen(cfgpath, "r");
    if (!fp) return 0;                       /* missing config is fine: defaults */
    toml_result_t res = toml_parse_file(fp);
    fclose(fp);
    if (!res.ok) { fprintf(stderr, "config: %s\n", res.errmsg); toml_free(res); return -1; }
    toml_datum_t top = res.toptab;
    toml_datum_t m = toml_get(top, "model");
    if (m.type == TOML_STRING) snprintf(c->model, sizeof c->model, "%.*s", m.u.str.len, m.u.s);
    toml_datum_t u = toml_get(top, "base_url");
    if (u.type == TOML_STRING) snprintf(c->base_url, sizeof c->base_url, "%.*s", u.u.str.len, u.u.s);
    toml_datum_t b = toml_get(top, "token_budget");
    if (b.type == TOML_INT64) c->token_budget = (long)b.u.int64;
    toml_datum_t mt = toml_get(top, "max_turns");
    if (mt.type == TOML_INT64 && mt.u.int64 >= 1 && mt.u.int64 <= 64) c->max_turns = (int)mt.u.int64;
    toml_free(res);
    return 0;
}
static void usage(void) {
    puts("usage: minipi [options]\n"
         "  --config PATH   TOML config (default ~/.minipi/config.toml)\n"
         "  --model NAME    override model\n"
         "  --base-url URL  dev-mode HTTP base url (http://127.0.0.1... only)\n"
         "  --help          this text\n");
}
int main(int argc, char **argv) {
    log_set_quiet(argc > 1); /* noisy logs only when flags given? keep quiet in REPL */
    minipi_cfg c; load_config(&c, NULL);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) load_config(&c, argv[++i]);
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) snprintf(c.model, sizeof c.model, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--base-url") && i + 1 < argc) snprintf(c.base_url, sizeof c.base_url, "%s", argv[++i]);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
    }
    if (*c.base_url) mp_provider_set_base_url(c.base_url);
    mp_bus *bus = mp_bus_new();
    mp_agent_cfg ac = { c.model, c.max_turns, c.token_budget };
    mp_agent *a = mp_agent_new(&ac, bus);
    if (!a) { fprintf(stderr, "agent init failed\n"); return 1; }
    const char *home = getenv("HOME") ? getenv("HOME") : "/tmp";
    char sessdir[4096]; snprintf(sessdir, sizeof sessdir, "%s/.minipi/sessions", home);
    mkdir(sessdir, 0700);
    mp_tui *tui = mp_tui_new(isatty(0));
    int rc = mp_tui_run(tui, a);
    mp_compact_if_needed(mp_agent_ctx(a), c.token_budget);
    mp_tui_free(tui); mp_agent_free(a); mp_bus_free(bus);
    return rc;
}
