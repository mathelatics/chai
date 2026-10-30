#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/resource.h>
#define MP_BASH_OUT_CAP (128*1024)
#define MP_BASH_TIMEOUT_S 30
const char *MP_SCHEMA_BASH =
 "{\"type\":\"object\",\"required\":[\"cmd\"],\"additionalProperties\":false,"
 "\"properties\":{\"cmd\":{\"type\":\"string\"}}}";
/* deny-list of obviously destructive patterns (policy engine M4 refines this) */
static const char *DENY[] = {"rm -rf /", "mkfs", "dd if=", NULL};
char *mp_tool_bash(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, "cmd");
    if (!cJSON_IsString(c)) return strdup("error: cmd must be a string");
    const char *cmd = c->valuestring;
    for (int i = 0; DENY[i]; i++) if (strstr(cmd, DENY[i])) return strdup("error: command blocked by policy");
    int pfd[2]; if (pipe(pfd)) return strdup("error: pipe failed");
    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return strdup("error: fork failed"); }
    if (pid == 0) {
        /* child: confine cwd, cap resources, redirect stdout/stderr */
        if (chdir(root)) _exit(127);
        dup2(pfd[1], 1); dup2(pfd[1], 2); close(pfd[0]); close(pfd[1]);
        struct rlimit cpu = { MP_BASH_TIMEOUT_S + 1, MP_BASH_TIMEOUT_S + 2 };
        struct rlimit as = { 512UL*1024*1024, 512UL*1024*1024 };
        struct rlimit of = { MP_BASH_OUT_CAP, MP_BASH_OUT_CAP };
        setrlimit(RLIMIT_CPU, &cpu); setrlimit(RLIMIT_AS, &as); setrlimit(RLIMIT_FSIZE, &of);
        execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char *buf = malloc(MP_BASH_OUT_CAP + 1);
    size_t off = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf + off, MP_BASH_OUT_CAP - off);
        if (r <= 0) break;
        off += (size_t)r;
        if (off >= MP_BASH_OUT_CAP) { kill(pid, SIGKILL); break; }
    }
    buf[off] = '\0'; close(pfd[0]);
    int status = 0; waitpid(pid, &status, 0);
    char *out = malloc(off + 128);
    snprintf(out, off + 128, "exit=%d truncated=%d\n%s",
             WIFEXITED(status) ? WEXITSTATUS(status) : -1,
             off >= MP_BASH_OUT_CAP, buf);
    free(buf);
    return out;
}
