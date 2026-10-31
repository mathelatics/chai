# chai — MiniPi (pure C) vs Pi (TypeScript) gap study & implementation plan

This folder is the **study + plan** for closing the gap between our pure-C agent
(**MiniPi**, source in `/workspace/src`, ~3.6k LOC) and the reference architecture we
documented from the pi repository (`/workspace/rough/pi/packages`, reports in
`/workspace/rough/docs/*.md`). Earlier passes also studied `hax` (`docs/HAX_STUDY.md`);
this round folds the **pi** findings into the same roadmap.

## Documents

| Doc | Contents |
|---|---|
| [GAP_ANALYSIS.md](GAP_ANALYSIS.md) | High-level → low-level comparison: design, runtime, TUI/user interaction, per-pi-package feature deltas against MiniPi's current code |
| [PLAN.md](PLAN.md) | Ordered, one-at-a-time implementation plan (C-0 … C-12), each step with scope, files, acceptance test, and status |

## Ground truth verified this session

- `cmake -S . -B build && cmake --build build -j` **succeeds** (after installing
  `libssl-dev` for the broker target). The stale TODO claim that "current in-flight
  edits do not compile" is **obsolete**: `mp_broker_set_key_env()` exists in
  `src/net/broker_client.c:29`.
- `ctest --test-dir build` → **2/2 passed** (`smoke_deps`, `unit_tests`).
- `--prompt` one-shot routes through `mp_agent_run_turn_pol` → live provider path
  (the offline-stub note in TODO P0-open is also obsolete; verify end-to-end anyway
  as part of C-0).
- Current user-facing layer: `src/tui/repl.c` (357 lines) — a `fgets()` loop that
  `fputs()` raw model deltas to stdout. This is the single biggest gap (see GAP §4).

## Working rule (from the user)

Implement **one step at a time**, accurately and correctly: each PLAN step must end
with a green build + tests + its own acceptance check before the next step starts.
