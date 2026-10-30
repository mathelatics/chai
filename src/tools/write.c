#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define MP_WRITE_MAX (1024*1024)
const char *MP_SCHEMA_WRITE =
 "{\"type\":\"object\",\"required\":[\"path\",\"content\"],\"additionalProperties\":false,"
 "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}}}";
char *mp_tool_write(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(args, "content");
    if (!cJSON_IsString(p) || !cJSON_IsString(c)) return strdup("error: path/content must be strings");
    if (strlen(c->valuestring) > MP_WRITE_MAX) return strdup("error: content too large (>1MiB)");
    /* For write, the file may not exist yet: confine by canonicalizing parent dir. */
    char dirbuf[4096]; snprintf(dirbuf, sizeof dirbuf, "%s", p->valuestring);
    char *slash = strrchr(dirbuf, '/');
    const char *check;
    if (slash) { *slash = '\0'; check = dirbuf; } else { check = "."; }
    char *dir = mp_path_confine(root, check);
    if (!dir) return strdup("error: path escapes workspace");
    char full[8192]; int wr = snprintf(full, sizeof full, "%s/%s", dir, slash ? slash+1 : p->valuestring);
    free(dir);
    if (wr < 0 || wr >= (int)sizeof full) return strdup("error: path too long");
    FILE *f = fopen(full, "wb");
    if (!f) return strdup("error: cannot open for write");
    size_t n = fwrite(c->valuestring, 1, strlen(c->valuestring), f);
    int ok = (fclose(f) == 0);
    char *out = malloc(256);
    if (ok && n == strlen(c->valuestring)) snprintf(out, 256, "wrote %zu bytes to %s", n, full);
    else snprintf(out, 256, "error: short write to %s", full);
    return out;
}
