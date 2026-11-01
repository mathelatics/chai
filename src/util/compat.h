/* util/compat.h — cross-platform shims (C-13: runs on Linux/macOS/Windows).
 *
 * Strategy: keep the codebase POSIX-shaped and provide missing pieces here.
 *   - POSIX systems: mostly no-ops; we only force _XOPEN_SOURCE=700 so that
 *     strdup / strcasecmp / fsync etc. are declared with a single define
 *     instead of per-file _GNU_SOURCE/_DEFAULT_SOURCE (which also drags in
 *     GNU-only extensions like memmem/strcasestr that MSVC lacks).
 *   - Windows (_WIN32): map to the CRT equivalents (_strdup, _mkdir), provide
 *     inline implementations of memmem/strcasestr, and translate POSIX socket
 *     shutdown() constants (Winsock uses SD_* instead of SHUT_*).
 *
 * Include this FIRST in any file that needs compat symbols. */
#ifndef MINIPI_COMPAT_H
#define MINIPI_COMPAT_H

#if defined(_WIN32) || defined(_MSC_VER)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <basetsd.h>
  #include <errno.h>
  #include <signal.h>

  /* --- POSIX -> CRT renames ------------------------------------------------
   * NOTE: read/write/close are NOT macro-renamed (too dangerous against
   * fread/fwrite-style tokens); call sites use mp_fd_read/mp_fd_write/
   * mp_fd_close from compat_fs.h instead. */
  #define strdup      _strdup
  #define getcwd      _getcwd
  #define chdir       _chdir
  #define unlink      _unlink
  #define isatty      _isatty
  #define fileno      _fileno
  #define sleep(s)    Sleep((s)*1000u)
  typedef int mode_t;
  typedef SSIZE_T ssize_t;

  /* mkdir(path, mode) -> _mkdir(path) */
  static inline int mp_mkdir_wrap(const char *p, int m) { (void)m; return _mkdir(p); }
  #define mkdir(p, m) mp_mkdir_wrap((p), (m))

  /* sockets: WSAStartup handled by mp_net_init() (compat.c). Call it at every
   * process entry point that touches sockets. */
  #define MP_NET_INIT() mp_net_init()

  /* shutdown() constants */
  #ifndef SHUT_RD
    #define SHUT_RD   SD_RECEIVE
    #define SHUT_WR   SD_SEND
    #define SHUT_RDWR SD_BOTH
  #endif
  /* BSD socket fd type maps to SOCKET; our code stores them as int fds and
   * calls close()/read()/write() via the mp_fd_* wrappers or closesocket().
   * Provide a safe close for sockets too: Winsock requires closesocket(), but
   * _close on an INVALID handle just fails harmlessly. We give call sites an
   * explicit helper. */
  #define MP_CLOSESOCK(s) closesocket((SOCKET)(intptr_t)(s))

  /* errno names */
  #ifndef EINTR
    #define EINTR 4
  #endif
  #ifndef ETIMEDOUT
    #define ETIMEDOUT 10060
  #endif

  /* signals used by our code: SIGKILL doesn't exist on Windows; map to SIGTERM
   * (TerminateProcess path in mp_spawn_capture covers hard kills). */
  #ifndef SIGKILL
    #define SIGKILL SIGTERM
  #endif

  /* sig_atomic_t comes from signal.h above. */

  /* --- missing string functions (inline, simple) --------------------------- */
  static inline void *memmem(const void *hay, size_t hl, const void *needle, size_t nl) {
      if (!nl) return (void *)hay;
      if (hl < nl) return NULL;
      const unsigned char *h = (const unsigned char *)hay;
      for (size_t i = 0; i + nl <= hl; i++)
          if (h[i] == ((const unsigned char *)needle)[0] && memcmp(h + i, needle, nl) == 0)
              return (void *)(h + i);
      return NULL;
  }
  static inline int strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
  static inline int strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
  static inline char *strcasestr(const char *hay, const char *needle) {
      size_t nl = strlen(needle);
      for (; *hay; hay++)
          if (_strnicmp(hay, needle, nl) == 0) return (char *)hay;
      return NULL;
  }

#else /* !Windows */
  /* One knob to rule them all: expose POSIX.1-2008 + XSI symbols
   * (strdup, strcasecmp, fsync, getline, ...) without GNU-only extras. */
  #ifndef _XOPEN_SOURCE
    #define _XOPEN_SOURCE 700
  #endif
  #define MP_NET_INIT() ((void)0)
  #define MP_CLOSESOCK(s) close((s))
#endif

#include "util/compat_fs.h"   /* always available: mp_getline, mp_now_mono... */

#endif /* MINIPI_COMPAT_H */
