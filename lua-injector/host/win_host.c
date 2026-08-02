/*
 * win_host.c — Windows host module (lua-host-x64.dll / lua-host-x86.dll).
 *
 * This DLL is loaded into the target process with LoadLibraryA by the
 * injector.  On DLL_PROCESS_ATTACH it spawns a single worker thread that:
 *
 *   1. creates the Lua state + host API (see luacore.c),
 *   2. writes a log file next to the DLL (lua_host_<pid>.log),
 *   3. creates the named pipe \\.\pipe\lua_host_<pid> and waits for the
 *      injector to connect,
 *   4. serves the protocol below until the client disconnects (then goes
 *      back to waiting for a new client) or sends EXIT (then unloads).
 *
 * Wire protocol (all frames are text/byte framed, no null bytes needed):
 *
 *   client -> host:
 *     "EVAL\n<dec-len>\n<len bytes of Lua source>\n"
 *     "FILE\n<path>\n"
 *     "PING\n"
 *     "EXIT\n"
 *   host -> client:
 *     "OK\n<dec-len>\n<len bytes of payload>\n"
 *     "ERR\n<dec-len>\n<len bytes of payload>\n"
 *
 *   payload is the captured print()/host.send() output on OK, and the
 *   error message on ERR.
 *
 * Only one host instance is allowed per process (the pipe name embeds the
 * PID).  If the DLL is loaded a second time, the second worker thread logs
 * an error and exits without doing anything.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luacore.h"

static const char *pipe_name_fmt = "\\\\.\\pipe\\lua_host_%lu";

/* Set to 1 by the injector's manual mapper BEFORE the entry point runs.
 * The host uses it to avoid loader-only operations (DisableThreadLibrary-
 * Calls, FreeLibrary) that are invalid for a manually mapped image.
 * Exported so the injector can find its address in the PE export table. */
__declspec(dllexport) int lua_host_manual_map = 0;

/* Name of the command currently being served (for crash logging). */
static volatile const char *g_cur_cmd = "none";

/* Id of the host worker thread (crash guard only reacts to this thread,
 * never to the target's own threads). */
static volatile DWORD g_host_tid = 0;

/* ------------------------------------------------------------------ */
/* tiny buffered pipe I/O helpers                                     */
/* ------------------------------------------------------------------ */

/* Read bytes until '\n'.  Returns bytes read (0 on EOF/error), never
 * writes more than max-1 bytes + NUL.  Strips a trailing '\r'. */
static size_t pipe_read_line(HANDLE pipe, char *dst, size_t max) {
    size_t n = 0;
    char c;
    while (n + 1 < max) {
        DWORD rd = 0;
        if (!ReadFile(pipe, &c, 1, &rd, NULL) || rd == 0) break;
        if (c == '\n') break;
        dst[n++] = c;
    }
    if (n && dst[n - 1] == '\r') n--;
    dst[n] = '\0';
    return n;
}

static int pipe_read_exact(HANDLE pipe, void *dst, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n) {
        DWORD rd = 0;
        if (!ReadFile(pipe, p, (DWORD)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n),
                      &rd, NULL) || rd == 0)
            return -1;
        p += rd;
        n -= rd;
    }
    return 0;
}

static int pipe_write_all(HANDLE pipe, const void *src, size_t n) {
    const unsigned char *p = (const unsigned char *)src;
    while (n) {
        DWORD wr = 0;
        if (!WriteFile(pipe, p, (DWORD)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n),
                       &wr, NULL) || wr == 0)
            return -1;
        p += wr;
        n -= wr;
    }
    return 0;
}

/* Send "OK\n<len>\n<payload>" or "ERR\n<len>\n<payload>". */
static void pipe_reply(HANDLE pipe, const char *status, const char *payload,
                       size_t plen) {
    char hdr[64];
    int hlen = snprintf(hdr, sizeof hdr, "%s\n%zu\n", status, plen);
    if (hlen <= 0) return;
    if (pipe_write_all(pipe, hdr, (size_t)hlen) != 0) return;
    if (plen) pipe_write_all(pipe, payload, plen);
}

