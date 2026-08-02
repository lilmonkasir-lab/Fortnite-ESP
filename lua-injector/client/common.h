/*
 * common.h — shared client core for lua-injector.
 *
 * Everything both the console client (injector.c) and the GUI client
 * (gui.c) need: process enumeration, host DLL injection (two methods:
 * classic LoadLibrary remote-thread and manual mapping, with auto
 * fallback), and the named-pipe protocol client.
 *
 * No console output in here — errors go into caller-provided buffers so
 * the GUI can display them and the console tool can print them.
 */
#ifndef LUA_INJECTOR_CLIENT_COMMON_H
#define LUA_INJECTOR_CLIENT_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* process enumeration                                              */
/* ---------------------------------------------------------------- */

typedef struct cc_proc {
    DWORD   pid;
    DWORD   ppid;
    int     bits;              /* 32 or 64; 0 if we could not open it */
    wchar_t name[256];
} cc_proc;

/* returns 0 on success; *out must be freed with cc_free_processes */
int  cc_list_processes(cc_proc **out, int *count);
void cc_free_processes(cc_proc *arr);

/* bitness of a pid: 32/64, or 0 if it cannot be opened/queried */
int  cc_process_bits(DWORD pid);

/* ---------------------------------------------------------------- */
/* injection                                                        */
/* ---------------------------------------------------------------- */

enum {
    CC_METHOD_LOADLIBRARY = 1,   /* classic CreateRemoteThread(LoadLibraryA) */
    CC_METHOD_MANUAL      = 2,   /* manual map: no loader, no LoadLibrary   */
    CC_METHOD_AUTO        = 3    /* try loadlibrary, fall back to manual    */
};

const char *cc_method_name(int method);

/*
 * Inject `dll` into `pid`.  Returns 0 on success (and optionally the
 * host module base address in *base_out, plus the method that actually
 * succeeded in *method_used).  On failure returns -1 and fills err
 * (errlen bytes, NUL-terminated).
 */
int cc_inject(DWORD pid, const char *dll, int method,
              uintptr_t *base_out, int *method_used,
              char *err, size_t errlen);

/* ---------------------------------------------------------------- */
/* pipe protocol client                                             */
/* ---------------------------------------------------------------- */

/* connect to \\.\pipe\lua_host_<pid>, retrying up to `tries` times.
 * returns INVALID_HANDLE_VALUE on failure. */
HANDLE cc_connect(DWORD pid, int tries);

/*
 * Send one command ("EVAL"/"FILE"/"PING"/"EXIT") with `n` bytes of data.
 * *out (malloc'd, caller frees with cc_free) receives the reply payload.
 * Returns 0 = OK, 1 = Lua error (payload is the message), -1 = connection
 * lost.
 */
int cc_command(HANDLE pipe, const char *cmd, const void *data, size_t n,
               char **out, size_t *olen);

void cc_free(char *p);

#endif /* LUA_INJECTOR_CLIENT_COMMON_H */
