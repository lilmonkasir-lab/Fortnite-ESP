/*
 * test_host.c — Linux test harness for the lua-injector host core.
 *
 * Exercises the exact same luacore.c that ships inside the Windows host
 * DLL, but as a plain Linux program.  Usage:
 *
 *     make build/lua-core-test
 *     ./build/lua-core-test < host/test_scripts.lua
 *
 * Input lines are evaluated as Lua chunks; lines starting with "!file "
 * load a script file.  Output is printed as:
 *     == OK ==  <captured print output>
 *     == ERR == <error message>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luacore.h"
#include "lua.h"

/* a stable, addressable buffer the test scripts can peek/poke/scan */
static char g_test_buf[128] = "HELLO_TEST_BUFFER_0123456789_ABCDEF";

static void log_cb(void *ud, const char *line) {
    (void)ud;
    fprintf(stderr, "[host.log] %s\n", line);
}

int main(void) {
    hc_state *s = hc_create();
    if (!s) {
        fprintf(stderr, "hc_create() failed\n");
        return 1;
    }
    hc_set_logfn(s, log_cb, NULL);

    /* expose the buffer's address to scripts as the global TESTBUF */
    lua_State *L = (lua_State *)hc_lua(s);
    lua_pushinteger(L, (lua_Integer)(uintptr_t)(void *)g_test_buf);
    lua_setglobal(L, "TESTBUF");

    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;

        char *out = NULL;
        size_t olen = 0;
        int rc;
        if (strncmp(line, "!file ", 6) == 0) {
            rc = hc_loadfile(s, line + 6, &out, &olen);
        } else {
            rc = hc_eval(s, line, n, &out, &olen);
        }
        printf("== %s ==\n", rc == 0 ? "OK" : "ERR");
        if (olen) fwrite(out, 1, olen, stdout);
        if (!olen || out[olen - 1] != '\n') printf("\n");
        free(out);
    }

    hc_destroy(s);
    return 0;
}
