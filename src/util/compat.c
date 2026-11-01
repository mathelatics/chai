/* util/compat.c — cross-platform primitives (C-13: Linux/macOS/Windows).
 * Implements everything in compat.h that needs a TU: raw file I/O wrappers,
 * dynamic line reading, monotonic time, terminal size, and the Windows
 * implementations of POSIX-only facilities (dirent enumeration via
 * FindFirstFileA, process spawn+wait via CreateProcessA, SIGINT handler via
 * SetConsoleCtrlHandler, socketpair via loopback TCP pair).
 * On POSIX every function is a thin wrapper, so behavior is unchanged. */
#include "util/compat.h"   /* must be first: sets _XOPEN_SOURCE / winsock includes */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

/* ------------------------------------------------------------------ time */
double mp_now_mono(void) {
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    if (clock_getres(CLOCK_REALTIME, &ts) == 0) {} /* fallthrough below */
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------- file io */
int mp_fd_read(int fd, void *buf, size_t n) {
#ifdef _WIN32
    int r = _read(fd, buf, (unsigned)n > INT_MAX ? INT_MAX : (unsigned)n);
    return r;
#else
    ssize_t r = read(fd, buf, n);
    return (int)r;
#endif
}
int mp_fd_write(int fd, const void *buf, size_t n) {
#ifdef _WIN32
    return _write(fd, buf, (unsigned)n > INT_MAX ? INT_MAX : (unsigned)n);
#else
    ssize_t r = write(fd, buf, n);
    return (int)r;
#endif
}
void mp_fd_close(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}
void mp_net_init(void) {
#ifdef _WIN32
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

/* ------------------------------------------------------------ getline */
int mp_getline(char **pl, size_t *cap, FILE *f) {
    if (!pl || !cap || !f) return -1;
    char *line = *pl; size_t c = *cap;
    size_t len = 0; int ch;
    for (;;) {
        ch = fgetc(f);
        if (ch == EOF && len == 0) return -1;
        if (len + 2 > c) {
            size_t nc = c ? c * 2 : 128;
            while (nc < len + 2) nc *= 2;
            char *nb = realloc(line, nc);
            if (!nb) return -1;
            line = nb; c = nc;
        }
        if (ch == EOF) break;
        line[len++] = (char)ch;
        if (ch == '\n') break;
    }
    line[len] = '\0';
    *pl = line; *cap = c;
    return (int)len;
}

/* ------------------------------------------------------- terminal size */
void mp_term_size(int *cols, int *rows) {
    int c = 80, r = 24;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#elif defined(TIOCGWINSZ)
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { c = ws.ws_col; r = ws.ws_row; }
#endif
    if (cols) *cols = c;
    if (rows) *rows = r;
}

/* =======================================================================
 *                       WINDOWS IMPLEMENTATIONS
 * ======================================================================= */
#ifdef _WIN32

/* ---- directory iteration (FindFirstFileA/FindNextFileA -> dirent-like) -- */
struct mp_dir {
    HANDLE h;
    struct mp_dent ent;
    int first;      /* next call returns the entry found by open() */
    int done;
};

mp_dir *mp_opendir(const char *path) {
    if (!path) return NULL;
    mp_dir *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    char pat[MAX_PATH * 2];
    if (snprintf(pat, sizeof pat, "%s\\*", path) >= (int)sizeof pat) { free(d); return NULL; }
    WIN32_FIND_DATAA fd;
    d->h = FindFirstFileA(pat, &fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    /* store first name in finddata; copy on demand */
    d->ent.d_name = strdup(fd.cFileName);
    d->ent.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    /* skip "." from the very first hit like POSIX code expects? No: callers
     * filter themselves (they compare against "."/".."). Keep parity. */
    return d;
}
struct mp_dent *mp_readdir(mp_dir *d) {
    if (!d) return NULL;
    if (d->done) return NULL;
    if (d->first) { d->first = 0; return d->ent.d_name ? &d->ent : NULL; }
    WIN32_FIND_DATAA fd;
    for (;;) {
        if (!FindNextFileA(d->h, &fd)) { d->done = 1; return NULL; }
        free(d->ent.d_name);
        d->ent.d_name = strdup(fd.cFileName);
        d->ent.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return &d->ent;
    }
}
void mp_closedir(mp_dir *d) {
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d->ent.d_name);
    free(d);
}

/* ---- paths -------------------------------------------------------------- */
char *mp_getcwd_alloc(void) {
    char buf[MAX_PATH * 2];
    if (!_getcwd(buf, sizeof buf)) return NULL;
    return strdup(buf);
}
const char *mp_path_sep(void) { return "\\"; }
int mp_path_is_abs(const char *p) {
    if (!p || !*p) return 0;
    if (isalpha((unsigned char)p[0]) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return 1;
    return p[0] == '\\' || p[0] == '/';
}

/* Lexical canonicalization under root (no symlinks resolved; Windows dev
 * machines rarely have them enabled without admin). Returns malloc'd path
 * with native separators, or NULL if it escapes root or doesn't exist. */
static void norm_seps(char *s) { for (; *s; s++) if (*s == '/') *s = '\\'; }
char *mp_canon_under(const char *root, const char *rel) {
    if (!root || !rel || !*rel) return NULL;
    char save_root[MAX_PATH * 2];
    snprintf(save_root, sizeof save_root, "%s", root); norm_seps(save_root);
    /* strip trailing sep(s) from root (but keep bare "C:\") */
    size_t rl = strlen(save_root);
    while (rl > 3 && save_root[rl - 1] == '\\') save_root[--rl] = 0;

    /* normalize root lexically too (it may contain . / ..) */
    char rootn[MAX_PATH * 2];
    {
        char *parts[64]; int np = 0;
        char tmp[MAX_PATH * 2]; snprintf(tmp, sizeof tmp, "%s", save_root);
        char *pfx = NULL, *cur = tmp;
        if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':' && tmp[2] == '\\') {
            char sv2 = tmp[2]; tmp[2] = 0; pfx = strdup(tmp); tmp[2] = sv2; cur = tmp + 3;
        } else if (tmp[0] == '\\') { pfx = strdup("\\"); cur = tmp + 1; }
        for (char *tok = strtok(cur, "\\"); tok; tok = strtok(NULL, "\\")) {
            if (!strcmp(tok, ".")) continue;
            if (!strcmp(tok, "..")) { if (np) np--; continue; }
            if (np < 64) parts[np++] = tok;
        }
        size_t off = 0; rootn[0] = 0;
        if (pfx) { off = (size_t)snprintf(rootn, sizeof rootn, "%s", pfx); free(pfx); }
        for (int i = 0; i < np; i++)
            off += (size_t)snprintf(rootn + off, sizeof rootn - off, "%s%s", i ? "\\" : "", parts[i]);
        if (off == 0) snprintf(rootn, sizeof rootn, "\\");
    }

    char joined[MAX_PATH * 4];
    if (mp_path_is_abs(rel)) {
        if (snprintf(joined, sizeof joined, "%s", rel) >= (int)sizeof joined) return NULL;
    } else {
        if (snprintf(joined, sizeof joined, "%s\\%s", rootn, rel) >= (int)sizeof joined) return NULL;
    }
    norm_seps(joined);

    /* collapse . and .. segments lexically, preserving drive prefix */
    char *parts[128]; int np = 0;
    char tmp[MAX_PATH * 4]; snprintf(tmp, sizeof tmp, "%s", joined);
    char *pfx = NULL;
    char *cur = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':' && tmp[2] == '\\') {
        char sv = tmp[2]; tmp[2] = 0; pfx = strdup(tmp); tmp[2] = sv; cur = tmp + 3;
    } else if (tmp[0] == '\\') { pfx = strdup("\\"); cur = tmp + 1; }
    for (char *tok = strtok(cur, "\\"); tok; tok = strtok(NULL, "\\")) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        if (np >= 128) { free(pfx); return NULL; }
        parts[np++] = tok;
    }
    char rebuilt[MAX_PATH * 4]; rebuilt[0] = 0;
    size_t off = 0;
    if (pfx) { off = (size_t)snprintf(rebuilt, sizeof rebuilt, "%s", pfx); free(pfx); }
    for (int i = 0; i < np; i++)
        off += (size_t)snprintf(rebuilt + off, sizeof rebuilt - off, "%s%s", i ? "\\" : "", parts[i]);
    if (off == 0) return NULL;

    /* confinement check: must start with rootn\ (case-insensitive on Windows) */
    size_t rnl = strlen(rootn);
    if (strlen(rebuilt) <= rnl) return NULL;
    if (_strnicmp(rebuilt, rootn, (int)rnl) != 0) return NULL;
    if (rebuilt[rnl] != '\\') return NULL;

    /* existence check */
    DWORD attr = GetFileAttributesA(rebuilt);
    if (attr == INVALID_FILE_ATTRIBUTES) return NULL;
    return strdup(rebuilt);
}

