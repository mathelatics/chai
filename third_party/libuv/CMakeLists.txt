# Vendored libuv (MIT) — Linux-only minimal static build for MiniPi.
# Source pinned at commit 33395d143f267f00820a3327588b1c397c66b209 (v1.53.1).
# Upstream CMakeLists.txt is NOT vendored; this is a hand-written minimal file
# replicating upstream's Linux source list + defines. See VENDORED.md.
cmake_minimum_required(VERSION 3.15)
project(uv_vendored C)

if(NOT UNIX OR APPLE OR CMAKE_SYSTEM_NAME STRGREATER "Darwin")
  # guard: intended for Linux; other platforms should vendor more files
endif()

set(UV_COMMON
  src/fs-poll.c src/idna.c src/inet.c src/random.c src/strscpy.c src/strtok.c
  src/thread-common.c src/threadpool.c src/timer.c src/uv-common.c
  src/uv-data-getter-setters.c src/version.c)

set(UV_UNIX
  src/unix/async.c src/unix/core.c src/unix/dl.c src/unix/fs.c
  src/unix/getaddrinfo.c src/unix/getnameinfo.c src/unix/loop-watcher.c
  src/unix/loop.c src/unix/pipe.c src/unix/poll.c src/unix/process.c
  src/unix/random-devurandom.c src/unix/signal.c src/unix/stream.c
  src/unix/tcp.c src/unix/thread.c src/unix/tty.c src/unix/udp.c
  src/unix/linux.c src/unix/procfs-exepath.c src/unix/random-getrandom.c
  src/unix/random-sysctl-linux.c src/unix/proctitle.c)

add_library(uv STATIC ${UV_COMMON} ${UV_UNIX})
target_include_directories(uv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(uv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(uv PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200112
                                      _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
find_package(Threads REQUIRED)
target_link_libraries(uv PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(uv PUBLIC rt)
endif()
