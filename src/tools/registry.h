#ifndef MINIPI_TOOLS_REGISTRY_H
#define MINIPI_TOOLS_REGISTRY_H
/* Tool registry: name -> {schema, handler}. Backed by khash. All model-supplied
 * arguments pass schema + UTF-8 validation before the handler runs. */
#include "cJSON.h"
typedef struct mp_toolreg mp_toolreg;
typedef char *(*mp_tool_fn)(const cJSON *args, void *ud); /* returns malloc'd result text */
int  mp_toolreg_add(mp_toolreg *r, const char *name, const cJSON *schema /*owned*/,
                    mp_tool_fn fn, void *ud);
char *mp_tool_execute(mp_toolreg *r, const char *name, const cJSON *args);
mp_toolreg *mp_toolreg_default(void);      /* read/write/edit/bash registered */
void        mp_toolreg_free(mp_toolreg *r);
cJSON *mp_toolreg_schemas(const mp_toolreg *r); /* array for provider payloads */
/* Path confinement helper (shared by file tools): canonicalize p and ensure it
 * is inside workspace root. Returns malloc'd canonical path or NULL. */
char *mp_path_confine(const char *workspace_root, const char *p);
#endif
