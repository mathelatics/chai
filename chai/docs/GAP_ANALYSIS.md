# GAP_ANALYSIS — MiniPi (pure C) vs Pi packages, high-level → low-level

Reference: `rough/docs/*.md` (pi package reports) + current MiniPi code under `src/`.
Each section = what pi does → what MiniPi has today → what is missing.

---

## 1. High-level architecture comparison

**Pi** is a strict layering of small packages with one contract between each pair:

```
telemetry ─ ai (unified streaming LLM API) ─ agent-core (loop+events) ─ coding-agent (CLI app)
              chord (distributed objects/RPC) ─ durable (commit-before-show harness) ─ env (remote exec)
                                    tui / mcp / codemode / evals  (front-end, tools, sandbox, QA)
```

Key design properties MiniPi should copy:

1. **Layer purity.** `agent-core` knows nothing about terminals, disk sessions or
   providers; `ai` knows nothing about tools; `tui` knows nothing about agents. Our
   `src/tui/repl.c` currently mixes REPL parsing, provider routing, policy prompts and
   session persistence in one file — the biggest structural debt.
2. **Event protocol as the spine.** pi's loop emits
   `agent_start → turn_start → message_start/update/end → tool_execution_start/update/end
   → turn_end → agent_end`, and *everything* above it (TUI, JSON mode, RPC, durable
   storage, telemetry) is just another consumer of that stream. We have `src/agent/events.c`
   (`mp_bus`) but the REPL bypasses it and prints deltas directly from the provider
   callback; there is no canonical event vocabulary on disk or wire.
3. **Error-as-data contract.** pi's `StreamFn` must never throw: failures come back as a
   final assistant message with `stopReason:"error"|"aborted"` + `errorMessage`, keeping
   the loop total and resumable. Ours returns NULL/−1 with ad-hoc stderr prints; aborts
   are indistinguishable from failures in the session log.
4. **Context transformation hooks.** pi separates `transformContext()` (prune/inject) from
   `convertToLlm()` (filter UI-only messages) before every model call. Our `context.c`
   builds one flat message array with no hook points, so compaction/transcript/redaction
   can't be inserted cleanly.

## 2. Durability & sessions (pi `durable` + `coding-agent` sessions vs `src/session/`)

- **pi-durable**: commit-before-show. Every model request, tool call and state change is a
  committed record *first*, executed second; crash mid-turn ⇒ reopen picks up where it
  stopped; failed attempts retried deterministically; task graph + idle checks/wake()
  without per-commit scans.
- **pi-coding-agent**: sessions are JSONL whose entries form a **tree**; the active branch
  feeds the next request; fork/clone/continue from any entry; compaction inserts a summary
  entry replacing older messages.
- **MiniPi today**: append-only JSONL with sha256 chain (`store.c`), checkpoints,
  `tree.c` exists, `compact.c` exists but not wired to thresholds. No resume/fork/undo UI,
  no "commit-before-show" — if we die mid-tool-call, the session may show a completed
  assistant message whose tool never ran (or vice versa).
- **Missing**: intent-before-execute records (`tool_call_committed` written *before* the
  gate verdict/executes, result appended after); recovery replay that reconciles
  committed-without-result; tree navigation exposed to users (`/fork`, `/undo`,
  `--resume` picker); retention pruning.

## 3. Provider layer (pi `ai` vs `src/providers/` + broker)

- **pi-ai**: ~50 providers, static+dynamic catalogs, layered auth (env → credential store
  → OAuth), token/cost accounting per message, mid-session model hand-off, retry/error
  classification (e.g. recent fix: don't retry Zhipu usage-limit error 1308 — i.e. *know
  which errors are terminal*).
- **MiniPi today**: openai/anthropic/nemotron kinds + openai-local, broker holds keys,
  `/models` probe. No cost/token accounting surfaced to the user, no retry taxonomy
  (retry-everything or retry-nothing), no per-model capability metadata (context limit,
  supports-thinking, image input) — model assumptions are hardcoded per kind.
- **Missing**: usage/cost line per turn (footer + JSON event), `model_meta` cache
  (hax's catalog.json idea; pi's `.models.ts` catalogs), error taxonomy distinguishing
  rate-limit-retryable vs quota-terminal vs context-overflow (→ auto-compact hint).

## 4. TUI & user interaction (pi `tui` + `coding-agent` interactive mode vs `src/tui/`)

This is the widest gap and the user asked specifically about it.

- **pi-tui**: interchangeable main-screen/alt-screen renderers behind one interface;
  **differential rendering + CSI 2026 synchronized output** (no flicker, atomic frames);
  bracketed paste with >10-line markers; component library (Editor with undo-stack &
  kill-ring, Markdown, CancellableLoader, SelectList, inline Kitty/iTerm2 images);
  keybindings system, autocomplete providers (paths + slash commands), fuzzy match;
  OKLab theming. Crucially `TuiMainScreen` **preserves native scrollback**.
- **pi-coding-agent interactive**: chat viewport over that framework, theme system, model
  search, external editor integration.
