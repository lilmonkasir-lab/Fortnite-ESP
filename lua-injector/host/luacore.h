/*
 * luacore.h — platform-independent core of the Lua host.
 *
 * A lua-injector "host" is a module that gets loaded into a target process
 * and runs a private Lua 5.4 VM inside it.  This file is the part of the
 * host that has nothing Windows-specific in it: creating the Lua state,
 * running chunks, capturing `print` output, and registering the `host.*`
 * API table that scripts can use to peek/poke memory, walk loaded modules,
 * resolve exports, allocate memory, and scan for byte patterns.
 *
 * The Windows glue (DLL entry point, named-pipe server, log file) lives in
 * win_host.c on top of this.  On Linux the same file is compiled into a
 * plain test harness (test_host.c) so the Lua integration can be exercised
 * outside of Windows.
 */
#ifndef LUA_INJECTOR_LUACORE_H
#define LUA_INJECTOR_LUACORE_H

#include <stddef.h>

typedef struct hc_state hc_state;

/* Called for each line written through host.log(...).  On Windows this is
 * backed by a log file; in the Linux test harness it is stderr. */
typedef void (*hc_log_fn)(void *ud, const char *line);

/* Create a Lua state + host API.  Returns NULL on OOM. */
hc_state *hc_create(void);

/* Return the underlying lua_State (for advanced embedding / tests). */
void *hc_lua(hc_state *s);

/* Destroy the state.  Safe to call with NULL. */
void hc_destroy(hc_state *s);

/* Install the logger callback. */
void hc_set_logfn(hc_state *s, hc_log_fn fn, void *ud);

/* Log one line through the logger (no-op when no logger is installed). */
void hc_log(hc_state *s, const char *line);

/*
 * Run `srclen` bytes of Lua source as one chunk.
 * Returns 0 on success, -1 on Lua error.
 * On both paths, *out receives a freshly malloc'd, NUL-terminated string
 * (caller must free) and *outlen its length:
 *   - success: the text captured from print()/host.send() ("" if none)
 *   - error:   the error message
 */
int hc_eval(hc_state *s, const char *src, size_t srclen, char **out, size_t *outlen);

/* Read a .lua file from disk and run it (wrapper around hc_eval). */
int hc_loadfile(hc_state *s, const char *path, char **out, size_t *outlen);

#endif /* LUA_INJECTOR_LUACORE_H */
