/* util/compat_fs.h — directory iteration + process spawn declarations used by
 * tools/, mcp/, and tui/ (C-13 cross-platform). See compat.c for impls. */
#ifndef MINIPI_COMPAT_FS_H
#define MINIPI_COMPAT_FS_H

#include <stddef.h>
#include <stdio.h>   /* FILE for mp_fsync */
#include <signal.h>   /* sig_atomic_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- monotonic seconds since an arbitrary epoch ------------------------- */
double mp_now_mono(void);

/* ---- raw fd io (POSIX read/write/close; _read/_write/_close on Win) ----- */
int  mp_fd_read(int fd, void *buf, size_t n);
int  mp_fd_write(int fd, const void *buf, size_t n);
void mp_fd_close(int fd);
void mp_net_init(void);          /* WSAStartup on Windows, no-op elsewhere */

/* ---- getline replacement (portable, heap-grown) -------------------------- */
int mp_getline(char **lineptr, size_t *n, FILE *stream);

/* ---- terminal size (fallback 80x24 when not a tty) ----------------------- */
void mp_term_size(int *cols, int *rows);

/* ---- dirent-like enumeration ----------------------------------------------
 * POSIX wraps opendir/readdir/closedir; Windows uses FindFirstFileA.
 * "." and ".." ARE returned on both platforms (callers filter them), so
 * behavior matches the historical POSIX code in ls/find/grep. */
struct mp_dent { const char *d_name; int is_dir; };
typedef struct mp_dir mp_dir;
mp_dir *mp_opendir(const char *path);
struct mp_dent *mp_readdir(mp_dir *d);      /* NULL at end/error */
void mp_closedir(mp_dir *d);

/* ---- fs primitives (portable mkdir/unlink/rename/fsync/mkdir-p) --------- */
int mp_fs_mkdir(const char *path, int mode);   /* 0 on success or EEXIST */
int mp_fs_unlink(const char *path);
int mp_fs_rename(const char *src, const char *dst);  /* replaces dst on Win too */
int mp_fsync(FILE *f);                          /* fflush + platform sync */
int mp_mkdir_p_portable(const char *path);       /* recursive create; 0 ok, -1 err */

/* ---- paths --------------------------------------------------------------- */
char *mp_getcwd_alloc(void);                 /* malloc'd, NULL on failure */
const char *mp_path_sep(void);               /* "/" or "\\" */
int mp_path_is_abs(const char *p);
/* Canonicalize rel under root; NULL if escapes root or does not exist.
 * POSIX: realpath-based (resolves symlinks). Windows: lexical + attribute check. */
char *mp_canon_under(const char *root, const char *rel);

/* ---- process spawn with fd redirection + timeout -------------------------
 * argv style (argv[0] = program). stdin_fd/stdout_fd: pass fds to wire to
 * child stdio (-1 to leave inherited). timeout_ms <= 0 => wait forever.
 * Returns 0 on success (exit_code set), -1 on spawn failure. */
int mp_spawn_capture(const char *const argv[], int stdin_fd, int stdout_fd,
                     int timeout_ms, int *exit_code);

/* ---- cooperative SIGINT (used by agent loop; replaces raw sigaction) ----- */
void mp_install_sigint(volatile sig_atomic_t *flag);

/* ---- socketpair (Linux AF_UNIX; others loopback TCP pair) ---------------- */
int mp_socketpair_emul(int sv[2]);

/* ---- tty detection that works without termios.h -------------------------- */
int mp_stdin_tty(void);

#ifdef __cplusplus
}
#endif
#endif /* MINIPI_COMPAT_FS_H */
