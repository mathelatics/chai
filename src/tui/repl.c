/* Headless line REPL over the streaming provider (broker or local inference).
 * All user-facing management happens HERE (per policy: the human interacts only
 * through TUI/CLI): /connect provider CRUD, /model + /models listing, /policy
 * permission modes, and per-tool-call approval prompts with risk flags.
 * Keys never touch this process — providers store env-var NAMES only; values
 * live in .env exported to the broker. */
#include "tui/app.h"
#include "tui/disp.h"
#include "tui/input.h"
#include "tui/live.h"
#include "agent/loop.h"
#include "providers/provider.h"
#include "tools/registry.h"
#include "session/store.h"
#include "session/compact.h"
#include "security/policy.h"
#include "config/config.h"
#include "util/paths.h"
#include "net/broker_client.h"
#include "cJSON.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>
#include <signal.h>

mp_config g_cfg;
static mp_policy *g_policy = NULL;
static int g_thinking = 0;
static long g_max_tokens = 16384;
static mp_store *g_store = NULL;
static mp_editor *g_ed = NULL;         /* C-4 raw-mode line editor */
static volatile sig_atomic_t g_int_now = 0;   /* SIGINT during a live turn */

/* While the provider streams, SIGINT must not kill the REPL: it flags an
 * interrupt and we report stop="aborted" (error-as-data, pi behaviour). The
 * editor installs its own handler only while waiting for input. */
static void on_turn_sigint(int s){ (void)s; g_int_now = 1; }

/* Read one answer line from the user through the editor (falls back to a
 * plain fgets when stdin is not a tty — mp_ed_readline handles both). */
static int ask_line(const char *prompt, char *out, size_t outsz) {
    if (g_ed) {
        char *res = NULL;
        mp_ed_action a = mp_ed_readline(g_ed, prompt, &res);
        if (a != MP_ED_SUBMIT || !res) { free(res); return -1; }
        snprintf(out, outsz, "%s", res);
        free(res);
        return 0;
    }
    fputs(prompt, stdout); fflush(stdout);
    if (!fgets(out, (int)outsz, stdin)) return -1;
    out[strcspn(out, "\r\n")] = 0;
    return 0;
}

typedef struct { int thinking; } rctx;
static void on_delta(const char *reasoning, const char *content, void *p) {
    (void)p;
    /* C-1: every streamed byte goes through the sanitizer; reasoning is a
     * separate dimmed stream from the answer. */
    if (reasoning && *reasoning) mp_disp_write(MP_DISP_REASONING, reasoning, SIZE_MAX);
    if (content && *content)    mp_disp_write(MP_DISP_ANSWER, content, SIZE_MAX);
}

/* Host part of a cloud base_url for broker path encoding. */
static int host_of(const char *url, char *out, size_t n) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (!len || len >= n) return -1;
    memcpy(out, p, len); out[len] = '\0';
    return 0;
}

static mp_reply *complete_with_tools(mp_ctx *ctx, mp_toolreg *tools) {
    mp_provider *prov = mp_cfg_active(&g_cfg);
    if (!prov) { fprintf(stderr, "[no provider connected — use /connect add]\n"); return NULL; }
    const char *model = g_cfg.session_model[0] ? g_cfg.session_model : prov->model;
    cJSON *schemas = tools ? mp_toolreg_schemas(tools) : NULL;
    char host[256]; const char *local_base = NULL, *broker_host = NULL;
    char key_hdr[64];
    snprintf(key_hdr, sizeof key_hdr, "%s",
             prov->key_env[0] ? prov->key_env : "NVIDIA_API_KEY");
    if (prov->kind == MP_KIND_OPENAI_LOCAL) {
        local_base = prov->base_url;   /* loopback enforced deeper too */
    } else {
        if (host_of(prov->base_url, host, sizeof host)) {
            snprintf(host, sizeof host, "%s", "integrate.api.nvidia.com");
        }
        broker_host = host;
        /* path component of base_url (e.g. /v1) -> chat/completions appended by nemotron.c */
        const char *pp = strstr(prov->base_url, "://");
        if (pp) { pp += 3; const char *sl = strchr(pp, '/');
                  mp_nemotron_set_upstream_path(sl ? sl : "/v1"); }
        else mp_nemotron_set_upstream_path("/v1");
    }
    mp_broker_set_key_env(key_hdr);   /* tells broker which env var holds the key */
    rctx rc = { g_thinking };
    mp_reply *r = mp_nemotron_stream_ex(model, ctx, g_thinking, g_max_tokens, 0.2, 0.95,
                                        on_delta, &rc, schemas, local_base, broker_host);
    if (schemas) cJSON_Delete(schemas);
    /* terminate both streams cleanly (flush partial multibyte, final newline) */
    mp_disp_end(MP_DISP_REASONING);
    mp_disp_end(MP_DISP_ANSWER);
    return r;
}

