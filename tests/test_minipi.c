/* Unit tests for in-house MiniPi modules (assert-based, no framework). */
#include "util/sha256.h"
#include "util/base64.h"
#include "util/uuid.h"
#include "util/utf8.h"
#include "util/diff.h"
#include "util/schema.h"
#include "util/arena.h"
#include "net/sse.h"
#include "session/tree.h"
#include "session/compact.h"
#include "tools/registry.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void t_sha256(void){ char hx[65];
    mp_sha256_hex("abc",3,hx);
    assert(!strcmp(hx,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    mp_sha256_hex("",0,hx);
    assert(!strcmp(hx,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    printf("sha256 ok\n"); }
static void t_b64(void){ char enc[64]; unsigned char dec[64];
    int n=mp_b64_encode((const unsigned char*)"hello world",11,enc,sizeof enc);
    assert(n==16&&!strcmp(enc,"aGVsbG8gd29ybGQ="));
    int m=mp_b64_decode(enc,(size_t)n,dec,sizeof dec);
    assert(m==11&&!memcmp(dec,"hello world",11));
    printf("base64 ok\n"); }
static void t_uuid(void){ char a[37],b[37];
    assert(mp_uuid_v4(a)==0&&mp_uuid_v4(b)==0&&strcmp(a,b));
    assert(strlen(a)==36&&a[14]=='4'&&a[8]=="-"[0]);
    printf("uuid ok\n"); }
static void t_utf8(void){
    assert(mp_utf8_valid("héllo→✓",strlen("héllo→✓")));
    unsigned char bad[]={'a',0xC0,0x80,'b'};           /* overlong */
    assert(!mp_utf8_valid((char*)bad,4));
    unsigned char sur[]={'a',0xED,0xA0,0x80};          /* surrogate */
    assert(!mp_utf8_valid((char*)sur,4));
    printf("utf8 ok\n"); }
static void t_diff(void){ const char*a[]={"one","two","three"}; const char*b[]={"one","TWO","three"};
    mp_diff_hunk h[16]; int n=mp_diff_lines(a,3,b,3,h,16);
    assert(n==3); assert(h[1].op==MP_DIFF_DEL&&h[2].op==MP_DIFF_ADD);
    printf("diff ok\n"); }
static void t_schema(void){ cJSON*s=cJSON_Parse(
    "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\"}},\"additionalProperties\":false}");
    cJSON*ok=cJSON_Parse("{\"path\":\"a.txt\"}"); cJSON*bad=cJSON_Parse("{\"nope\":1}");
    assert(!mp_schema_validate(s,ok));
    char*e=mp_schema_validate(s,bad); assert(e); free(e);
    cJSON_Delete(s);cJSON_Delete(ok);cJSON_Delete(bad);
    printf("schema ok\n"); }
static void t_arena(void){ mp_arena a; mp_arena_init(&a,1024);
    char*p=mp_arena_strdup(&a,"xyz"); assert(p&&!strcmp(p,"xyz"));
    mp_arena_reset(&a); p=mp_arena_alloc(&a,64); assert(p);
    mp_arena_destroy(&a); printf("arena ok\n"); }
static int sse_hits; static char sse_last[256];
static void sse_cb(const char*ev,const char*data,void*ud){(void)ev;(void)ud;sse_hits++;snprintf(sse_last,sizeof sse_last,"%s",data);}
static void t_sse(void){ mp_sse*s=mp_sse_new(sse_cb,NULL); sse_hits=0;
    const char*raw="data: {\"a\":1}\n\nevent: ping\ndata: hi\n\ndata: line1\ndata: line2\n\n";
    mp_sse_feed(s,raw,strlen(raw)); mp_sse_free(s);
    assert(sse_hits==3); assert(!strcmp(sse_last,"line1\nline2"));
    printf("sse ok\n"); }
static void t_tree(void){ mp_tree t; mp_tree_init(&t);
    mp_tree_add(&t,"r",NULL,"user","first");
    mp_tree_add(&t,"a","r","assistant","second");
    mp_tree_add(&t,"b","r","assistant","branch");
    const mp_entry*path[8]; int n=mp_tree_path(&t,path,8);
    assert(n==2&&!strcmp(path[0]->content,"first")&&!strcmp(path[1]->content,"branch"));
    mp_tree_free(&t); printf("tree ok\n"); }
static void t_compact(void){ mp_ctx c; mp_ctx_init(&c);
    mp_ctx_push(&c,"system","sys",NULL);
    for(int i=0;i<40;i++){char buf[512];memset(buf,'x',sizeof buf);mp_ctx_push(&c,"user",buf,NULL);}
    assert(c.est_tokens>1000);
    assert(mp_compact_if_needed(&c,500));
    assert(c.n<42); mp_ctx_free(&c); printf("compact ok\n"); }
static void t_tools(void){ setenv("MINIPI_WORKSPACE","/tmp",1);
    mp_toolreg*r=mp_toolreg_default();
    cJSON*args=cJSON_Parse("{\"path\":\"../etc/passwd\"}");
    char*out=mp_tool_execute(r,"read",args); assert(out&&strstr(out,"error")); free(out);
    cJSON_Delete(args);
    args=cJSON_Parse("{\"cmd\":\"echo hi && echo rm -rf / x | grep -q . || true\"}");
    out=mp_tool_execute(r,"bash",args); assert(out&&strstr(out,"blocked")); free(out);
    cJSON_Delete(args);
    args=cJSON_Parse("{\"cmd\":\"echo hello-from-bash\"}");
    out=mp_tool_execute(r,"bash",args); assert(out&&strstr(out,"hello-from-bash")); free(out);
    cJSON_Delete(args);
    mp_toolreg_free(r); printf("tools ok\n"); }
int main(void){ t_sha256();t_b64();t_uuid();t_utf8();t_diff();t_schema();
    t_arena();t_sse();t_tree();t_compact();t_tools();
    puts("ALL TESTS PASS"); return 0; }
