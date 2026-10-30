/* mini-pi entry point. Pure C only — no Python at runtime.
 * Security model: the API key NEVER enters this binary's code or config files.
 * It lives in .env, exported into the localhost TLS broker's environment at
 * launch; providers store only the NAME of the env var (key_env). The C client
 * talks plaintext to the 127.0.0.1 broker (cloud) or directly to a loopback
 * OpenAI-compatible server (LM Studio / Ollama / llama.cpp local inference).
 * All management (connect provider, pick model, permission mode) happens in
 * the REPL/TUI; every tool call is gated by security/policy before execution. */
#define _GNU_SOURCE
#include "agent/loop.h"
#include "tools/registry.h"
#include "session/store.h"
#include "security/policy.h"
#include "config/config.h"
#include "util/paths.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--thinking] [--prompt TEXT] [--auto-approve] [--model NAME]\n"
        "             [--max-tokens N] [--budget N] [--root DIR] [--resume FILE.jsonl]\n"
        "             [--policy ask|auto-read|full-auto]\n"
        "  default: interactive REPL (streaming + thinking off).\n"
        "  --thinking     show reasoning stream (nemotron delta.reasoning_content)\n"
        "  --prompt TEXT  one-shot mode; FAILS CLOSED: tools need --auto-approve\n"
        "  --auto-approve full-auto policy for one-shot (HIGH risk still blocked)\n"
        "  keys: keep them in .env -> broker env only (see README)\n", argv0);
}

int main(int argc, char **argv) {
    int thinking = 0, auto_approve = 0;
    const char *prompt = NULL, *model_override = NULL;
    long max_tokens = 16384, budget = 120000;
    const char *resume = NULL;
    (void)getenv("MINIPI_ROOT"); /* --root handled by REPL/TUI workspace confinement */
    mp_pmode pmode = MP_MODE_ASK_ALWAYS;
    int have_policy_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--thinking")) thinking = 1;
        else if (!strcmp(argv[i], "--prompt") && i + 1 < argc) prompt = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model_override = argv[++i];
        else if (!strcmp(argv[i], "--max-tokens") && i + 1 < argc) max_tokens = atol(argv[++i]);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget = atol(argv[++i]);
        else if (!strcmp(argv[i], "--resume") && i + 1 < argc) resume = argv[++i];
        else if (!strcmp(argv[i], "--auto-approve")) auto_approve = 1;
        else if (!strcmp(argv[i], "--policy") && i + 1 < argc) {
            if (mp_mode_parse(argv[++i], &pmode)) { fprintf(stderr, "bad --policy\n"); return 2; }
            have_policy_flag = 1;
        }
        else { usage(argv[0]); return 2; }
    }

    /* Load config first: it seeds active provider/model/policy from
     * ~/.minipi/config.toml plus built-in defaults (nemotron pre-registered). */
    extern mp_config g_cfg;   /* process-wide config shared with REPL + loop routing */
    mp_cfg_load(&g_cfg, NULL);
    mp_config *cfg = &g_cfg;
    if (model_override && cfg->active[0]) {
        mp_provider *p = mp_cfg_active(cfg);
        if (p) snprintf(cfg->session_model, sizeof cfg->session_model, "%s", model_override);
    }
    if (!have_policy_flag && cfg->policy_mode[0])
        mp_mode_parse(cfg->policy_mode, &pmode);

    /* Loud session-dir diagnostics: a wrong/missing $HOME used to silently drop
     * all persistence (the rc=1 mystery). Now we print exactly where it goes. */
    const char *mdir = mp_minipi_dir();
    if (!mdir) log_warn("main: no usable home (%s) — sessions will NOT persist", mp_home_error());
    else {
        char sd[4096]; snprintf(sd, sizeof sd, "%s/sessions", mdir);
        log_info("main: session dir target: %s", sd);
    }

    /* Resume path: verify hash chain BEFORE replaying anything. */
    if (resume) {
        int v = mp_store_verify(resume);
        if (v != 0) { fprintf(stderr, "refusing to resume %s: integrity check failed (%d)\n", resume, v); return 1; }
    }

    mp_bus *bus = mp_bus_new();
    mp_agent_cfg acfg = { .model = cfg->active[0] ? (cfg->session_model[0] ? cfg->session_model : mp_cfg_active(cfg)->model)
                                                  : "offline-stub",
                          .max_turns = cfg->max_turns ? cfg->max_turns : 8,
                          .token_budget = budget };
    (void)budget; (void)pmode;
    mp_agent *agent = mp_agent_new(&acfg, bus);
    mp_toolreg *tools = mp_agent_tools(agent);
    if (!agent) { fprintf(stderr, "agent init failed\n"); return 1; }
    if (resume) {
        mp_ctx *ctx = mp_agent_ctx(agent);
        if (mp_store_replay(resume, ctx) < 0) { fprintf(stderr, "resume replay failed\n"); return 1; }
    }

    int rc;
    if (prompt) {
        /* One-shot fail-closed path uses the core loop with an approving bus
         * subscriber so nothing runs silently: without --auto-approve any
         * side-effecting tool is DENIED (interactive=0 inside the policy). */
        mp_policy *pol = mp_policy_new(auto_approve ? MP_MODE_FULL_AUTO : pmode, 0);
        rc = mp_agent_run_turn_pol(agent, prompt, pol, tools);
        /* Print the final assistant message (loop pushes it into ctx). */
        mp_ctx *cctx = mp_agent_ctx(agent);
        for (int i = cctx->n - 1; i >= 0; i--) {
            mp_msg *m = &cctx->v[i];
            if (m->role && !strcmp(m->role, "assistant") && m->content && *m->content) {
                fputs(m->content, stdout); fputc('\n', stdout); break;
            }
        }
        mp_policy_free(pol);
    } else {
        rc = mp_repl_run(agent, NULL, thinking, max_tokens, NULL);
    }
    mp_agent_free(agent);
    mp_bus_free(bus);
    return rc == -2 ? 1 : (rc < 0 ? 1 : 0);
}