/* ---- slash commands ------------------------------------------------------ */
static void cmd_connect(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "list")) {
        mp_provider *act = mp_cfg_active(&g_cfg);
        printf("providers (%d), active: %s\n", g_cfg.n, g_cfg.active[0] ? g_cfg.active : "(none)");
        for (int i = 0; i < g_cfg.n; i++)
            printf("  %s%-14s %-13s %s  model=%s  key_env=%s%s\n",
                   (act == &g_cfg.v[i]) ? "* " : "  ", g_cfg.v[i].name,
                   mp_kind_name(g_cfg.v[i].kind), g_cfg.v[i].base_url, g_cfg.v[i].model,
                   g_cfg.v[i].key_env[0] ? g_cfg.v[i].key_env : "(none)", "");
        if (argc < 2)
            printf("usage: /connect list | add <name> <kind> <base_url> <model> [KEY_ENV_NAME]"
                   " | rm <name> | use <name>\n"
                   "  kinds: nemotron | openai | anthropic | openai-local (LM Studio/Ollama/llama.cpp)\n"
                   "  NOTE: last arg is the ENV VAR NAME whose value the broker holds; keys are never stored.\n");
        return;
    }
    if (!strcmp(argv[1], "add") && argc >= 6) {
        const char *ke = argc >= 7 ? argv[6] : "";
        int rc = mp_cfg_add(&g_cfg, argv[2], argv[3], argv[4], argv[5], ke);
        if (rc) { printf("add failed (code %d: dup/bad kind/bad url/full)\n", rc); return; }
        mp_cfg_use(&g_cfg, argv[2]);
        mp_cfg_save(&g_cfg);
        printf("added + activated '%s'. Cloud kinds need the broker running with %s exported;"
               " openai-local talks directly to your loopback server.\n",
               argv[2], ke && *ke ? ke : "the provider's key env");
        return;
    }
    if (!strcmp(argv[1], "rm") && argc >= 3) {
        if (mp_cfg_rm(&g_cfg, argv[2])) { printf("no such provider\n"); return; }
        mp_cfg_save(&g_cfg); printf("removed %s\n", argv[2]); return;
    }
    if (!strcmp(argv[1], "use") && argc >= 3) {
        if (mp_cfg_use(&g_cfg, argv[2])) { printf("no such provider\n"); return; }
        mp_cfg_save(&g_cfg);
        mp_provider *p = mp_cfg_active(&g_cfg);
        printf("active provider: %s (%s) model=%s\n", p->name, mp_kind_name(p->kind), p->model);
        return;
    }
    printf("bad /connect usage\n");
}

static void cmd_models(void) {
    mp_provider *prov = mp_cfg_active(&g_cfg);
    if (!prov) { printf("no active provider\n"); return; }
    char *resp = NULL; size_t rl = 0; int st = -1;
    if (prov->kind == MP_KIND_OPENAI_LOCAL) {
        st = mp_http_get_json_raw(prov->base_url, "/models", &resp, &rl);
    } else {
        char host[256];
        if (host_of(prov->base_url, host, sizeof host) == 0) {
            char path[320]; snprintf(path, sizeof path, "/%s/v1/models", host);
            st = mp_broker_post(path, "", 0, &resp, &rl);
        }
    }
    if (st != 200 || !resp) {
        printf("model list unavailable (status %d)%s — configured default: %s\n", st,
               st == -1 ? " (is the broker/local server up? GET /models may also be unsupported)" : "",
               prov->model);
        free(resp); return;
    }
    cJSON *j = cJSON_Parse(resp); free(resp);
    const cJSON *data = j ? cJSON_GetObjectItemCaseSensitive(j, "data") : NULL;
    if (cJSON_IsArray(data)) {
        printf("models from %s:\n", prov->name);
        const cJSON *m = NULL;
        cJSON_ArrayForEach(m, data) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
            if (cJSON_IsString(id)) printf("  %s\n", id->valuestring);
        }
    } else { printf("(no 'data' array in response)\n"); }
    if (j) cJSON_Delete(j);
}

