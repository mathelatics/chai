cmake_minimum_required(VERSION 3.15)
project(minipi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# --- vendored MIT dependencies (cjson logc http_parser tomlc17 ureq klib termbox2 uv)
add_subdirectory(third_party)

# --- MiniPi core sources (in-house, MIT) -------------------------------------
add_library(minipi_core STATIC
  src/util/arena.c
  src/util/sha256.c
  src/util/base64.c
  src/util/uuid.c
  src/util/utf8.c
  src/util/diff.c
  src/util/schema.c
  src/agent/events.c
  src/agent/context.c
  src/agent/loop.c
  src/tools/registry.c
  src/tools/read.c
  src/tools/write.c
  src/tools/edit.c
  src/tools/bash.c
  src/providers/provider.c
  src/providers/openai.c
  src/providers/anthropic.c
  src/net/http.c
  src/net/sse.c
  src/session/store.c
  src/session/tree.c
  src/session/compact.c
  src/mcp/client.c
  src/net/broker_client.c
  src/net/tls_broker.c
  src/providers/nemotron.c
  src/tui/app.c
  src/tui/render.c
)
target_include_directories(minipi_core PUBLIC src)
target_link_libraries(minipi_core PUBLIC
  cjson logc http_parser tomlc17 klib ureq $<TARGET_NAME_IF_EXISTS:uv> $<TARGET_NAME_IF_EXISTS:termbox2>)

add_executable(minipi src/main.c)
target_link_libraries(minipi PRIVATE minipi_core)

enable_testing()
add_executable(smoke_deps tests/smoke_deps.c)
target_link_libraries(smoke_deps PRIVATE minipi_core)
add_test(NAME smoke_deps COMMAND smoke_deps)

add_executable(test_minipi tests/test_minipi.c)
target_link_libraries(test_minipi PRIVATE minipi_core)
add_test(NAME unit_tests COMMAND test_minipi)
