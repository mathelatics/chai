#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
struct mp_mcp { pid_t pid; int in, out; int next_id; int alive; };
static int write_line(int fd, const char *s) { size_t n = strlen(s);
    while (n) { ssize_t w = write(fd, s, n); if (w <= 0) return -1; s += w; n -= (size_t)w; }
    return write(fd, "\n", 1) == 1 ? 0 : -1; }
static char *read_line(int fd) { static char buf[65536]; size_t o = 0;
    while (o + 1 < sizeof buf) { ssize_t r = read(fd, buf + o, 1); if (r <= 0) break;
        if (buf[o] == '\n') break; o++; }
    buf[o] = '\0'; return o ? buf : NULL; }
mp_mcp *mp_mcp_spawn(const char *cmd) {
    if (!cmd) return NULL;
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) return NULL;
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) { dup2(p2c[0], 0); dup2(c2p[1], 1); close(p2c[1]); close(c2p[0]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
    close(p2c[0]); close(c2p[1]);
    mp_mcp *m = calloc(1, sizeof *m);
    m->pid = pid; m->in = p2c[1]; m->out = c2p[0]; m->next_id = 1; m->alive = 1;
    return m;
}
static int rpc(mp_mcp *m, const char *method, const cJSON *params, cJSON **result) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(req, "id", m->next_id++);
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", cJSON_Duplicate(params, 1));
    char *s = cJSON_PrintUnformatted(req); cJSON_Delete(req);
    if (!s) return -1;
    int rc = write_line(m->in, s); free(s);
    if (rc) return -1;
    char *line = read_line(m->out); if (!line) return -1;
    cJSON *resp = cJSON_Parse(line); if (!resp) return -1;
    cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    if (err) { cJSON_Delete(resp); return -2; }
    if (result) *result = cJSON_DetachItemFromObjectCaseSensitive(resp, "result");
    cJSON_Delete(resp);
    return 0;
}
int mp_mcp_initialize(mp_mcp *m) {
    if (!m || !m->alive) return -1;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_CreateObject(); cJSON_AddItemToObject(p, "capabilities", caps);
    cJSON_AddItemToObject(p, "clientInfo", cJSON_CreateObject());
    cJSON *res = NULL; int rc = rpc(m, "initialize", p, &res);
    if (res) cJSON_Delete(res); cJSON_Delete(p);
    if (rc) return rc;
    write_line(m->in, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    return 0;
}
cJSON *mp_mcp_list_tools(mp_mcp *m) {
    cJSON *res = NULL;
    if (rpc(m, "tools/list", NULL, &res) || !res) { if (res) cJSON_Delete(res); return NULL; }
    cJSON *tools = cJSON_DetachItemFromObjectCaseSensitive(res, "tools");
    cJSON_Delete(res);
    return tools;
}
char *mp_mcp_call_tool(mp_mcp *m, const char *name, const cJSON *args) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddItemToObject(p, "arguments", args ? cJSON_Duplicate(args, 1) : cJSON_CreateObject());
    cJSON *res = NULL;
    if (rpc(m, "tools/call", p, &res) || !res) { cJSON_Delete(p); return strdup("error: mcp call failed"); }
    cJSON_Delete(p);
    char *txt = strdup("");
    cJSON *content = cJSON_DetachItemFromObjectCaseSensitive(res, "content");
    if (cJSON_IsArray(content) && content->child) {
        const cJSON *tx = cJSON_GetObjectItemCaseSensitive(content->child, "text");
        if (cJSON_IsString(tx)) { free(txt); txt = strdup(tx->valuestring); }
    }
    if (content) cJSON_Delete(content);
    cJSON_Delete(res);
    return txt;
}
void mp_mcp_shutdown(mp_mcp *m) { if (!m) return;
    close(m->in); close(m->out); m->alive = 0;
    int st; kill(m->pid, 15); waitpid(m->pid, &st, 0); free(m); }