- **MiniPi today**: `repl.c` = `fgets(4096)` single-line input, raw `fputs()` of deltas
  (terminal-corrupting control chars / partial UTF-8 pass through), thinking text
  interleaved into the answer stream, no history/completion/pickers, no spinner, no
  markdown. `render.c` links termbox2 but the agreed direction (HAX_STUDY §4, confirmed
  by pi-tui's own main-screen mode) is **in-place repaint preserving scrollback**, not a
  full-screen takeover.
- **Missing (maps to plan steps C-1…C-7)**:
  - `tui/disp`: stream-safe block writer (ctrl-strip + utf8 sanitize, newline collapse,
    display-width accounting for CJK/emoji, partial-sequence carry across chunks).
  - `tui/live_region` + `footer`: ≤N-row repaint region below committed output; spinner +
    elapsed + live tool rows; deferred show; **CSI 2026 synchronized-update bracket**
    (steal directly from pi-tui's technique).
  - `tui/input`: raw-mode line editor — arrows/home/end/word ops, multiline (Shift+Enter),
    persisted history + Ctrl-R, Tab completion of `/commands` and args, `@file` picker,
    **bracketed paste** handling for big pastes, Ctrl-C/Ctrl-D semantics.
  - `tui/picker`: type-to-filter pickers for `/model`, `/provider`, `/policy`, `/resume`.
  - Approval UX v2 (U6): risk badge + argv/cwd + diff preview, `[y/a/s/n/e]`, typed
    confirm for CRITICAL, decision recorded in the audit JSONL.
  - Interrupts: Esc/SIGINT pause-at-turn-boundary, double = interrupt-and-save, partial
    answer kept, reasoning discarded (pi's `stopReason:"aborted"` makes this representable).

## 5. Tools & execution (pi `coding-agent` tools, `codemode`, `mcp`, `env` vs `src/tools/`)

- **pi**: read/bash/edit/write/ls/find/grep (+PowerShell variant), file-mutation queue
  serializing concurrent writes, output accumulator/truncation guards applied before
  results re-enter context, project trust checks.
- **pi-codemode**: model writes a JS program orchestrating many tool calls inside a
  QuickJS sandbox; nested calls never enter the LLM context — huge token savings.
- **pi-mcp**: standalone stdio+HTTP MCP client with OAuth; coding-agent wires it in as a
  package, not core.
- **pi-env**: remote execution environments (SSH-deployed daemon, SHA-named binary,
  framed protocol) — tools act elsewhere while state stays home.
- **MiniPi today**: read/write/edit/bash with schema validation, path confinement, rlimits,
  env scrubbing, deny-lists; `mcp/client.c` exists but unwired. No ls/find/grep tools, no
  output_cap-before-model-feedback (currently whole blobs go back to the model), no
  mutation queue (single-threaded so far, fine), no codemode equivalent.
- **Missing (prioritized)**: output capping with elision marker (cheap, protects context —
  C-2); ls/find/grep (cheap, models expect them); MCP wiring behind `/mcp add` with
  **lazy schema injection** (pi/hax agree: eager schemas are context tax); codemode-style
  batching is a long-term stretch (needs a sandbox; QuickJS embeddable in C is viable but
  large — defer, note only).

## 6. Extensibility & headless interfaces (pi `coding-agent` modes)

- **pi**: print/JSON mode (JSONL event stream), RPC mode (JSONL commands over stdin/stdout,
  even extension UIs run over it), TypeScript SDK. Philosophy: no plugin ABI — compose via
  subprocesses + machine-readable streams.
- **MiniPi today**: `--prompt` one-shot only; human-readable output; nothing an
  orchestrator can consume.
- **Missing**: `--json` emitting our mp_bus events as JSONL (turn/tool_call/tool_result/
  usage/result records) — cheap and unlocks scripting; later an RPC mode is the same event
  protocol inverted (commands in, events out).

## 7. Observability (pi `telemetry`, `evals`)

- **pi-telemetry**: vendor-neutral typed event schemas; pi-evals runs whole agent sessions
  in Docker and scores behavior (does doc X lift agent performance?).
- **MiniPi today**: log.c to file; no structured telemetry; no behavioral evals.
- **Missing**: every session event already destined for JSONL doubles as telemetry; a tiny
  scenario-runner (fixed prompts + expected tool-call assertions against openai-local
  models like ollama) gives us pi-evals-style regression testing without cloud costs.

## 8. What MiniPi has that pi does NOT emphasize (keep, don't regress)

- **sha256-chained session log** (true audit trail; pi's tree sessions aren't hash-chained;
  hax has none at all).
- **Key custody architecture**: C binary never sees secrets; loopback TLS broker with host
  allow-list; Authorization stripped client-side.
- **Policy gate with risk scanner + fail-closed one-shot mode** (hax argues against gates;
  pi leans minimal; ours is a deliberate differentiator — keep, but make approvals
  non-reflexive per U6).

## 9. Low-level working order (runtime flow today → target)

Today: `main.c → config load → repl loop → fgets → complete_with_tools() → provider SSE
callback fputs → tool loop in loop.c → policy fgets prompt → tool exec → result string →
next turn → store append`. 

Target (pi-shaped): `main.c → dispatch(mode) → agent loop ⇄ mp_bus events → [disp|json|rpc
consumers]`; provider callbacks produce typed events only; tools gated via bus subscribers;
session writer commits intent-before-execution; TUI is purely an event consumer with its
own input/picker/disp modules calling into the loop via a command queue (pi's
QueueMode steering: "all"/"one-at-a-time").
