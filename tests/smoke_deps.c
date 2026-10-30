/* tests/smoke_deps.c — prove every vendored MIT dependency compiles, links,
 * and runs. Covers: cJSON, log.c, http_parser, tomlc17, klib (khash/kvec),
 * termbox2 (link check only; no TTY required), libuv (loop + timer). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "cJSON.h"
#include "log.h"
#include "http_parser.h"
#include "tomlc17.h"
#include "termbox2.h"   /* declarations only; impl lives in the shim TU */
#include "khash.h"
#include "kvec.h"
#include "uv.h"

KHASH_MAP_INIT_STR(reg, int)

static int g_timer_fired = 0;
static void on_timer(uv_timer_t *t) { (void)t; g_timer_fired = 1; }

int main(void) {
    /* --- cJSON: parse a tool call, re-serialize it ---------------------- */
    cJSON *j = cJSON_Parse("{\"tool\":\"bash\",\"args\":{\"cmd\":\"ls\"}}");
    assert(j);
    assert(!strcmp(cJSON_GetObjectItem(j, "tool")->valuestring, "bash"));
    char *out = cJSON_PrintUnformatted(j);
    assert(out); free(out);
    cJSON_Delete(j);

    /* --- http_parser: parse an SSE-style response header ---------------- */
    http_parser p; http_parser_settings s;
    http_parser_init(&p, HTTP_RESPONSE);
    http_parser_settings_init(&s);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n";
    size_t used = http_parser_execute(&p, &s, resp, strlen(resp));
    (void)used;
    assert(p.status_code == 200);

    /* --- tomlc17: parse agent config ------------------------------------ */
    const char *cfg = "model = \"claude\"\ntemperature = 0.7\n";
    toml_result_t r = toml_parse(cfg, (int)strlen(cfg));
    assert(r.ok);
    toml_free(r);

    /* --- klib: tool registry hash map + dynamic vector ------------------ */
    kh_reg_t *h = kh_init(reg);
    int ret;
    kh_put(reg, h, "read", &ret);
    kh_put(reg, h, "edit", &ret);
    assert(kh_size(h) == 2);
    assert(kh_get(reg, h, "edit") != kh_end(h));
    kh_destroy(reg, h);

    kvec_t(int) v = {0, 0, 0};
    for (int i = 0; i < 10; i++) kv_push(int, v, i);
    assert(v.n == 10 && v.a[9] == 9);
    free(v.a);

    /* --- log.c ----------------------------------------------------------- */
    log_set_quiet(1);
    log_info("vendored deps smoke test running");

    /* --- libuv: event loop with a timer ---------------------------------- */
    uv_loop_t *loop = uv_default_loop();
    uv_timer_t tmr;
    uv_timer_init(loop, &tmr);
    uv_timer_start(&tmr, on_timer, 1, 0);
    uv_run(loop, UV_RUN_DEFAULT);
    assert(g_timer_fired);

    /* --- termbox2: link-only check (no terminal needed in CI) ------------ */
    int rc = tb_set_output_mode(TB_OUTPUT_NORMAL);
    assert(rc == TB_ERR_NOT_INIT); /* no TTY in sandbox is fine */

    printf("ALL VENDORED DEPS COMPILE AND LINK (incl. libuv, termbox2, klib)\n");
    return 0;
}
