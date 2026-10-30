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

## Next (queued)
- [ ] True incremental streaming through the broker (currently buffered-until-complete; fine for dev).
- [ ] termbox2 full-screen TUI on top of the REPL event flow.
- [ ] MCP stdio client wiring (`mcp/client.c` exists) + `/mcp add` command.
- [ ] Compaction polish (tree branching UI, `/undo`).
- [ ] Optional external guard hook (Llama Guard via openai-local provider).
