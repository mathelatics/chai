# Vendored termbox2 (MIT) — single-header library.
# Pinned at commit 48cda6f83a4a1a98d3546beafe55b31a5861c21d.
# Exactly one .c translation unit includes termbox2.h with TB_IMPL defined.
cmake_minimum_required(VERSION 3.15)
project(termbox2_vendored C)

add_library(termbox2 STATIC termbox2_impl.c)
target_include_directories(termbox2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(termbox2 PUBLIC m)
