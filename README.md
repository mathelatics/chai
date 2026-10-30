# MiniPi — a Pi-style coding agent in pure C (strict MIT dependencies)

MiniPi is an interactive terminal coding agent modeled after `pi`: streaming LLM
chat, tool use (`read` / `write` / `edit` / `bash`), session persistence with
branching, MCP tool servers over stdio, and a TUI — built as a small, auditable
C11 core on top of vendored **MIT-only** libraries.

- Core language: C11, no extensions required outside vendored deps.
- Build: CMake ≥ 3.15 (primary), plain `gcc` one-liner (fallback).
- Platform today: Linux (libuv vendoring is Linux-scoped; macOS/BSD need extra
  uv sources — see docs/TODO.md M0).
- License posture: every vendored file is MIT. TLS is the one deliberate gap
  (see docs/ARCHITECTURE.md §7 and third_party/VENDORED.md).

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/minipi --help          # CLI skeleton
ctest --test-dir build         # dependency smoke test + unit tests
```

Verify all vendored deps compile/link/run without CMake:

```sh
gcc -std=c11 -D_GNU_SOURCE -Ithird_party/cjson/include -Ithird_party/log.c/include \
    -Ithird_party/http_parser/include -Ithird_party/tomlc17/include \
    -Ithird_party/klib/include -Ithird_party/termbox2/include \
    -Ithird_party/libuv/include -Ithird_party/libuv/src \
    tests/smoke_deps.c third_party/cjson/src/cJSON.c third_party/log.c/src/log.c \
    third_party/http_parser/src/http_parser.c third_party/tomlc17/src/tomlc17.c \
    third_party/termbox2/termbox2_impl.c third_party/libuv/src/*.c \
    third_party/libuv/src/unix/*.c -lpthread -ldl -lrt -lm -o /tmp/smoke && /tmp/smoke
```

## Documentation map

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Full system design: layers, data flow, module contracts, threading model, memory rules, TLS strategy |
| [docs/PLAN.md](docs/PLAN.md) | Milestone plan M0→M6, sequencing rationale, risks |
| [docs/TODO.md](docs/TODO.md) | Living checklist — check items off as they land |
| [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) | Every vendored library: why, API surface used, integration notes |
| [third_party/VENDORED.md](third_party/VENDORED.md) | Compliance ledger: repo, commit SHA, license, files copied, review date |
| [AGENTS.md](AGENTS.md) | Rules for humans *and* agents editing this repo (security-critical C conventions) |

## Layout

```
mini-pi-c/
├── CMakeLists.txt
├── AGENTS.md
├── docs/                  ARCHITECTURE, PLAN, TODO, DEPENDENCIES
├── src/
│   ├── main.c             CLI entry, config load, mode dispatch
│   ├── util/              arena, sha256, base64, uuid, utf8, diff  (in-house)
│   ├── agent/             loop.c context.c events.c               (in-house)
│   ├── tools/             registry read write edit bash diff       (in-house)
│   ├── providers/         provider openai anthropic                (in-house)
│   ├── net/               http sse tls_broker                      (in-house + ureq)
│   ├── session/           store tree compact                       (in-house)
│   ├── mcp/               client (JSON-RPC over stdio)             (in-house)
│   └── tui/               app render                               (termbox2)
├── third_party/           8 MIT libraries, file-level vendored
└── tests/                 smoke_deps.c + unit tests per milestone
```

## Dependency set (all MIT, pinned commits)

| Lib | Role | Target kind |
|---|---|---|
| cJSON | JSON everywhere | static |
| log.c | leveled logging | static |
| http_parser | HTTP response parsing (SSE/chunked) | static |
| tomlc17 | config files | static |
| ureq | dev-only plaintext HTTP fallback | interface (header-only) |
| libuv v1.53.1 | event loop, timers, child procs, tty | static (Linux) |
| termbox2 | terminal UI | static (shim TU) |
| klib | khash/kvec/klist/ksort/kstring | interface (header-only) |

In-house (not vendored by policy): SSE parser, JSON-Schema subset validator,
Myers diff, JSONL session store, MCP client, SHA-256, base64, UUIDv4, arena
allocator, UTF-8 validation, permission gates, process sandboxing.
