#include "paths.h"
#include "log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv — missing include truncated getenv() to int on ILP32 => segfault */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static char g_reason[256] = "";

const char *mp_home_error(void) { return g_reason[0] ? g_reason : "unknown"; }

const char *mp_home_dir(void) {
    /* Copy env values into a static buffer: tests mutate HOME/MINIPI_HOME
       via setenv/unsetenv, which can free the string getenv() returned. */
    static char val[4096];
    g_reason[0] = '\0';
    const char *h = getenv("MINIPI_HOME");
    if (h && *h) {
        if (h[0] == '/') {
            snprintf(val, sizeof val, "%s", h);
            return val;
        }
        snprintf(g_reason, sizeof g_reason, "MINIPI_HOME set but not an absolute path: '%s'", h);
        return NULL;
    }
    h = getenv("HOME");
    if (h && *h) {
        if (h[0] == '/') {
            snprintf(val, sizeof val, "%s", h);
            return val;
        }
        snprintf(g_reason, sizeof g_reason, "HOME set but not an absolute path: '%s'", h);
        return NULL;
    }
    snprintf(g_reason, sizeof g_reason,
             "neither MINIPI_HOME nor HOME is set (test shell spawned without env?)");
    return NULL;
}

int mp_mkdir_p(const char *path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char buf[4096];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) { errno = ENAMETOOLONG; return -1; }
    for (char *p = buf + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        char save = *p; *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) return -1;
        if (save == '\0') break;
        *p = save;
    }
    return 0;
}

const char *mp_minipi_dir(void) {
    static char dir[4096];
    const char *home = mp_home_dir();
    if (!home) { log_error("paths: %s", mp_home_error()); return NULL; }
    if (snprintf(dir, sizeof dir, "%s/.minipi", home) >= (int)sizeof dir) {
        log_error("paths: home dir path too long"); return NULL;
    }
    return dir;
}
