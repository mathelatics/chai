# Vendored MIT dependencies for the agent runtime.
# Each library is compiled as a STATIC target from its minimal file set only.

# cJSON — JSON parse/build (DaveGamble/cJSON, MIT)
add_library(cjson STATIC ${CMAKE_CURRENT_LIST_DIR}/cjson/src/cJSON.c)
target_include_directories(cjson PUBLIC ${CMAKE_CURRENT_LIST_DIR}/cjson/include)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)

# log.c — leveled logging to stderr/file (rxi/log.c, MIT)
add_library(logc STATIC ${CMAKE_CURRENT_LIST_DIR}/log.c/src/log.c)
target_include_directories(logc PUBLIC ${CMAKE_CURRENT_LIST_DIR}/log.c/include)

# http_parser — incremental HTTP request/response parser (nodejs/http-parser, MIT)
add_library(http_parser STATIC ${CMAKE_CURRENT_LIST_DIR}/http_parser/src/http_parser.c)
target_include_directories(http_parser PUBLIC ${CMAKE_CURRENT_LIST_DIR}/http_parser/include)

# tomlc17 — TOML v1.1 config parser (cktan/tomlc17, MIT)
add_library(tomlc17 STATIC ${CMAKE_CURRENT_LIST_DIR}/tomlc17/src/tomlc17.c)
target_include_directories(tomlc17 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/tomlc17/include)

# ureq — header-only micro HTTP client (solusipse/ureq, MIT)
# Requires cjson + http_parser at link time by the consuming target.
add_library(ureq INTERFACE)
target_include_directories(ureq INTERFACE ${CMAKE_CURRENT_LIST_DIR}/ureq/include)
target_link_libraries(ureq INTERFACE cjson http_parser)
