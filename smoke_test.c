/* Compile/link smoke test for all vendored deps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "log.h"
#include "http_parser.h"
#include "tomlc17.h"

int main(void) {
    /* cJSON: build + parse an LLM-style tool-call payload */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "role", "assistant");
    char *s = cJSON_PrintUnformatted(o);
    cJSON *p = cJSON_Parse(s);
    printf("cjson ok: %s\n", cJSON_GetStringValue(cJSON_GetObjectItem(p, "role")));
    free(s); cJSON_Delete(o); cJSON_Delete(p);

    /* http_parser: parse a status line */
    http_parser hp; http_parser_init(&hp, HTTP_RESPONSE);
    const char *resp = "HTTP/1.1 200 OK\r\n";
    http_parser_settings st = {0};
    size_t n = http_parser_execute(&hp, &st, resp, strlen(resp));
    printf("http_parser ok: parsed=%zu status=%u\n", n, hp.status_code);

    /* tomlc17: parse agent config */
    const char *cfg = "model = \"pi\"\ntemperature = 0.7\n";
    toml_result_t r = toml_parse(cfg, (int)strlen(cfg));
    if (!r.ok) { printf("toml error: %s\n", r.errmsg); return 1; }
    toml_datum_t m = toml_get(r.toptab, "model");
    printf("tomlc17 ok: model=%s\n", m.type == TOML_STRING ? m.u.s : "(missing)");
    toml_free(r);

    /* log.c */
    log_set_quiet(true);
    log_info("logging subsystem linked");

    printf("ALL VENDORED DEPS COMPILE AND LINK\n");
    return 0;
}
