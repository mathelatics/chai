#ifndef MINIPI_UTIL_SCHEMA_H
#define MINIPI_UTIL_SCHEMA_H
/* Minimal JSON-Schema subset validator for tool arguments: type (object,
 * string, integer, number, boolean, array), required[], properties{}, items{}.
 * Rejects unknown keys when additionalProperties:false. */
#include "cJSON.h"
/* Returns NULL on success, or a malloc'd error message (caller frees). */
char *mp_schema_validate(const cJSON *schema, const cJSON *value);
#endif