static void cmd_policy(int argc, char **argv) {
    if (argc >= 2) {
        mp_pmode m;
        if (mp_mode_parse(argv[1], &m)) { printf("modes: ask | auto-read | full-auto\n"); return; }
        if (m == MP_MODE_FULL_AUTO)
            printf("WARNING: full-auto runs LOW/MED actions without asking. HIGH-risk stays blocked.\n");
        mp_policy_set_mode(g_policy, m);
        snprintf(g_cfg.policy_mode, sizeof g_cfg.policy_mode, "%s", argv[1]);
        mp_cfg_save(&g_cfg);
    }
    printf("permission mode: %s\n", mp_mode_name(mp_policy_mode(g_policy)));
}

static int handle_slash(char *line, mp_agent *agent) {
    (void)agent;
    char *argv[8]; int argc = 0;
    for (char *t = strtok(line, " \t\n"); t && argc < 8; t = strtok(NULL, " \t\n")) argv[argc++] = t;
    if (!argc) return 0;
    if (!strcmp(argv[0], "/connect")) { cmd_connect(argc, argv); return 0; }
    if (!strcmp(argv[0], "/models"))  { cmd_models(); return 0; }
    if (!strcmp(argv[0], "/provider")){ cmd_connect(2, (char*[]){"/connect","list"}); return 0; }
    if (!strcmp(argv[0], "/policy"))  { cmd_policy(argc, argv); return 0; }
    if (!strcmp(argv[0], "/model")) {
        if (argc >= 2) {
            snprintf(g_cfg.session_model, sizeof g_cfg.session_model, "%s", argv[1]);
            mp_cfg_save(&g_cfg);
            printf("model set to: %s\n", argv[1]);
        } else {
            mp_provider *p = mp_cfg_active(&g_cfg);
            printf("current: %s (try /models for a live list)\n",
                   g_cfg.session_model[0] ? g_cfg.session_model : (p ? p->model : "?"));
        }
        return 0;
    }
    if (!strcmp(argv[0], "/help")) {
        printf("commands:\n"
               "  /connect list|add <name> <kind> <base_url> <model> [KEY_ENV]|rm <n>|use <n>\n"
               "  /model [name]      show/set model        /models  live model list\n"
               "  /policy [ask|auto-read|full-auto]       permission mode\n"
               "  /thinking          toggle reasoning stream\n"
               "  /approve           list session 'always allow' grants info\n"
               "  /quit              exit\n"
               "every tool call asks for permission (y/a/n/d); HIGH-risk is flagged.\n");
        return 0;
    }
    if (!strcmp(argv[0], "/thinking")) { g_thinking = !g_thinking; mp_disp_set_reasoning_enabled(g_thinking); printf("thinking: %s\n", g_thinking ? "on" : "off"); return 0; }
    if (!strcmp(argv[0], "/quit") || !strcmp(argv[0], "/exit")) return 1;
    printf("unknown command %s (try /help)\n", argv[0]);
    return 0;
}

/* Ask the human about one pending tool call. Returns verdict after recording.
 * deny_note (if non-NULL buffer) receives the "deny+note" message for the model.
 * While a prompt is open we temporarily restore canonical-mode SIGINT handling
 * (default action): Ctrl-C at the y/a/n/d question cancels the approval and
 * denies the call, instead of only setting the turn-abort flag. */
