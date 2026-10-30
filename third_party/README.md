# Vendored Third-Party Dependencies (MIT-only)

Procured per dependency policy: MIT license verified via GitHub API (`spdx_id == "MIT"`),
minimal file sets only (no tests/examples/docs/benchmarks), pinned to commit SHAs below.

| Library | Source (MIT) | Pinned commit | Files vendored | Purpose in agent runtime |
|---------|--------------|---------------|----------------|--------------------------|
| cjson     | DaveGamble/cJSON        | 6d9f2443ab | src/cJSON.c, include/cJSON.h, LICENSE | LLM request/response JSON (tool-calls, SSE payloads) |
| log.c     | rxi/log.c               | f9ea34994b | src/log.c, include/log.h, LICENSE | Leveled logging of agent loop, tool exec, errors |
| http_parser | nodejs/http-parser    | ec8b5ee63f | src/http_parser.c, include/http_parser.h, LICENSE-MIT | Incremental HTTP response parsing (incl. streamed LLM output) |
| tomlc17   | cktan/tomlc17           | 4987cf895d | src/tomlc17.c, include/tomlc17.h, LICENSE | Agent config files (model, temperature, tools) |
| ureq      | solusipse/ureq          | a413e8c3c2 | include/*.h (header-only), LICENSE | Micro HTTP client for OpenAI/Anthropic-style API calls |

## Rejected candidates (license rule)
- curl/curl — NOASSERTION (curl-style license, not MIT)
- ggreau/mqtt-c — EPL-2.0; eclipse-paho/* — EPL/EDL
- armin.io/libhv — BSD-3-Clause; acl-dev/acl — LGPL-2.1
- likholiha/cJSON-HTTP-POST-Requests — no LICENSE file found
- cktan/tomlc99 — NOASSERTION (custom)

## Build integration
See `third_party/CMakeLists.txt`. From a parent project:
```cmake
add_subdirectory(third_party)
target_link_libraries(my_agent PRIVATE cjson logc http_parser tomlc17 ureq)
```

## Verification
`smoke_test.c` at repo root exercises each library and links against all four static
targets + the header-only one. Verified with gcc -std=c11 -Wall and CMake 3.16+ (see build/).