/* ------------------------------------------------------------------ */
/* log file (next to the DLL)                                         */
/* ------------------------------------------------------------------ */

static void log_cb(void *ud, const char *line) {
    FILE *f = (FILE *)ud;
    if (!f) return;
    fprintf(f, "%s\n", line);
    fflush(f);
}

static FILE *open_log(HMODULE hDll) {
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(hDll, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        /* manually mapped: the base is not a real module, so write the
         * log into the temp directory instead */
        if (!GetTempPathA(sizeof dir, dir)) return NULL;
    } else {
        char *slash = strrchr(dir, '\\');
        if (!slash) return NULL;
        *slash = '\0';
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\lua_host_%lu.log", dir,
             (unsigned long)GetCurrentProcessId());
    return fopen(path, "w");
}

/* ------------------------------------------------------------------ */
/* host worker thread                                                 */
/* ------------------------------------------------------------------ */

/* The current host log file, reachable from the crash guard (the VEH
 * handler runs on the same thread as host_main). */
static FILE *g_logf = NULL;

/* Vectored exception handler: if the HOST thread hits a CPU exception
 * (e.g. a Lua C function touching a bad address), log exactly what
 * happened, then terminate just the host thread.  The target process
 * survives; the pipe handle closes; the client reports a clear
 * "connection lost" and points the user at this log.  Before this, a
 * host crash was silent and unexplained. */
static LONG CALLBACK host_veh(PEXCEPTION_POINTERS ep) {
    if (g_host_tid != 0 && GetCurrentThreadId() == g_host_tid) {
        if (g_logf) {
            fprintf(g_logf,
                    "[crash] exception 0x%08lX at 0x%llX while serving "
                    "'%s' — host thread terminating; the target keeps "
                    "running; re-attach to get a fresh host\n",
                    (unsigned long)ep->ExceptionRecord->ExceptionCode,
                    (unsigned long long)(uintptr_t)ep->ExceptionRecord->
                        ExceptionAddress,
                    g_cur_cmd);
            fflush(g_logf);
        }
        ExitThread(0); /* never return; closes the pipe -> client notice */
    }
    return EXCEPTION_CONTINUE_SEARCH; /* not ours: leave it alone */
}

/* Serve one command.  Lua errors are handled by hc_eval (pcall); a CPU
 * exception here is caught by host_veh above. */
static void serve_command(HANDLE pipe, hc_state *st, const char *cmd,
                          FILE *logf) {
    if (strcmp(cmd, "EVAL") == 0) {
        char lenbuf[32] = {0};
        if (pipe_read_line(pipe, lenbuf, sizeof lenbuf) == 0) return;
        unsigned long n = strtoul(lenbuf, NULL, 10);
        if (n > (16u << 20)) {
            pipe_reply(pipe, "ERR", "EVAL payload too large",
                       strlen("EVAL payload too large"));
            return;
        }
        char *src = (char *)malloc(n + 1);
        if (!src) {
            pipe_reply(pipe, "ERR", "out of memory", 12);
            return;
        }
        if (pipe_read_exact(pipe, src, n) != 0) {
            free(src);
            return;
        }
        src[n] = '\0';
        char *out;
        size_t olen;
        int rc = hc_eval(st, src, n, &out, &olen);
        if (rc != 0 && (!out || olen == 0)) {
            /* never send an empty error payload */
            static const char fallback[] = "(no error message)";
            pipe_reply(pipe, "ERR", fallback, sizeof fallback - 1);
        } else {
            pipe_reply(pipe, rc == 0 ? "OK" : "ERR", out, olen);
        }
        if (logf)
            fprintf(logf, "[cmd] EVAL -> %s\n", rc == 0 ? "OK" : "ERR");
        free(out);
        free(src);
    } else if (strcmp(cmd, "FILE") == 0) {
        char path[MAX_PATH] = {0};
        if (pipe_read_line(pipe, path, sizeof path) == 0) return;
        if (strlen(path) >= MAX_PATH - 1) {
            pipe_reply(pipe, "ERR", "script path too long",
                       strlen("script path too long"));
            return;
        }
        char *out;
        size_t olen;
        int rc = hc_loadfile(st, path, &out, &olen);
        if (rc != 0 && (!out || olen == 0)) {
            static const char fallback[] = "(no error message)";
            pipe_reply(pipe, "ERR", fallback, sizeof fallback - 1);
        } else {
            pipe_reply(pipe, rc == 0 ? "OK" : "ERR", out, olen);
        }
        if (logf)
            fprintf(logf, "[cmd] FILE %s -> %s\n", path,
                    rc == 0 ? "OK" : "ERR");
        free(out);
    } else if (strcmp(cmd, "PING") == 0) {
        pipe_reply(pipe, "OK", "pong", 4);
    } else if (strcmp(cmd, "EXIT") == 0) {
        pipe_reply(pipe, "OK", "host unloading", 14);
    } else {
        pipe_reply(pipe, "ERR", "unknown command",
                   strlen("unknown command"));
    }
}