static mp_verdict approve_call(const char *name, const cJSON *args,
                               char *deny_note, size_t dn_sz) {
    mp_risk risk = MP_RISK_NONE; const char *cat = "none";
    char preview[2048];
    mp_verdict v = mp_policy_check(g_policy, name, args, preview, sizeof preview, &risk, &cat);
    if (v != MP_NEED_PROMPT) return v;
    struct sigaction sa, old; memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL; sigaction(SIGINT, &sa, &old);
    printf("\n\033[33m[%s risk:%s%s] agent wants to run: %s\033[0m\n"
           "  allow? (y)once (a)always-this-tool (n)deny (d)deny+note: ",
           name, mp_risk_name(risk), strcmp(cat,"none") ? cat : "", preview);
    fflush(stdout);
    char ans[128] = "";
    int got = ask_line("", ans, sizeof ans);
    sigaction(SIGINT, &old, NULL);
    if (got) { mp_policy_record_answer(g_policy, name, 'n'); return MP_DENY; }
    char c = (char)tolower((unsigned char)ans[0]);
    if (c == 'd') {
        char note[512] = "";
        if (ask_line("  note to model: ", note, sizeof note) < 0) note[0] = 0;
        mp_policy_record_answer(g_policy, name, 'n');
        if (deny_note && dn_sz)
            snprintf(deny_note, dn_sz, "user denied this action (%s: %s). Reason: %s",
                     mp_risk_name(risk), cat, note);
        return MP_DENY;
    }
    mp_policy_record_answer(g_policy, name, (c=='y'||c=='a') ? c : 'n');
    if (c == 'y' || c == 'a') {
        if (g_store) { char meta[256]; snprintf(meta, sizeof meta, "{\"approved\":\"%c\",\"risk\":\"%s\",\"cat\":\"%s\"}", c, mp_risk_name(risk), cat); mp_store_append(g_store, "approval", name, NULL, meta); }
        return MP_ALLOW;
    }
    if (g_store) mp_store_append(g_store, "denial", name, NULL, "{\"approved\":\"n\"}");
    return MP_DENY;
}

