# PLAN — one step at a time (C-0 … C-12)

Rules of engagement (per user instruction):
- **Exactly one step lands per pass.** A step is done only when: build green,
  `ctest` green, its acceptance check passes, docs/TODO updated, and this file's
  status column flipped to DONE. Never start the next step on a red build.
- Pure C11, MIT-only vendored deps, no new mandatory dependencies.
- Keep the differentiators: sha256 chain, key custody, fail-closed policy gate.

Statuses: TODO / IN-PROGRESS / DONE(date).

| # | Step | Source gap | Status |
|---|------|-----------|--------|
| C-0 | Baseline verification & stale-doc cleanup | docs/TODO "P0 open" items | **DONE** (this pass: build+tests verified green; mp_broker_set_key_env exists; --prompt routes live) |
| C-1 | `util/utf8_sanitize` + `tui/ctrl_strip` + `tui/disp` — stream-safe output; reasoning separated from answer stream | GAP §4 / U1 | TODO |
| C-2 | Tool-output capping (`output_cap`: bytes + width + lines, elision marker) applied **before** results re-enter context | GAP §5 / U2 | TODO |
| C-3 | `tui/live_region` + `tui/footer`: spinner, elapsed timer, deferred show, ≤8 live rows, CSI 2026 synchronized bracket | GAP §4 / U3 + pi-tui technique | TODO |
| C-4 | `tui/input`: raw-mode line editor — arrows/home/end/word-ops, multiline, per-cwd history, Ctrl-R, Tab completion, bracketed paste, Ctrl-C/Ctrl-D | GAP §4 / U4 + pi-tui Editor | TODO |
| C-5 | Event spine refactor: REPL consumes `mp_bus` events only; error-as-data (`stopReason: error|aborted`) in provider layer; canonical event vocabulary shared by all consumers | GAP §1.2–1.3 | TODO |
| C-6 | `--json` JSONL event stream for one-shot mode (turn/tool_call/tool_result/usage/result records) | GAP §6 | TODO |
| C-7 | Approval UX v2: risk badge + exact argv/cwd + diff preview, `[y/a/s/n/e]`, typed confirm for CRITICAL, scoped grants, audit records | GAP §4 / U6 | TODO |
| C-8 | Interrupts/pause: SIGINT/Esc pause-at-turn-boundary, double=interrupt-and-save, partial kept, resume hint on stderr, `max_turns` bound surfaced | GAP §4 / U7 | TODO |
| C-9 | Session ergonomics: `-c/--continue`, `--resume[=ID]` picker, `/undo`, `/fork`, retention pruning, header record w/ git branch+commit, XDG paths; commit-before-show tool-intent records | GAP §2 / U9 + pi-durable | TODO |
| C-10 | Context hooks + auto-compaction: `transform_context`/`convert_to_llm` seams, threshold-based compaction + `/compact [focus]` seed-append | GAP §1.4 / U10 + pi compaction | TODO |
| C-11 | Model metadata & usage: per-model capability cache (context limit, thinking), token/cost accounting per turn shown in footer + JSON events, retry/error taxonomy | GAP §3 + pi-ai | TODO |
| C-12 | Extra tools (ls/find/grep) + MCP wiring behind `/mcp add` with lazy schema injection; streaming markdown rendering folded into `disp` | GAP §4–5, U2/U5 + pi-mcp | TODO |

## Sequencing rationale

- C-1/C-2 first: they stop terminal corruption and context blowups — highest perceived
  quality jump, lowest risk, pure additions.
- C-3/C-4 before C-5: give users the interactive win while the loop is still simple;
  both are self-contained modules with narrow APIs so they don't fight the refactor.
- C-5 is the structural keystone (pi's event spine); doing it after C-1..C-4 means we
  refactor *finished* components instead of inventing them mid-refactor.
- C-6 falls out of C-5 almost for free (same events, JSON serializer).
- C-7/C-8 polish the human-trust story built on top of C-5's abortable loop.
- C-9/C-10 deliver pi-durable-style recoverability and pi-context compaction once the
  event vocabulary exists to express intents and summaries as records.
- C-11 surfaces cost/errors that pi-ai treats as first-class.
- C-12 is breadth (more tools/MCP/markdown) deliberately last.

## Per-step definition of done (template)

```
Scope:        files added/changed, public API of the new module(s)
Acceptance:   concrete check (unit test in tests/test_minipi.c and/or scripted CLI run)
Regression:   cmake --build && ctest 2/2 (+ new test count) green
Docs:         chai/docs/PLAN.md status flip + docs/TODO.md item checked
```

## Next up: C-1 (draft scope)

- Add `src/util/utf8.[ch]` extension: `mp_utf8_sanitize(buf,len,out)` replacing invalid /
  C0 control chars (except \n,\t) with U+FFFD or stripping ANSI-incomplete sequences
  safely across chunk boundaries (carry state in a small struct).
- New `src/tui/disp.[ch]`: `mp_disp_write(stream_id, text, len)` where stream_id ∈
  {ANSWER, REASONING(dim prefix or suppressed), TOOL_OUT}; newline collapse (pending vs
  committed), flushes on message end. REPL's `on_delta()` and bash tool output route
  through it; termbox2 `render.c` untouched (stays optional/experimental).
- Acceptance: unit test feeding split-across-chunk multibyte UTF-8 + ESC sequences →
  output contains no raw control bytes other than \n\t; existing 2 tests still pass.