static void host_main(HMODULE hDll) {
    unsigned long pid = (unsigned long)GetCurrentProcessId();
    char pipe_name[64];
    snprintf(pipe_name, sizeof pipe_name, pipe_name_fmt, pid);

    FILE *logf = open_log(hDll);
    g_logf = logf;
    hc_state *st = hc_create();
    if (!st) {
        if (logf) {
            fprintf(logf, "FATAL: hc_create failed\n");
            fclose(logf);
        }
        return;
    }
    hc_set_logfn(st, log_cb, logf);
    hc_log(st, "lua host attached");

    for (;;) {
        HANDLE pipe = CreateNamedPipeA(
            pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,                 /* one instance: one client at a time */
            1 << 20,           /* out buffer */
            1 << 20,           /* in buffer  */
            0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            hc_log(st, "pipe already exists or failed; host instance "
                       "already running?");
            break;
        }
        if (!ConnectNamedPipe(pipe, NULL)) {
            if (GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                continue;      /* nobody connected; try again */
            }
        }
        hc_log(st, "client connected");

        int keep_going = 1;
        while (keep_going) {
            char cmd[16] = {0};
            if (pipe_read_line(pipe, cmd, sizeof cmd) == 0) break; /* dc */

            g_cur_cmd = "EVAL";
            if (strcmp(cmd, "FILE") == 0) g_cur_cmd = "FILE";
            else if (strcmp(cmd, "PING") == 0) g_cur_cmd = "PING";
            else if (strcmp(cmd, "EXIT") == 0) g_cur_cmd = "EXIT";

            serve_command(pipe, st, cmd, logf);

            if (strcmp(cmd, "EXIT") == 0) keep_going = 0;
        }
        CloseHandle(pipe);
        hc_log(st, "client disconnected");
        if (!keep_going) break;
    }

    hc_log(st, "host shutting down");
    hc_destroy(st);
    if (logf) fclose(logf);
    g_logf = NULL;
    if (lua_host_manual_map)
        ExitThread(0);           /* not a real module — nothing to free */
    else
        FreeLibraryAndExitThread(hDll, 0);
}

static DWORD WINAPI host_thread(LPVOID param) {
    g_host_tid = GetCurrentThreadId();
    AddVectoredExceptionHandler(1, host_veh);
    host_main((HMODULE)param);
    return 0; /* not reached (FreeLibraryAndExitThread) */
}

BOOL WINAPI DllMain(HINSTANCE hDll, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        if (!lua_host_manual_map)
            DisableThreadLibraryCalls(hDll); /* invalid for manual maps */
        HANDLE t = CreateThread(NULL, 0, host_thread, hDll, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