/* One user turn with tool loop + permission gates. Returns 0 ok. */
static int repl_turn(mp_agent *agent, const char *text) {
    mp_bus *bus = mp_agent_bus(agent);
    g_int_now = 0;
    struct sigaction sa, old; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_turn_sigint; sigaction(SIGINT, &sa, &old);
    mp_event ev = {MP_EV_USER_MSG, text, NULL}; mp_bus_emit(bus, &ev);
    mp_ctx_push(mp_agent_ctx(agent), "user", text, NULL);
    if (g_store) mp_store_append(g_store, "user", text, NULL, NULL);

    /* flag harmful user input up front (audit + visible warning) */
    const char *ucat = "none";
    mp_risk ur = mp_policy_scan(text, &ucat);
    if (ur >= MP_RISK_MED)
        printf("[policy] flag: user input matches %s (risk %s) — tools will require explicit approval\n",
               ucat, mp_risk_name(ur));

    mp_toolreg *tools = mp_agent_tools(agent);
    int rc = 0; char stop[32] = "stop";
    long tot_in = 0, tot_out = 0; int turns_used = 0;
    for (int turn = 0; turn < g_cfg.max_turns; turn++) {
        if (g_int_now) { snprintf(stop, sizeof stop, "aborted"); break; }  /* C-8 */
        mp_live_update(turn ? "continuing…" : "thinking…");                /* C-3 */
        mp_reply *r = complete_with_tools(mp_agent_ctx(agent), tools);
        if (!r) { mp_live_stop(); fprintf(stderr, "[provider] completion failed (transport or HTTP error)\n");
                  snprintf(stop, sizeof stop, "error"); rc = -2; break; }
        turns_used++;
        /* C-5/C-6: per-turn token accounting through the event spine */
        tot_in += r->input_tokens; tot_out += r->output_tokens;
        mp_usage u = { r->input_tokens, r->output_tokens, 0.0 };
        mp_event uev = {MP_EV_USAGE, NULL, &u}; mp_bus_emit(bus, &uev);
        if (r->tool_calls) {
            const cJSON *tc = NULL;
            cJSON_ArrayForEach(tc, r->tool_calls) {
                if (g_int_now) { snprintf(stop, sizeof stop, "aborted"); break; }
                const cJSON *nj = cJSON_GetObjectItemCaseSensitive(tc, "name");
                const cJSON *aj = cJSON_GetObjectItemCaseSensitive(tc, "arguments");
                const char *name = cJSON_IsString(nj) ? nj->valuestring : "";
                const cJSON *idj = cJSON_GetObjectItemCaseSensitive(tc, "id");
                char tcid[80]; snprintf(tcid, sizeof tcid, "%s", cJSON_IsString(idj) ? idj->valuestring : "call_0");
                mp_event tcev = {MP_EV_TOOL_CALL, name, aj}; mp_bus_emit(bus, &tcev);
                mp_live_update("awaiting approval");   /* prompt goes above live region */
                char denybuf[768] = "";
                mp_verdict v = approve_call(name, aj, denybuf, sizeof denybuf);
                if (v == MP_DENY) {
                    if (!denybuf[0])
                        snprintf(denybuf, sizeof denybuf,
                                 "user DENIED this tool call. Do not retry it unchanged.");
                    mp_event rev = {MP_EV_TOOL_RESULT, denybuf, NULL}; mp_bus_emit(bus, &rev);
                    cJSON *idwrap = cJSON_CreateObject();
                    cJSON_AddStringToObject(idwrap, "id", tcid);
                    mp_ctx_push(mp_agent_ctx(agent), "assistant", "", cJSON_Duplicate(tc, 1));
                    mp_ctx_push(mp_agent_ctx(agent), "tool", denybuf, idwrap);
                    if (g_store) mp_store_append(g_store, "denial", name, NULL, NULL);
                    continue;
                }
                char live[96]; snprintf(live, sizeof live, "running %s…", name);
                mp_live_update(name && *name ? live : "running tool…");
                char *res = mp_tool_execute(tools, name, aj);
                mp_event rev = {MP_EV_TOOL_RESULT, res, NULL}; mp_bus_emit(bus, &rev);
                {   /* echo tool output through the sanitizer (C-1) */
                    char hdr[128];
                    int hl = snprintf(hdr, sizeof hdr, "[tool %s] ", name);
                    mp_disp_write(MP_DISP_TOOL_OUT, hdr, (size_t)hl);
                    mp_disp_write(MP_DISP_TOOL_OUT, res ? res : "(error)", SIZE_MAX);
                    mp_disp_end(MP_DISP_TOOL_OUT);
                }
                cJSON *idwrap = cJSON_CreateObject();
                cJSON_AddStringToObject(idwrap, "id", tcid);
                mp_ctx_push(mp_agent_ctx(agent), "assistant", "", cJSON_Duplicate(tc, 1));
                mp_ctx_push(mp_agent_ctx(agent), "tool", res ? res : "(error)", idwrap);
                if (g_store) { char *tcs = cJSON_PrintUnformatted(tc); mp_store_append(g_store, "tool", res ? res : "(error)", tcs, NULL); free(tcs); }
                free(res);
            }
            if (r->text && *r->text) mp_ctx_push(mp_agent_ctx(agent), "assistant", r->text, NULL);
        } else {
            mp_ctx_push(mp_agent_ctx(agent), "assistant", r->text ? r->text : "", NULL);
            if (g_store) mp_store_append(g_store, "assistant", r->text ? r->text : "", NULL, NULL);
            mp_event dn = {MP_EV_ASSISTANT_DONE, r->text, NULL}; mp_bus_emit(bus, &dn);
            if (r->stop_reason[0]) snprintf(stop, sizeof stop, "%s", r->stop_reason);
            mp_reply_free(r);
            break;
        }
        mp_event dn = {MP_EV_ASSISTANT_DONE, r->text, NULL}; mp_bus_emit(bus, &dn);
        if (r->stop_reason[0]) snprintf(stop, sizeof stop, "%s", r->stop_reason);
        mp_reply_free(r);
    }
    mp_compact_if_needed(mp_agent_ctx(agent), g_cfg.token_budget);
    if (g_store) mp_store_checkpoint(g_store);
    mp_event te = {MP_EV_TURN_END, NULL, NULL}; mp_bus_emit(bus, &te);
    mp_run_stats st; memset(&st, 0, sizeof st);
    st.input_tokens = tot_in; st.output_tokens = tot_out; st.turns = turns_used;
    snprintf(st.stop, sizeof st.stop, "%s", stop);
    mp_event ae = {MP_EV_AGENT_END, stop, &st}; mp_bus_emit(bus, &ae);
    if (mp_live_active()) {
        char fin[96]; snprintf(fin, sizeof fin, "done (%d turn%s, +%ld in / +%ld tok) [%s]",
                               turns_used, turns_used == 1 ? "" : "s", tot_in, tot_out, stop);
        mp_live_update(fin);
        mp_live_tick();
        mp_live_stop();
    }
    sigaction(SIGINT, &old, NULL);
    return rc;
}

