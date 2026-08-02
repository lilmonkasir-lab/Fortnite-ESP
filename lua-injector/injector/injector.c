/*
 * injector.c — lua-injector console client (built on client/common.c).
 *
 *   lua-injector-x64.exe --list
 *   lua-injector-x64.exe --pid <pid> [--method auto|loadlibrary|manual] ...
 *   lua-injector-x64.exe                  interactive: list + pick a PID
 *
 * Features:
 *   - process listing with bitness,
 *   - host injection: LoadLibrary remote-thread, manual map, or auto,
 *   - one-shot --eval / --script, piped stdin, or an interactive REPL,
 *   - pauses on errors and prints a crash screen so a double-clicked
 *     console window never silently vanishes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define MAX_SCRIPT_SIZE (16u << 20)

/* ================================================================== */
/* console helpers                                                    */
/* ================================================================== */

static char *wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static void print_list(cc_proc *arr, int n) {
    printf("%-9s %-5s %s\n", "PID", "BITS", "IMAGE NAME");
    for (int i = 0; i < n; i++) {
        char *name = wide_to_utf8(arr[i].name);
        printf("%-9lu %-5s %s\n",
               (unsigned long)arr[i].pid,
               arr[i].bits == 64 ? "x64" : arr[i].bits == 32 ? "x86" : "?",
               name ? name : "(?)");
        free(name);
    }
}

static int stdin_is_console(void) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

/* If we are attached to a real console (i.e. the exe was double-clicked
 * and Windows will destroy the window the moment we exit), wait for a key
 * press so the error message stays visible.  When stdin is a pipe (e.g.
 * `echo ... | lua-injector`), skip the pause so scripting still works. */
static void pause_on_exit(void) {
    if (!stdin_is_console()) return;
    printf("\nPress Enter to close this window...");
    fflush(stdout);
    int c;
    do { c = getchar(); } while (c != '\n' && c != EOF);
}

/* Last resort: if something genuinely crashes, print the exception info and
 * keep the window open so the user can read/report it. */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    fprintf(stderr,
            "\n*** lua-injector crashed ***\n"
            "exception code 0x%08lX at address 0x%llX\n",
            (unsigned long)ep->ExceptionRecord->ExceptionCode,
            (unsigned long long)(uintptr_t)ep->ExceptionRecord->
                ExceptionAddress);
    fprintf(stderr, "check lua_host_<pid>.log next to the DLL for clues\n");
    pause_on_exit();
    return EXCEPTION_EXECUTE_HANDLER;
}

static void connection_lost_hint(void) {
    printf("connection lost — the Lua host in the target stopped "
           "responding (it crashed or was unloaded), or the target "
           "process exited.\n");
    printf("  details: lua_host_<pid>.log next to the DLL (%%TEMP%% for "
           "manual maps)\n");
}

static void print_result(int rc, const char *payload, size_t plen) {
    if (rc == 0) {
        if (plen) fwrite(payload, 1, plen, stdout);
        if (!plen || payload[plen - 1] != '\n') printf("\n");
    } else {
        printf("Lua error: %s\n", payload ? payload : "(no message)");
    }
}

/* ================================================================== */
/* REPL                                                               */
/* ================================================================== */

static void repl_help(void) {
    printf(
        "  !help           show this help\n"
        "  !file <path>    run a Lua script file from disk\n"
        "  !unload         unload the Lua host from the process\n"
        "  !quit / !exit   disconnect (host stays; reconnect with --pid)\n"
        "  anything else   evaluated as Lua code in the process\n");
}

static void repl(HANDLE pipe) {
    printf("connected. type Lua code (host.* API available) or !help.\n");
    for (;;) {
        fputs("lua> ", stdout);
        fflush(stdout);
        char line[16384];
        if (!fgets(line, sizeof line, stdin)) { printf("\n"); break; }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (n == 0) continue;

        if (line[0] == '!') {
            if (strcmp(line, "!help") == 0) {
                repl_help();
            } else if (strncmp(line, "!file ", 6) == 0) {
                char *out; size_t olen;
                int rc = cc_command(pipe, "FILE", line + 6,
                                    strlen(line + 6), &out, &olen);
                if (rc < 0) { connection_lost_hint(); break; }
                print_result(rc, out, olen);
                cc_free(out);
            } else if (strcmp(line, "!unload") == 0) {
                char *out; size_t olen;
                int rc = cc_command(pipe, "EXIT", NULL, 0, &out, &olen);
                if (rc < 0) printf("connection lost\n");
                else if (rc == 0) printf("%s\n", out);
                cc_free(out);
                printf("host unloaded.\n");
                break;
            } else if (strcmp(line, "!quit") == 0 ||
                       strcmp(line, "!exit") == 0) {
                break;
            } else {
                printf("unknown command (try !help)\n");
            }
            continue;
        }

        char *out; size_t olen;
        int rc = cc_command(pipe, "EVAL", line, n, &out, &olen);
        if (rc < 0) { connection_lost_hint(); break; }
        print_result(rc, out, olen);
        cc_free(out);
    }
}

