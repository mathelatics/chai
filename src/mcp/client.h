#ifndef MINIPI_MCP_CLIENT_H
#define MINIPI_MCP_CLIENT_H
/* MCP client: JSON-RPC 2.0 over stdio to a subprocess (spawn via fork/exec).
 * Implements initialize -> tools/list -> tools/call. M5 milestone. */
#include "cJSON.h"
typedef struct mp_mcp mp_mcp;
mp_mcp *mp_mcp_spawn(const char *cmd_line);           /* e.g. "npx -y @modelcontextprotocol/server-fs" */
int     mp_mcp_initialize(mp_mcp *m);                 /* handshake; 0 ok */
cJSON  *mp_mcp_list_tools(mp_mcp *m);                /* NULL on error; caller deletes */
char   *mp_mcp_call_tool(mp_mcp *m, const char *name, const cJSON *args); /* malloc'd text result */
void    mp_mcp_shutdown(mp_mcp *m);
#endif
