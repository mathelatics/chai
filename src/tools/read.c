#include "tools/registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MP_READ_MAX (256*1024)
const char *MP_SCHEMA_READ =
 "{\"type\":\"object\",\"required\":[\"path\"],\"additionalProperties\":false,"
 "\"properties\":{\"path\":{\"type\":\"string\"}}}";
char *mp_tool_read(const cJSON *args, void *ud) {
    const char *root = ud;
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(args, "path");
    if (!cJSON_IsString(p)) return strdup("error: path must be a string");
    char *canon = mp_path_confine(root, p->valuestring);
    if (!canon) return strdup("error: path escapes workspace or does not exist");
    FILE *f = fopen(canon, "rb");
    if (!f) { free(canon); return strdup("error: cannot open file"); }
    char *buf = malloc(MP_READ_MAX + 1);
    size_t n = fread(buf, 1, MP_READ_MAX, f);
    fclose(f);
    buf[n] = '\0';
    char *out = malloc(n + strlen(canon) + 32);
    sprintf(out, "%s (%zu bytes)\n%s", canon, n, buf);
    free(buf); free(canon);
    return out;
}
