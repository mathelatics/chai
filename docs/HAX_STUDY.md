# hax study — TUI/CLI gap analysis for miniPi

Reference clone: `rough/hax` (github.com/OleksandrChekhovskyi/hax, MIT). ~45k LOC of C,
meson build, deps = libcurl + jansson only. Read-only study; never vendored (see `.gitignore`).

Why it matters: hax solves the *same problem we set out to solve* — a small native-C terminal
coding agent with streaming, local models, sessions, and tool use — and it has clearly iterated
on the human-facing layer far longer than we have. Our REPL (`src/tui/repl.c`, 352 lines) is a
`fgets()` loop that prints deltas raw. hax's user-facing layer is ~10k LOC across `src/render/`
and `src/terminal/`. That delta is our entire UX gap.

---

## 1. What hax actually is (architecture worth copying)

```
src/agent*        core loop, dispatch, usage/stats, env/context assembly
src/providers/    one file per backend: chat_body/chat_events (OpenAI-compat), anthropic_*,
                  codex (OAuth/ChatGPT), deepseek, llamacpp, ollama, http_provider (shared)
src/render/       disp (block-aware stdout writer), markdown(+scan/table/wrap), footer,
                  spinner, progress, tool_render, diff_color, ctrl_strip, render_ctx
src/terminal/     input(1395) + input_core, live_region, picker + picker_core, ansi/sgr,
                  escape, theme, vt_resolve, clipboard, notify, width
src/tools/        bash(+classify/output/process/shell/env), read/write/edit, task_registry,
                  output_cap, path_preprocess, image_sniff
src/text/         utf8, utf8_sanitize, width, trim, shell_quote, url
src/transport/    http, sse, retry, oauth, ca, api_error
cross-cutting     config.json + state.json, cred_store, catalog/model_meta/effort, compact,
                  history, transcript, trace, session_log(JSONL), banner, busy, buf, xalloc
```

Two ideas do most of the work:

**(a) It never takes over the screen.** No alternate buffer, no full-screen redraw. It writes
conversation text into the terminal's *native scrollback* and repaints only a small "live region"
below it (`terminal/live_region.h`: records painted row widths, knows how to climb back to the
region top even after the terminal rewrapped rows on resize). `render/footer.h` owns at most
`FOOTER_ROWS_MAX 8` rows: spinner status line + live tool output, animated on its own thread,
stepping aside whenever `disp` emits content, painting in synchronized frames so terminals never
show a blank flash. **This is exactly why our planned termbox2 full-screen TUI is the wrong
call** — see §4.

**(b) Everything user-visible is a stream-safe renderer.** `render/disp.h` tracks pending vs
committed newlines (so blocks get exactly one blank line), the cursor column accounting for
double-width/CJK and emoji sequences, partial UTF-8 codepoints and partial ANSI escapes carried
across chunk boundaries. `render/tool_render.h` feeds arbitrary tool-output chunks through
`ctrl_strip` + `utf8_sanitizer` before display, and picks a preview policy
(`DIFF` / `HEAD` / `HEAD_TAIL` with middle elision). `render/markdown*.c` renders streaming
Markdown including tables and wrapping. We do none of this today: we `fputs()` model bytes and
tool output straight to stdout.

---

## 2. Feature-by-feature: what hax has that we don't