int mp_repl_run(mp_agent *agent, const char *model, int thinking, long max_tokens,
                const char *session_dir) {
    g_thinking = thinking;
    mp_disp_set_reasoning_enabled(thinking);
    g_max_tokens = max_tokens > 0 ? max_tokens : 16384;
    mp_cfg_load(&g_cfg, NULL);
    if (model && *model) snprintf(g_cfg.session_model, sizeof g_cfg.session_model, "%s", model);
    mp_pmode pm = MP_MODE_ASK_ALWAYS;
    mp_mode_parse(g_cfg.policy_mode, &pm);
    g_policy = mp_policy_new(pm, isatty(STDIN_FILENO));

    /* session dir: explicit arg > MINIPI_HOME/HOME/.minipi/sessions — loud on failure */
    char sessdir[4096];
    const char *mdir = session_dir;
    if (!mdir || !*mdir) {
        const char *base = mp_minipi_dir();
        if (!base) {
            fprintf(stderr, "[session] NOT persisted: %s (set HOME or MINIPI_HOME)\n", mp_home_error());
        } else {
            snprintf(sessdir, sizeof sessdir, "%s/sessions", base);
            mdir = sessdir;
        }
    }
    if (mdir && *mdir) {
        g_store = mp_store_open(mdir, "repl-current");
        if (g_store) printf("[session] persisting to %s\n", mp_store_path(g_store));
        else fprintf(stderr, "[session] NOT persisted (see log_error above)\n");
    }

    /* C-4: raw-mode line editor with history (falls back to plain reads on
     * pipes). History lives next to the sessions under ~/.minipi. */
    {
        char hf[4096];
        const char *base = mp_minipi_dir();
        if (base) snprintf(hf, sizeof hf, "%s/history", base);
        else hf[0] = 0;
        g_ed = mp_ed_new(hf[0] ? hf : NULL);
    }

    printf("mini-pi REPL — broker at %s | provider=%s model=%s | policy=%s\n"
           "type /help, then anything else goes to the model. Ctrl-D quits.\n",
           mp_broker_url(), g_cfg.active,
           g_cfg.session_model[0] ? g_cfg.session_model : (mp_cfg_active(&g_cfg) ? mp_cfg_active(&g_cfg)->model : "?"),
           mp_mode_name(mp_policy_mode(g_policy)));
    int quit = 0;
    for (;;) {
        char *line = NULL;
        mp_ed_action a = g_ed ? mp_ed_readline(g_ed, "> ", &line)
                              : (fgets(line = calloc(1, 8192), 8192, stdin)
                                 ? (line[strcspn(line, "\r\n")] = 0, MP_ED_SUBMIT) : MP_ED_QUIT);
        if (a == MP_ED_QUIT) { free(line); break; }
        if (a == MP_ED_INTERRUPT) { free(line); continue; }   /* idle ^C: just clear */
        if (!line || !*line) { free(line); continue; }
        if (g_ed) mp_ed_hist_add(g_ed, line);
        if (line[0] == '/') { quit = handle_slash(line, agent); free(line); if (quit) break; continue; }
        repl_turn(agent, line);
        free(line);
    }
    if (g_ed) { mp_ed_hist_save(g_ed); mp_ed_free(g_ed); g_ed = NULL; }
    if (g_store) { mp_store_checkpoint(g_store); mp_store_close(g_store); g_store = NULL; }
    mp_policy_free(g_policy); g_policy = NULL;
    return 0;
}