/* ================================================================== */
/* stdin piping support                                               */
/* ================================================================== */

static char *read_all_stdin(size_t *len) {
    size_t cap = 1 << 16, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= MAX_SCRIPT_SIZE) break;   /* hard cap, avoid OOM */
            size_t nc = cap * 2;
            if (nc > MAX_SCRIPT_SIZE) nc = MAX_SCRIPT_SIZE;
            char *nb = (char *)realloc(buf, nc);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap = nc;
        }
        size_t room = cap - n - 1;
        size_t rd = fread(buf + n, 1, room, stdin);
        n += rd;
        if (rd < room) break;                     /* EOF or error */
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* ================================================================== */
/* usage / main                                                       */
/* ================================================================== */

static void usage(const char *prog) {
    printf(
        "lua-injector — attach a Lua host to a running process and script it live\n"
        "\n"
        "usage:\n"
        "  %s --list\n"
        "  %s --pid <pid> [options]\n"
        "  %s                     interactive: list processes, pick a PID\n"
        "\n"
        "options:\n"
        "  --list              list running processes (pid, bits, image)\n"
        "  --pid <pid>         target process id\n"
        "  --dll <path>        host DLL to inject (default: lua-host-x64.dll /\n"
        "                      lua-host-x86.dll next to this executable)\n"
        "  --method <m>        injection method: auto (default), loadlibrary,\n"
        "                      manual\n"
        "  --script <file.lua> run a script file right after attaching\n"
        "  --eval \"<code>\"     run a one-liner right after attaching\n"
        "  --attach-only       inject the host but do not open a session\n"
        "  --stay              with --script/--eval: keep the host attached\n"
        "  --help              show this help\n"
        "\n"
        "examples:\n"
        "  %s --list\n"
        "  %s --pid 4820 --eval 'print(\"hello from\", host.os())'\n"
        "  %s --pid 4820 --script mymod.lua --stay\n"
        "  %s --pid 4820 --method manual\n"
        "  %s --pid 4820            # interactive Lua REPL\n"
        "  echo 'print(host.arch())' | %s --pid 4820\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetUnhandledExceptionFilter(crash_handler);

    int do_list = 0, have_pid = 0, attach_only = 0, stay = 0;
    DWORD pid = 0;
    int method = CC_METHOD_AUTO;
    const char *dll = NULL, *script = NULL, *eval = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = (DWORD)strtoul(argv[++i], NULL, 10);
            have_pid = 1;
        } else if (strcmp(argv[i], "--dll") == 0 && i + 1 < argc) {
            dll = argv[++i];
        } else if (strcmp(argv[i], "--method") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "loadlibrary") == 0 || strcmp(m, "load") == 0)
                method = CC_METHOD_LOADLIBRARY;
            else if (strcmp(m, "manual") == 0 || strcmp(m, "map") == 0)
                method = CC_METHOD_MANUAL;
            else if (strcmp(m, "auto") == 0)
                method = CC_METHOD_AUTO;
            else {
                fprintf(stderr, "unknown --method '%s' (auto|loadlibrary|"
                        "manual)\n", m);
                pause_on_exit();
                return 1;
            }
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            script = argv[++i];
        } else if (strcmp(argv[i], "--eval") == 0 && i + 1 < argc) {
            eval = argv[++i];
        } else if (strcmp(argv[i], "--attach-only") == 0) {
            attach_only = 1;
        } else if (strcmp(argv[i], "--stay") == 0) {
            stay = 1;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            pause_on_exit();
            return 1;
        }
    }

    if (do_list) {
        cc_proc *arr;
        int n;
        if (cc_list_processes(&arr, &n) != 0) {
            fprintf(stderr, "could not snapshot the process list\n");
            pause_on_exit();
            return 1;
        }
        print_list(arr, n);
        cc_free_processes(arr);
        return 0;
    }

    if (!have_pid) {
        cc_proc *arr;
        int n;
        if (cc_list_processes(&arr, &n) == 0) {
            print_list(arr, n);
            cc_free_processes(arr);
        }
        printf("\nEnter PID to attach to: ");
        fflush(stdout);
        char buf[64];
        if (!fgets(buf, sizeof buf, stdin)) {
            pause_on_exit();
            return 1;
        }
        pid = (DWORD)strtoul(buf, NULL, 10);
        if (pid == 0) {
            fprintf(stderr, "no PID given\n");
            pause_on_exit();
            return 1;
        }
    }

    /* ---- choose host DLL ---- */
    int tbits = cc_process_bits(pid);
    if (!tbits) {
        fprintf(stderr,
                "error: cannot open process %lu (access denied — run as "
                "administrator, or the process is protected)\n",
                (unsigned long)pid);
        pause_on_exit();
        return 1;
    }
    char dllbuf[MAX_PATH];
    if (!dll) {
        if (!GetModuleFileNameA(NULL, dllbuf, MAX_PATH)) {
            fprintf(stderr, "error: cannot find this executable's path\n");
            pause_on_exit();
            return 1;
        }
        char *slash = strrchr(dllbuf, '\\');
        if (slash) *slash = '\0';
        snprintf(dllbuf + strlen(dllbuf), sizeof dllbuf - strlen(dllbuf),
                 "\\lua-host-%s.dll", tbits == 64 ? "x64" : "x86");
        dll = dllbuf;
    }
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "error: host DLL not found: %s\n", dll);
        pause_on_exit();
        return 1;
    }

    /* ---- attach (or reuse an existing host) ---- */
    HANDLE pipe = cc_connect(pid, 3);
    if (pipe != INVALID_HANDLE_VALUE) {
        printf("host already attached to pid %lu — reconnecting\n",
               (unsigned long)pid);
    } else {
        char err[512];
        uintptr_t base = 0;
        int used = CC_METHOD_AUTO;
        int rc = cc_inject(pid, dll, method, &base, &used, err, sizeof err);
        if (rc != 0) {
            fprintf(stderr, "error: %s\n", err);
            pause_on_exit();
            return 1;
        }
        printf("injected %s into pid %lu via %s (host module at 0x%llX)\n",
               dll, (unsigned long)pid, cc_method_name(used),
               (unsigned long long)base);
        pipe = cc_connect(pid, 120);
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "error: injected, but the host pipe never "
                            "became available\n");
            pause_on_exit();
            return 1;
        }
    }

    if (attach_only) {
        printf("attached. use --pid %lu again to open a session later.\n",
               (unsigned long)pid);
        CloseHandle(pipe);
        return 0;
    }

    if (eval) {
        char *out; size_t olen;
        int rc = cc_command(pipe, "EVAL", eval, strlen(eval), &out, &olen);
        if (rc < 0) {
            connection_lost_hint();
            CloseHandle(pipe);
            pause_on_exit();
            return 1;
        }
        print_result(rc, out, olen);
        cc_free(out);
        if (!stay) {
            cc_command(pipe, "EXIT", NULL, 0, &out, &olen);
            cc_free(out);
        }
        CloseHandle(pipe);
        return 0;
    }

    if (script) {
        char *out; size_t olen;
        int rc = cc_command(pipe, "FILE", script, strlen(script),
                            &out, &olen);
        if (rc < 0) {
            connection_lost_hint();
            CloseHandle(pipe);
            pause_on_exit();
            return 1;
        }
        print_result(rc, out, olen);
        cc_free(out);
        if (!stay) {
            cc_command(pipe, "EXIT", NULL, 0, &out, &olen);
            cc_free(out);
        }
        CloseHandle(pipe);
        return 0;
    }

    if (!stdin_is_console()) {
        size_t n;
        char *src = read_all_stdin(&n);
        if (!src) {
            fprintf(stderr, "could not read stdin\n");
            CloseHandle(pipe);
            pause_on_exit();
            return 1;
        }
        char *out; size_t olen;
        int rc = cc_command(pipe, "EVAL", src, n, &out, &olen);
        cc_free(src);
        if (rc < 0) {
            connection_lost_hint();
            CloseHandle(pipe);
            pause_on_exit();
            return 1;
        }
        print_result(rc, out, olen);
        cc_free(out);
        if (!stay) {
            cc_command(pipe, "EXIT", NULL, 0, &out, &olen);
            cc_free(out);
        }
        CloseHandle(pipe);
        return 0;
    }

    repl(pipe);
    CloseHandle(pipe);
    printf("disconnected (host keeps running in pid %lu; "
           "reconnect with --pid %lu)\n", (unsigned long)pid,
           (unsigned long)pid);
    return 0;
}