/* ---- spawn + wait (replaces fork/execvp + waitpid) ---------------------- */
int mp_spawn_capture(const char *const argv[], int stdin_fd, int stdout_fd,
                     int timeout_ms, int *exit_code) {
    /* Build command line: quote args containing spaces. argv[0] is the shell
     * (caller passes cmd.exe or full program path). */
    char cmd[MAX_PATH * 8] = "";
    size_t o = 0;
    for (int i = 0; argv[i]; i++) {
        int needq = strchr(argv[i], ' ') != NULL;
        int n = needq
            ? snprintf(cmd + o, sizeof cmd - o, "%s\"%s\"", i ? " " : "", argv[i])
            : snprintf(cmd + o, sizeof cmd - o, "%s%s", i ? " " : "", argv[i]);
        if (n < 0 || o + (size_t)n >= sizeof cmd) return -1;
        o += (size_t)n;
    }
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE hin = (stdin_fd >= 0) ? (HANDLE)_get_osfhandle(stdin_fd) : NULL;
    HANDLE hout = (stdout_fd >= 0) ? (HANDLE)_get_osfhandle(stdout_fd) : NULL;
    if (hin)  { si.hStdInput  = hin;  si.dwFlags |= STARTF_USESTDHANDLES; }
    if (hout) { si.hStdOutput = hout; si.hStdError = hout; si.dwFlags |= STARTF_USESTDHANDLES; }
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    DWORD wr = WaitForSingleObject(pi.hProcess,
        timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    if (wr == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1); wr = WaitForSingleObject(pi.hProcess, 5000); }
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exit_code) *exit_code = (int)code;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}

