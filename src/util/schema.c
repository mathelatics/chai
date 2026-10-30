#include "schema.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static char *err(const char *fmt,...){ char *b=malloc(256); if(!b)return NULL;
    va_list ap;(void)ap; snprintf(b,256,"%s",fmt); return b; }
static int type_ok(const char *t,const cJSON *v){
    if(!strcmp(t,"object"))return cJSON_IsObject(v);
    if(!strcmp(t,"string"))return cJSON_IsString(v);
    if(!strcmp(t,"integer"))return cJSON_IsNumber(v)&&v->valuedouble==(double)(long long)v->valuedouble;
    if(!strcmp(t,"number"))return cJSON_IsNumber(v);
    if(!strcmp(t,"boolean"))return cJSON_IsBool(v);
    if(!strcmp(t,"array"))return cJSON_IsArray(v);
    return 0; }
char *mp_schema_validate(const cJSON *s,const cJSON *v){
    if(!s||!v)return err("null schema/value");
    const cJSON *ty=cJSON_GetObjectItemCaseSensitive(s,"type");
    if(cJSON_IsString(ty)&&!type_ok(ty->valuestring,v))return err("type mismatch");
    if(cJSON_IsObject(v)){
        const cJSON *req=cJSON_GetObjectItemCaseSensitive(s,"required");
        if(cJSON_IsArray(req)){const cJSON*r=NULL;
            cJSON_ArrayForEach(r,req)if(cJSON_IsString(r)&&!cJSON_GetObjectItemCaseSensitive(v,r->valuestring))
                return err("missing required property");}
        const cJSON *props=cJSON_GetObjectItemCaseSensitive(s,"properties");
        const cJSON *addp=cJSON_GetObjectItemCaseSensitive(s,"additionalProperties");
        const cJSON *it=NULL;
        cJSON_ArrayForEach(it,v){
            const cJSON *psub=props?cJSON_GetObjectItemCaseSensitive(props,it->string):NULL;
            if(psub){char*e=mp_schema_validate(psub,it);if(e)return e;}
            else if(cJSON_IsFalse(addp))return err("unknown property not allowed");
        }
    }
    if(cJSON_IsArray(v)){const cJSON *items=cJSON_GetObjectItemCaseSensitive(s,"items");
        if(items){const cJSON*e=NULL;cJSON_ArrayForEach(e,v){char*r=mp_schema_validate(items,e);if(r)return r;}}}
    return NULL;
}
