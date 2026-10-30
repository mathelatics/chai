# MiniPi — Living TODO

## P0 — session-dir bug (explains last rc=1)  [DONE this pass]
- [x] `mp_home_dir()` helper: `MINIPI_HOME` > `$HOME` (must be absolute & non-empty) > fail loudly with errno log. No silent `/tmp`.
- [x] mkdir() failure now logs path + errno (`strerror`); store-open failure logged, session continues in-memory but user sees the reason.
- [x] `store.c`: `fopen` failure logs errno + full path; parent dir created recursively (mkdir -p semantics).
- [x] Verified round-trip: JSONL written, replay count matches, sha256 checkpoint stable across reopen (appends continue chain via replay-hash reseed).

## P1 — provider/connection management from TUI/CLI (pure C)  [DONE]
- [x] `src/config/config.c`: provider registry parsed from config.toml `[provider.<name>]` blocks (kind/base_url/model/key_env), active provider persisted under `[session]`.
- [x] REPL slash commands: `/connect list|add <kind> <base_url> <model> [key_env]|rm <name>|use <name>`, `/model [name]`, `/models` (live GET /v1/models via broker), `/provider` shows current.
- [x] Key custody unchanged: only the *name* of the key env var is stored; the value lives in `.env` → broker process env only. The C binary never reads or embeds keys.
- [x] Local OpenAI-compatible inference supported first-class: kind=openai-local bypasses the broker (plaintext to 127.0.0.1 ONLY, port-guarded like net/http dev mode) → LM Studio :1234, Ollama :11434/v1, llama.cpp :8080.

## P2 — tool-call plumbing actually working  [DONE]
- [x] `mp_nemotron_stream()` now sends `tools: mp_toolreg_schemas()` (OpenAI function format) + `tool_choice:auto` when a registry is passed.
- [x] Tool results fed back as proper `role:"tool"` messages with `tool_call_id` (was folded into user text).
- [x] nemotron.c tool accumulator de-globaled (per-call state on stack).

## P3 — permission gates + harmful flagging  [DONE]
- [x] `src/security/policy.c`: risk scanner (destructive shell, privesc, curl|sh, secrets paths, exfil URLs, prompt-injection phrases) → level+category per action/user-input.
- [x] Every tool call requires explicit user approval in REPL (y/a/n/d). Denial returned to model as tool result so it adapts.
- [x] Modes: ask-always (default) / auto-read / full-auto (`--auto-approve`, HIGH-risk still blocked).
- [x] `--prompt` one-shot fails closed (no tools run) unless `--auto-approve`.
- [x] bash child env scrubbed of `*_API_KEY`/`*_TOKEN`/`*_SECRET`; deny-list hardened (fork bomb, dd of=, chmod 777 /, shutdown/reboot).
- [x] Approval/denial/blocked events persisted to session JSONL as audit trail.

## P4 — security posture notes  [DONE]
- [x] README "Security state" section documents: key in .env→broker env only; broker binds 127.0.0.1; host allow-list; Authorization stripped from client; no logging.
- [!] EXPOSED KEY: rotate/delete the NVIDIA key that appeared in chat history BEFORE further use. Until rotated treat it as public. Added `.env.example` (placeholders only) and key-rotation checklist in README.

## Open from last pass (P0 of the UX plan — must land before anything else)
- [ ] Implement `mp_broker_set_key_env()` in C **and** the matching header handling in
      `tools/tls_broker.py`; current in-flight edits do not compile. Rebuild + full test pass.
- [ ] Route `--prompt` one-shot through the live provider (not the offline stub); answer→stdout,
      banner/usage/errors/resume-hint→stderr.
- [ ] Live end-to-end run with `.env` key: connect → pick model → chat → tool call w/ approval →
      verify session JSONL + sha256 chain on disk.

## UX roadmap — derived from docs/HAX_STUDY.md (study of rough/hax, MIT, ~45k LOC C agent)
- [ ] U1 stream-safe output: `util/utf8_sanitize` + `tui/ctrl_strip` on ALL deltas/tool output;
      reasoning out of the answer stream; `tui/disp` newline collapsing. Biggest perceived win.
- [ ] U2 streaming Markdown (headings/bullets/fences, tables later) + diff-colored edit previews;
      `output_cap` semantics applied BEFORE results go back to the model.
- [ ] U3 in-place live footer: spinner + elapsed timer + ≤8 rows live tool output, deferred show.
      **REPLACES the termbox2 full-screen plan** (see HAX_STUDY §4: alt-buffer loses scrollback,
      fights tmux, breaks copy-mode; hax proves in-place repaint is the better UX for a small C bin).
- [ ] U4 real line editor (`tui/input`): arrows/home/end/word-ops, multiline, per-cwd history file,
      Ctrl-R search, Tab completion of `/commands`+args, `@file` picker, Ctrl-L/Ctrl-C/Ctrl-D.
- [ ] U5 type-to-filter pickers for `/provider`, `/model`, `/policy`, `/resume` + interactive-choice
      persistence (separate "state" file from portable config).
- [ ] U6 approval UX: risk badge + exact argv/cwd + diff preview; `[y] once [a] session [s] scoped
      grant [n] deny [e] details`; typed confirm for CRITICAL; decision recorded in JSONL audit trail.
      (hax has NO gate and argues against it — we keep ours but must avoid reflexive-y fatigue.)
- [ ] U7 pause/interrupt: Esc/SIGINT = pause at turn boundary, second = interrupt-and-save, resumable;
      SIGUSR1 in one-shot; `max_turns` bound.
- [ ] U8 inspectability: `--json` JSONL event stream for orchestrators, `/transcript` showing exactly
      what was sent (system prompt + schemas + tool calls/results), opt-in redacted wire trace.
- [ ] U9 session ergonomics: `-c/--continue`, `--resume[=ID]`, `/undo`, `/fork`, retention pruning,
      header record w/ git branch+commit. KEEP our sha256 chain (hax has none — auditability edge).
- [ ] U10 context & capability: auto-compaction at % threshold + `/compact [focus]` (seed append,
      never delete history); per-model capability probe cached like `catalog.json`; `effort_clamp`;
      presets (`system_prompt_append` + tint); `AGENTS.md` discovery global→repo root→cwd.

## Still queued
- [ ] True incremental streaming through the broker (currently buffered-until-complete; fine for dev).
- [ ] MCP stdio client wiring (`mcp/client.c` exists) + `/mcp add`. Keep OUT of core context:
      adopt hax's lazy-schema idea — always-on minimum tools only, expand on demand (HAX_STUDY §3.2).
- [ ] Config format reconsideration: hax uses JSON for config AND wire because the parser is linked
      anyway. Ours pulls a TOML parser — decide JSON vs keep tomlc17 deliberately.
- [ ] Optional external guard hook (Llama Guard via openai-local provider).
- [ ] Dependency policy: match hax's bar (Debian main, non-GPL, else run as subprocess). This alone
      justifies dropping termbox2 from the default build.