/* ---- SIGINT cooperative interrupt --------------------------------------- */
static volatile sig_atomic_t *g_int_flag;
static BOOL WINAPI ctrl_c_handler(DWORD type) {
    if (type == CTRL_C_EVENT && g_int_flag) { *g_int_flag = 1; return TRUE; }
    return FALSE;
}
void mp_install_sigint(volatile sig_atomic_t *flag) {
    g_int_flag = flag;
    SetConsoleCtrlHandler(ctrl_c_handler, TRUE);
}

/* ---- socketpair emulation over loopback TCP (for broker self-tests) ----- */
int mp_socketpair_emul(int sv[2]) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { closesocket(ls); return -1; }
    int alen = sizeof a; getsockname(ls, (struct sockaddr *)&a, &alen);
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == INVALID_SOCKET) { closesocket(ls); return -1; }
    if (connect(c, (struct sockaddr *)&a, sizeof a)) { closesocket(c); closesocket(ls); return -1; }
    SOCKET s = accept(ls, NULL, NULL);
    closesocket(ls);
    if (s == INVALID_SOCKET) { closesocket(c); return -1; }
    sv[0] = (int)c; sv[1] = (int)s;
    return 0;
}

/* ---- tty detection helper used by input.c ------------------------------- */
int mp_stdin_tty(void) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) ? 1 : 0;
}

#else /* !Windows: POSIX implementations */

#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

struct mp_dir { DIR *d; char path[4096]; };
static const char *dirpath(mp_dir *d) { return d->path; }
mp_dir *mp_opendir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    mp_dir *m = calloc(1, sizeof *m);
    if (!m) { closedir(d); return NULL; }
    m->d = d;
    snprintf(m->path, sizeof m->path, "%s", path);
    return m;
}
struct mp_dent *mp_readdir(mp_dir *d) {
    struct dirent *de = readdir(d->d);
    if (!de) return NULL;
    static struct mp_dent out;              /* per-call reuse like POSIX readdir */
    static char namebuf[NAME_MAX + 1];
    snprintf(namebuf, sizeof namebuf, "%s", de->d_name);
    out.d_name = namebuf;
    out.is_dir = 0;
#if defined(DT_DIR)
    if (de->d_type == DT_DIR) out.is_dir = 1;
    else if (de->d_type != DT_UNKNOWN) out.is_dir = 0;
    else
#else
    /* no d_type on this platform: always stat */
    if (1)
#endif
    {   /* stat fallback (covers macOS pre-APFS quirks + network FS) */
        struct stat st;
        char full[8192];
        snprintf(full, sizeof full, "%s/%s", dirpath(d), namebuf);
        if (stat(full, &st) == 0) out.is_dir = S_ISDIR(st.st_mode);
    }
    return &out;
}
void mp_closedir(mp_dir *d) { if (!d) return; closedir(d->d); free(d); }