| Area | hax | miniPi today | Gap severity |
|---|---|---|---|
| Input editing | readline-style movement, multiline (Shift-Enter), Up/Down recall, **Ctrl-R fuzzy history search**, Tab completion of `/cmd` and args, `@file` picker via fzf, Ctrl-G `$EDITOR`, Ctrl-V clipboard image, wide-char-correct cursor | `fgets(line, 4096)` single line, no history, no completion | **BLOCKING for "state of the art"** |
| Provider selection | `/provider` `/model` `/effort` open **type-to-filter pickers** (`picker_run`), model ids completed from a background-fetched list, choices persisted to `state.json` | `/connect`, `/model` typed by exact id; nothing persisted interactively | HIGH |
| Streaming UX | incremental Markdown, spinner+elapsed timer, deferred show (no blink for short waits), label settle 2s | raw token dump, no spinner, thinking text interleaved with answer | HIGH |
| Tool output | live rows in footer, unified-diff coloring for edits, head/tail capping, control chars stripped, `READ_LINE_DELIM →` so line numbers can't be mistaken for indentation | print result blob | HIGH |
| Approval UX | *(deliberately absent — see §3)* | y/n prompt per call | we are ahead here, but the prompt must be built properly (§5) |
| Sessions | JSONL append-only under `$XDG_STATE_HOME/hax/sessions/<slug+hash-of-cwd>/`, `<timestamp>_<uuid>.jsonl`, owner-only 0600, retention pruning, header/selection/undo/user_turn/item records, `-c`, `--resume` picker, `--resume=ID -p`, `/fork`, `/undo`, resume restores provider/model/effort/preset | JSONL + sha256 chain + checkpoints (our chain is stronger); `--prompt` still routes to offline stub; no resume/fork/undo UI | MEDIUM-HIGH |
| Inspectability | **Ctrl-T model-facing transcript** (system prompt, tool schemas, model-visible calls/results, post-compaction context) in `$PAGER`; Ctrl-O rendered conversation; `HAX_TRACE` redacted HTTP/SSE wire trace; `hax --json` JSONL event stream for orchestrators | nothing | HIGH — this is the debugging superpower |
| Context mgmt | `/compact [focus]`, auto at `compact.threshold` % of `context_limit`, summary appended as a *seed* (history never deleted) | `session/compact.c` exists, not wired to threshold/auto or focus arg | MEDIUM |
| Model metadata | `model_meta_*` probes capabilities per model (context limit, effort ladder, image input), `catalog.json` cache in `$XDG_CACHE_HOME`, config overrides > provider report > snapshot | hardcoded assumptions per provider | MEDIUM |
| Effort/reasoning | categorical `effort_set` + `effort_clamp()` nearest-level fallback; reasoning shown/hidden by key | `--thinking` flag only | LOW-MED |
| Presets | named provider+model+effort+`system_prompt_append`+`tint` bundles; `/preset-save`; described presets become **subagent roles**; `hax daily` shortcut | none | MEDIUM |
| Project instructions | global + repo-root→cwd `AGENTS.md` hierarchy; `.agents/skills/*/SKILL.md` discovery incl. cross-agent `~/.agents/skills` | none | MEDIUM |
| One-shot discipline | `-p`: answer→stdout, banner/usage/errors/**resume hint**→stderr; `printf … \| hax -p`; SIGUSR1=clean pause, SIGINT=interrupt-and-save (exit 130) | `--prompt` prints, but goes to stub | HIGH (correctness) |
| Interruption | Esc = pause at next turn boundary; Esc Esc = immediate interrupt, partial answer kept, unfinished reasoning discarded; Enter-on-empty continues; `max_turns` bound | none (blocks until done) | HIGH |
| Notifications | `notify` setting: OSC bell/sequence when REPL needs attention | none | LOW |
| XDG paths | config/state/cache all XDG-correct w/ fallbacks | `$HOME/.minipi/...` | LOW |
| Theme | `theme dark/light`, preset tints (teal/violet/rose/sage), SGR state tracked in `disp` | ANSI codes inline | LOW |

---

## 3. Where hax deliberately disagrees with us — and what to steal anyway

`docs/philosophy.md` is unusually honest about omissions. Three matter for our decisions:

1. **No permission prompts.** Their argument: an in-process gate "earns its keep poorly", trains
   reflexive approval, and *is not a security boundary* since the same shell/FS/credentials sit one
   confused command away; isolation should come from the OS (container/VM).
   **Our requirement is the opposite (every tool requires user permission), so we keep the gate —
   but we must treat their critique as real.** Mitigations to design in, not skip:
   - Make approval **non-reflexive**: show risk class + exact argv/cwd + a diff preview for
     writes, and require typing rather than bare `y` for HIGH/CRITICAL (their "reflexive y" point).
   - Support **scoped grants** ("allow `git diff` this session", "allow reads under `src/`") instead
     of one global mode, so the gate costs little on routine work.
   - Keep the belt-and-braces OS story: cwd confinement, symlink checks (`path_preprocess`),
     env scrubbing for child processes (`bash_env`), and document that the C binary holds no keys.
   - Note: our broker already gives them what they ask for (loopback bind, host allow-list,
     Authorization stripped) while still gating tools. That's a defensible "both".

2. **No MCP in core** — capabilities are CLIs plus a `SKILL.md` telling the model when to use them;
   `hax-mcp` is a separate CLI that pulls tool definitions *only as a task needs them*.
   **Steal the lazy-schema idea regardless of MCP**: injecting every tool schema into every request
   is context tax. Our `mp_toolreg_schemas()` should emit the always-on minimum (read/bash/edit/
   write) and expand on demand. Also strongly suggests our queued MCP stdio client stay optional
   and out-of-process.

3. **No hooks/plugins; compose via subprocesses + `--json`.** Cheap for us: emitting a JSONL event
   stream on stdout in one-shot mode gives scripts/orchestrators observability without any plugin
   ABI. High value, low cost — add it.

Also worth noting: they use **JSON for both config and wire** because jansson is linked anyway —
"avoid TOML/YAML = second parser". Our `config.c` currently drags in a TOML parser; consider
switching to JSON (or keeping TOML only if the vendored parser is already paid for). And their
dependency bar ("must be in Debian main, not GPL, otherwise run it as a subprocess") is a good
governance rule for us too — it independently kills the termbox2 plan below.

---

## 4. Recommendation: drop the termbox2 full-screen TUI

Our queue said "termbox2 full-screen TUI on top of the REPL event flow". hax demonstrates the
opposite and explains it in the README: *"Respects your terminal… Only redraws the current
streaming line or the input area, native scrollback is preserved. Does not take over or mess with
your terminal."* Full-screen alt-buffer agents lose scrollback, fight tmux/screen, break copy-mode,
mishandle wide characters, and need a widget toolkit — the antithesis of a few-MB C binary.

Replace that item with three in-place pieces, each mirroring a hax module:

- `src/tui/disp.[ch]` — block-aware, style-tracking, UTF-8/ANSI-safe stdout writer (port shape of
  `render/disp.h`).
- `src/tui/live_region.[ch]` + `footer` — repaint N rows below the output; spinner + elapsed +
  live tool rows; synchronized-update bracket; deferred show so short waits don't blink.
- `src/tui/input.[ch]` — raw-mode line editor with history, Ctrl-R, Tab completion, `@file`.

Keep termbox2 only as an optional `--fullscreen` experiment behind a build flag, not the default.

---

## 5. Concrete plan for miniPi (ordered, each step independently shippable)

**P0 — finish what's open** (from prior notes): implement `mp_broker_set_key_env` in C +
matching header handling in `tools/tls_broker.py`; rebuild; full test pass; route `--prompt`
through the live provider instead of the offline stub; then the live end-to-end run.

**U1 — don't corrupt the terminal (biggest perceived-quality jump).**
Add `util/utf8_sanitize` (we already have `util/utf8`) + `tui/ctrl_strip` and pipe **all** model
deltas and tool output through them. Fix thinking/answer interleaving: reasoning goes to stderr or
a dim prefix, never mixed into the answer stream. Add `tui/disp` newline collapsing so blocks get
one blank line.

**U2 — streaming Markdown + tool rendering.** Port concepts from `render/markdown_scan.c` +
`markdown_wrap.c` (headings/bullets/code fences, table support later) and `render/diff_color.c`
for `edit`/`write` diffs. Adopt `output_cap` semantics: cap bytes + line width + line count, with
an explicit elision marker, and apply it **before** results go to the model (protects context).

**U3 — live footer.** `tui/footer` with spinner glyph, elapsed timer, deferred show, ≤8 rows of
live tool output, stepping aside for disp. This replaces "buffered-until-complete" as the thing
users notice, and pairs naturally with true incremental streaming (queued P-task).

**U4 — real line editor.** `tui/input`: arrow-key movement, home/end, word-left/right, kill/yank,
multiline, Up/Down history persisted per-directory (hax keeps `history` beside the session files —
good idea, scope recall to cwd), Ctrl-R search, Tab completion for `/commands` and args, `@file`
(fzf if present, else internal picker), Ctrl-L clear/redraw, Ctrl-C twice-to-quit, Ctrl-D quit.

**U5 — pickers.** `tui/picker` (type-to-filter, borrowed labels, `detail` + `description` rows,
returns -1 on non-TTY) used by `/provider`, `/model`, `/policy`, `/resume`. Wire persistence to a
`state.json` equivalent so interactive choices survive restarts without touching portable config.

**U6 — approval UX done right.** Single-line summary + risk badge, exact command + cwd, diff
preview for file writes, `[y] once · [a] always-this-session · [s] scoped grant · [n] deny · [e]
show full args`. Require typed confirmation for CRITICAL. Log the decision into the session JSONL
(audit trail hax doesn't have — ours becomes a feature).

**U7 — pause/interrupt.** SIGINT/Esc semantics: first = clean pause at turn boundary, second =
interrupt with completed work saved, resumable. In one-shot mode map SIGUSR1/SIGTERM likewise and
print the resume hint on stderr. This makes long runs feel safe — which is what actually reduces
permission fatigue.

**U8 — inspectability.** `--json` JSONL event stream in one-shot mode (turn/tool_call/tool_result/
usage/result records, closed by `result`), `/transcript` (Ctrl-T equivalent) showing exactly what
was sent, and an opt-in redacted wire trace. Highest ROI for us while the agent is still buggy.

**U9 — session ergonomics.** `-c/--continue`, `--resume[=ID]`, `/undo`, `/fork`, retention
pruning, header record with git branch/commit, selection records. Keep our sha256 chain (they have
none) — it's a genuine differentiator for auditability. Move to XDG paths.

**U10 — context & capability.** Auto-compaction at a `%` threshold with `/compact [focus]`, seed
append (never delete history); per-model capability probe (context limit, effort levels, image
input) cached like `catalog.json`; `effort_clamp` for graceful degradation; presets with
`system_prompt_append` + tint; `AGENTS.md` discovery (global → repo root → cwd).

**Deferred / rejected:** termbox2 default UI (rejected, §4); MCP eager schema injection (rejected —
lazy, §3); plugin/hook system (rejected — subprocess composition + `--json`); TOML config
(reconsider → JSON, one parser).

---

## 6. Files to read again while implementing

- `rough/hax/src/terminal/live_region.h` — the rewind/climb algorithm (resize-safe repaint).
- `rough/hax/src/render/footer.h` — attach/show_deferred/set_rows/rows_that_fit contract.
- `rough/hax/src/render/disp.h` — newline collapse, `disp_screen_column`, mid-sequence detection.
- `rough/hax/src/render/tool_render.h` — feed/chunk-boundary-safe preview with head/tail elision.
- `rough/hax/src/terminal/input.c` (1395 L) + `picker.c` — the line editor and filter picker.
- `rough/hax/src/transcript.c` — model-facing view (what we need for `/transcript`).
- `rough/hax/src/tools/output_cap.{h,c}` + `bash_classify.{h,c}` — capping + read-only heuristic
  (useful for our risk classes; theirs is display-only, ours can inform policy).
- `rough/hax/docs/{usage,configuration,sessions,philosophy}.md` — the product spec equivalents.
