# MiniPi — Feature Research: how real agents do it (Oct 2026)

Compiled before writing code. Sources: official docs + public source of the major
terminal coding agents. Each section = what they do → what MiniPi adopts.

## 1. Connection / provider management

| Agent | How users configure providers & keys |
|---|---|
| **Claude Code** | Env vars (`ANTHROPIC_API_KEY`, `ANTHROPIC_BASE_URL`), `/model` command in-session, `apiKeyHelper` script for dynamic key fetch, `.claude/settings.json` per-project. Keys never stored in plaintext config by default. |
| **Codex CLI (OpenAI)** | `config.toml` profiles (`[profiles.provider_x]`) with `base_url`, `env_key`, wire API; `--oss` flag for local open models; `/login`; model chosen per profile at runtime. |
| **Gemini CLI** | `.env` file + env vars (`GEMINI_API_KEY`, `GOOGLE_GENAI_USE_VERTEXAI`), `/model` slash command, settings.json. |
| **Aider** | YAML/CLI config with `--openai-api-base`, per-chat `--model`, `aider --show-models`; OpenAI-compat is a first-class target (works w/ Ollama/LM Studio unchanged). |
| **Ollama** | Local server on `127.0.0.1:11434`, native `/api/chat` **and** an OpenAI-compatible `/v1` endpoint; models pulled separately. |
| **LM Studio** | Local server on `127.0.0.1:1234`, explicitly emulates OpenAI `/v1/chat/completions` + `/v1/models`. |
| **llama.cpp server** | llama-server exposes `/v1/chat/completions` + `/v1/models` on any bind/port. |

**Adopt for MiniPi:**
- A **provider registry** in `~/.minipi/config.toml`: named providers with
  `kind` in {nemotron, openai, anthropic, openai-local}, `base_url`, `model(s)`,
  `key_env` (name of the env var holding the key — the *value* never lives in the C binary).
- `/connect list|add|rm|use <name>` REPL commands; `/models` probes
  `GET <base>/models` through the broker (the standard discovery endpoint all four support).
- Key custody stays exactly as designed: key lives in `.env`/broker process env;
  C binary only ever talks to `127.0.0.1` broker (cloud) or `127.0.0.1` local
  inference server (no key needed). Broker allow-list extended with
  loopback targets for LM Studio/Ollama/llama.cpp ports.
- Fallback order for HOME-style env bugs (this session's lesson): explicit
  `MINIPI_HOME` > `$HOME` > log-and-fail-loudly (not silent `/tmp`).

## 2. Model selection

- Claude Code: `/model` lists entitlements, supports aliases and full IDs; persists choice.
- Codex: config profiles + `codex --model X`; `/model` in TUI.
- Gemini CLI: `/model` with tab-completion from live model list.
- **Adopt:** `/model [name]` — bare form lists (live via `/v1/models` probe when reachable, else configured list); setting validates non-empty, applies next turn, persisted to config.toml `[session] model=`.

## 3. Tool permissions (ask-before-run)

| Agent | Mechanism |
|---|---|
| **Claude Code** | Every tool call gated unless pre-approved. Modes: default ask / `acceptEdits` / `plan` / `bypassPermissions`. Per-tool rules (`Bash(git *)` prefix patterns), always-deny wins, session-scoped "always allow" grants, hooks for custom policy. Non-interactive runs fail closed. |
| **Codex CLI** | Sandbox (landlock/seccomp) + approval policy: `untrusted` (ask for everything), `on-request`, `on-failure`, `never`; writes outside workspace always ask. |
| **Gemini CLI** | Confirmation per tool-call class; "Trust this folder", auto-approve read-only tools, always confirm shell edits. |
| **OpenHands** | Runtime with action classification; LLM-as-judge risk analysis before risky commands. |
| **Goose (Block)** | Recipe-level permission prompts; type-based gates (shell/edit vs read). |

Consensus design rules we adopt:
1. **Default = ask for every tool**, grouped by class (`read` may be auto-allowed, `write/edit/bash` always ask initially).
2. Responses: `y` once / `a` always-allow this tool this session / `n` deny / `d` deny+explain back to model.
3. **Deny is fed back to the model** as a tool result ("user denied…") so it can adapt — Claude Code does exactly this.
4. Policy modes: `ask-always` (default), `auto-read` (reads free, mutations ask), `full-auto` (explicit opt-in per session, logged loudly).
5. Non-interactive (`--prompt`) mode **fails closed**: dangerous tools denied unless `--auto-approve` passed explicitly.

## 4. Harmful-input / injection flagging

- **Meta Llama Guard 3/4** (open weights): classifies prompt & completion against hazard categories. Used as an out-of-band classifier.
- **NVIDIA Content Safety / Nemotron safety models**: same idea, NeMo Guardrails ships them as rails.
- **Rebuff/LLMGuard-style**: heuristic layer — secret-leak regexes, injection-pattern matching, canary strings, perplexity.
- **Claude Code/Codex**: static deny-lists for destructive shell (`rm -rf /`, `mkfs`, fork bombs, `curl|sh`), plus git-root protection and write-outside-workspace detection.

**Adopt (offline-first, since MiniPi must run without any safety API):**
- Pattern engine over tool arguments + user text: destructive shell, privilege escalation, remote-code-pipe, credential-file access (`~/.ssh`, `.env`, keyring paths), outbound exfil URLs, classic injection phrases ("ignore previous instructions", jailbreak markers).
- Risk score 0..1 per pending action; HIGH risk forces interactive confirmation even in `auto-read` mode, and hard-blocks in `full-auto` unless overridden.
- Category flags recorded into the session JSONL (audit trail).
- Optional pluggable external guard hook later (Llama Guard via local llama.cpp is reachable through the same OpenAI-compat path).

## 5. User scope constraint ("user only able to run the TUI/CLI")

- All surveyed agents confine the agent process itself: workspace path confinement (already in `mp_path_confine`), no network for tools, cwd pinned.
- **Adopt:** bash tool runs in a child with `chdir(root)` (exists) + rlimits (exists) + **secret env scrubbing** (strip `*_API_KEY`/`*_TOKEN` from child env), writes outside workspace blocked (exists), and the *only* way to change policy/provider/model at runtime is via REPL/TUI slash commands.

## 6. What already exists in MiniPi vs gaps

Exists: broker TLS boundary + host allow-list + key-in-env-only; JSONL store w/ sha256 chain; schema-validated tool registry; path confinement; rlimit'd bash; SSE parser w/ reasoning_content.
Gaps this plan closes: session dir silently wrong on bad `$HOME` (bug), no errno logging, tool schemas not sent to provider (model can't emit tool calls), no permission gate, no risk flagging, no provider CRUD/model listing from REPL, no local-inference routing.
