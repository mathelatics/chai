cmake_minimum_required(VERSION 3.15)
project(minipi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# --- vendored MIT dependencies (cjson logc http_parser tomlc17 klib termbox2)
add_subdirectory(third_party)

# The TLS broker is the only component needing OpenSSL. Keep it optional so the
# agent + full TUI still build on systems without OpenSSL (local inference and
# plaintext loopback keep working).
option(MINIPI_BUILD_BROKER "Build the optional OpenSSL TLS broker" ON)

# --- MiniPi core sources (in-house, MIT) -------------------------------------
add_library(minipi_core STATIC
  src/util/arena.c
  src/util/sha256.c
  src/util/base64.c
  src/util/uuid.c
  src/util/paths.c
  src/util/utf8.c
  src/util/diff.c
  src/util/schema.c
  src/agent/events.c
  src/agent/context.c
  src/agent/loop.c
  src/tools/registry.c
  src/tools/output_cap.c
  src/tools/read.c
  src/tools/ls.c
  src/tools/find.c
  src/tools/grep.c
  src/tools/write.c
  src/tools/edit.c
  src/tools/bash.c
  src/providers/provider.c
  src/providers/router.c
  src/providers/anthropic.c
  src/net/sse.c
  src/session/store.c
  src/session/tree.c
  src/session/compact.c
  src/mcp/client.c
  src/net/broker_client.c
  src/config/config.c
  src/config/keys.c
  src/security/policy.c
  src/providers/nemotron.c
  src/tui/app.c
  src/tui/fullscreen.c
  src/tui/repl.c
  src/tui/disp.c
  src/tui/input.c
  src/tui/live.c
)
target_include_directories(minipi_core PUBLIC src)
target_link_libraries(minipi_core PUBLIC
  cjson logc http_parser tomlc17 klib termbox2 m)

add_executable(minipi src/main.c)
target_link_libraries(minipi PRIVATE minipi_core)

enable_testing()
add_executable(smoke_deps tests/smoke_deps.c)
target_link_libraries(smoke_deps PRIVATE minipi_core)
add_test(NAME smoke_deps COMMAND smoke_deps)

add_executable(test_minipi tests/test_minipi.c)
target_link_libraries(test_minipi PRIVATE minipi_core)
add_test(NAME unit_tests COMMAND test_minipi)

# --- minipi-broker: pure-C TLS boundary (separate executable) ----------------
# Optional: the core never links OpenSSL. Without it, cloud providers are
# unavailable but local inference (Ollama/LM Studio/llama.cpp/vLLM) still works.
if(MINIPI_BUILD_BROKER)
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    add_executable(minipi-broker src/net/tls_broker_main.c)
    target_link_libraries(minipi-broker PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(minipi_core PUBLIC MINIPI_HAVE_BROKER=1)
    message(STATUS "MiniPi: TLS broker enabled (OpenSSL ${OPENSSL_VERSION})")
  else()
    message(STATUS "MiniPi: OpenSSL not found — cloud providers disabled, local inference available")
  endif()
endif()