char *mp_getcwd_alloc(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return NULL;
    return strdup(buf);
}
const char *mp_path_sep(void) { return "/"; }
int mp_path_is_abs(const char *p) { return p && p[0] == '/'; }

char *mp_canon_under(const char *root, const char *rel) {
    if (!root || !rel || !*rel) return NULL;
    char joined[8192];
    if (rel[0] == '/') snprintf(joined, sizeof joined, "%s", rel);
    else if (snprintf(joined, sizeof joined, "%s/%s", root, rel) >= (int)sizeof joined) return NULL;
    char buf[4096];
    if (!realpath(joined, buf)) return NULL;
    size_t rl = strlen(root);
    if (strncmp(buf, root, rl) != 0 || (buf[rl] != '/' && buf[rl] != '\0')) return NULL;
    return strdup(buf);
}

int mp_spawn_capture(const char *const argv[], int stdin_fd, int stdout_fd,
                     int timeout_ms, int *exit_code) {
    if (!argv || !argv[0]) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (stdin_fd >= 0) dup2(stdin_fd, 0);
        if (stdout_fd >= 0) { dup2(stdout_fd, 1); dup2(stdout_fd, 2); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (timeout_ms > 0) {
        /* poll-based wait with deadline: kill after timeout */
        double deadline = mp_now_mono() + timeout_ms / 1000.0;
        for (;;) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) break;
            if (r < 0) { return -1; }
            if (mp_now_mono() > deadline) { kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
            usleep(20 * 1000);
        }
    } else {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

static volatile sig_atomic_t *g_posix_int_flag;
static void posix_int_handler(int sig) { (void)sig; if (g_posix_int_flag) *g_posix_int_flag = 1; }
void mp_install_sigint(volatile sig_atomic_t *flag) {
    g_posix_int_flag = flag;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = posix_int_handler;
    sigaction(SIGINT, &sa, NULL);
}

int mp_socketpair_emul(int sv[2]) {
#ifdef __linux__
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
#else
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1)) { close(ls); return -1; }
    socklen_t alen = sizeof a; getsockname(ls, (struct sockaddr *)&a, &alen);
    int c = socket(AF_INET, SOCK_STREAM, 0);
    if (c < 0) { close(ls); return -1; }
    if (connect(c, (struct sockaddr *)&a, sizeof a)) { close(c); close(ls); return -1; }
    int s = accept(ls, NULL, NULL);
    close(ls);
    if (s < 0) { close(c); return -1; }
    sv[0] = c; sv[1] = s;
    return 0;
#endif
}

int mp_stdin_tty(void) { return isatty(0); }

#endif /* _WIN32 */

/* ------------------------------------------------------- fs primitives */
int mp_fs_mkdir(const char *path, int mode) {
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) return 0;
    return -1;
#else
    if (mkdir(path, (mode_t)mode) == 0 || errno == EEXIST) return 0;
    return -1;
#endif
}
int mp_fs_unlink(const char *path) {
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : -1;      /* works even on read-only-ish; SetFileAttributes below */
#else
    return unlink(path);
#endif
}
int mp_fs_rename(const char *src, const char *dst) {
#ifdef _WIN32
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    return -1;
#else
    return rename(src, dst);
#endif
}
int mp_fsync(FILE *f) {
    if (!f) return -1;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fflush(f) ? -1 : fsync(fileno(f));
#endif
}
int mp_mkdir_p_portable(const char *path) {
    if (!path || !*path) return -1;
    char buf[4096];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return -1;
    for (char *p = buf + 1; ; p++) {
        if (*p != '/' && *p != '\\' && *p != '\0') continue;
        char save = *p; *p = '\0';
        if (buf[0] && mp_fs_mkdir(buf, 0700) != 0) return -1;
        if (save == '\0') break;
        *p = save;
    }
    return 0;
}
