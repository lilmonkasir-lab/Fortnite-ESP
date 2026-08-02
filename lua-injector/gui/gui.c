/*
 * gui.c — lua-injector GUI client (built on client/common.c).
 *
 * A Win32 window that does everything the console client does, without
 * typing:
 *
 *   - live process list (PID / bitness / image), auto-refreshed,
 *   - attach (auto / LoadLibrary / manual map) with PING verification,
 *   - multiline Lua eval box (paste whole scripts, Ctrl+Enter to run),
 *   - script-file runner with Browse (paths resolved to absolute),
 *   - live output log + status bar,
 *   - tray-balloon notifications for attach/script results,
 *   - error log file (lua-injector-gui.log next to the exe) + "Open log"
 *     button + crash handler,
 *   - "Open console REPL" button for power use.
 *
 * All pipe/injection work happens on worker threads and posts results
 * back to the UI thread (WM_APP+n) so the window never freezes.
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define _WIN32_IE    0x0600
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define GUI_CLASS L"LuaInjectorGUIW"
#define GUI_TITLE L"lua-injector GUI"
#define GUI_LOG   "lua-injector-gui.log"
#define TRAY_ID   1

/* control ids */
enum {
    IDC_LIST = 1001,
    IDC_REFRESH,
    IDC_PID_EDIT,
    IDC_PID_LABEL,
    IDC_METHOD,
    IDC_METHOD_LABEL,
    IDC_ATTACH,
    IDC_DETACH,
    IDC_EVAL_EDIT,
    IDC_EVAL_RUN,
    IDC_SCRIPT_EDIT,
    IDC_SCRIPT_BROWSE,
    IDC_SCRIPT_RUN,
    IDC_REPL,
    IDC_OPENLOG,
    IDC_OUTPUT,
    IDC_STATUS,
    IDC_DLL_LABEL
};

/* worker -> UI messages */
enum {
    WM_APP_ATTACHED = WM_APP + 1,
    WM_APP_RESULT   = WM_APP + 3,
    WM_APP_TRAY     = WM_APP + 10
};

typedef struct msg_attach {
    DWORD pid;
    int   ok;
    char  text[512];
} msg_attach;

typedef struct msg_result {
    int   rc;                  /* 0 ok, 1 lua error, -1 connection lost */
    char  tag[8];              /* "EVAL" / "FILE" / "EXIT" */
    char  text[8192];
} msg_result;

static HWND      g_list, g_pidEdit, g_method, g_evalEdit, g_scriptEdit;
static HWND      g_output, g_status;
static HWND      g_attachBtn, g_detachBtn, g_evalRun, g_scriptRun, g_replBtn;
static HANDLE    g_pipe = INVALID_HANDLE_VALUE;
static DWORD     g_pid  = 0;
static int       g_busy = 0;            /* attach in progress */
static CRITICAL_SECTION g_lock;         /* serializes pipe traffic */
static HWND      g_hwnd = NULL;         /* main window (worker targets) */
static HFONT     g_font = NULL, g_fontBold = NULL;
static NOTIFYICONDATAW g_nid;           /* tray icon for notifications */
static char      g_logpath[MAX_PATH];

/* ------------------------------------------------------------------ */
/* error log                                                          */
/* ------------------------------------------------------------------ */

static void log_line(const char *fmt, ...) {
    FILE *f = fopen(g_logpath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    log_line("*** CRASH *** exception code 0x%08lX at address 0x%llX",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             (unsigned long long)(uintptr_t)ep->ExceptionRecord->
                 ExceptionAddress);
    char buf[512];
    snprintf(buf, sizeof buf,
             "lua-injector GUI crashed.\n\n"
             "exception code: 0x%08lX\naddress: 0x%llX\n\n"
             "Details were written to:\n%s",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             (unsigned long long)(uintptr_t)ep->ExceptionRecord->
                 ExceptionAddress,
             g_logpath);
    MessageBoxA(NULL, buf, "lua-injector", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void append_output(const char *text) {
    SendMessageA(g_output, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageA(g_output, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(g_output, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageA(g_output, EM_SCROLLCARET, 0, 0);
}

static void set_status(const char *text) {
    SetWindowTextA(g_status, text);
}

static void set_busy(int busy) {
    g_busy = busy;
    EnableWindow(g_attachBtn, !busy);
    EnableWindow(g_detachBtn, !busy);
}

/* tray balloon notification */
static void notify(const char *title, const char *msg, int is_error) {
    MultiByteToWideChar(CP_UTF8, 0, title, -1,
                        g_nid.szInfoTitle, 64);
    MultiByteToWideChar(CP_UTF8, 0, msg, -1,
                        g_nid.szInfo, 256);
    g_nid.uFlags |= NIF_INFO;
    g_nid.dwInfoFlags = is_error ? NIIF_ERROR : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static char *exe_dir(char *buf, size_t n) {
    if (!GetModuleFileNameA(NULL, buf, (DWORD)n)) return NULL;
    char *s = strrchr(buf, '\\');
    if (s) *s = '\0';
    return buf;
}

static void dll_path_for(DWORD pid, char *out, size_t n) {
    char dir[MAX_PATH];
    exe_dir(dir, sizeof dir);
    int bits = cc_process_bits(pid);
    snprintf(out, n, "%s\\lua-host-%s.dll", dir,
             bits == 64 ? "x64" : "x86");
}

/* ------------------------------------------------------------------ */
/* process list                                                       */
/* ------------------------------------------------------------------ */

static void refresh_list(void) {
    cc_proc *arr = NULL;
    int n = 0;
    SendMessageA(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    if (cc_list_processes(&arr, &n) == 0) {
        for (int i = 0; i < n; i++) {
            char name[256], pidtxt[16], bitstxt[8];
            WideCharToMultiByte(CP_UTF8, 0, arr[i].name, -1,
                                name, sizeof name, NULL, NULL);
            snprintf(pidtxt, sizeof pidtxt, "%lu", (unsigned long)arr[i].pid);
            snprintf(bitstxt, sizeof bitstxt, "%s",
                     arr[i].bits == 64 ? "x64" :
                     arr[i].bits == 32 ? "x86" : "?");
            LVITEMA it;
            memset(&it, 0, sizeof it);
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = pidtxt;
            ListView_InsertItem(g_list, &it);
            ListView_SetItemText(g_list, i, 1, bitstxt);
            ListView_SetItemText(g_list, i, 2, name);
        }
        cc_free_processes(arr);
        set_status(n ? "process list refreshed" : "no processes found");
    } else {
        set_status("could not snapshot processes");
    }
    SendMessageA(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
}

static void fill_pid_from_selection(void) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return;
    char buf[64] = {0};
    LVITEMA it;
    memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT;
    it.iItem = i;
    it.iSubItem = 0;
    it.pszText = buf;
    it.cchTextMax = sizeof buf;
    if (ListView_GetItem(g_list, &it))
        SetWindowTextA(g_pidEdit, buf);
}

/* ------------------------------------------------------------------ */
/* worker threads                                                     */
/* ------------------------------------------------------------------ */

typedef struct attach_ctx { DWORD pid; int method; } attach_ctx;

static DWORD WINAPI attach_worker(LPVOID p) {
    attach_ctx *ctx = (attach_ctx *)p;
    msg_attach *ma = (msg_attach *)calloc(1, sizeof *ma);
    if (!ma) { free(ctx); return 0; }
    ma->pid = ctx->pid;

    log_line("attach: pid=%lu method=%s", (unsigned long)ctx->pid,
             cc_method_name(ctx->method));

    char dll[MAX_PATH];
    dll_path_for(ctx->pid, dll, sizeof dll);
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        snprintf(ma->text, sizeof ma->text,
                 "host DLL not found: %s", dll);
        log_line("attach FAIL pid=%lu: %s", (unsigned long)ctx->pid,
                 ma->text);
        PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
        free(ctx);
        return 0;
    }
    if (!cc_process_bits(ctx->pid)) {
        snprintf(ma->text, sizeof ma->text,
                 "cannot open pid %lu (access denied — run this GUI as "
                 "administrator, or the process is protected)",
                 (unsigned long)ctx->pid);
        log_line("attach FAIL pid=%lu: %s", (unsigned long)ctx->pid,
                 ma->text);
        PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
        free(ctx);
        return 0;
    }

    char err[512];
    uintptr_t base = 0;
    int used = CC_METHOD_AUTO;
    int rc = cc_inject(ctx->pid, dll, ctx->method, &base, &used, err,
                       sizeof err);
    if (rc != 0) {
        snprintf(ma->text, sizeof ma->text, "%s", err);
        log_line("attach FAIL pid=%lu: %s", (unsigned long)ctx->pid,
                 ma->text);
        PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
        free(ctx);
        return 0;
    }
    log_line("attach: injected pid=%lu via %s base=0x%llX",
             (unsigned long)ctx->pid, cc_method_name(used),
             (unsigned long long)base);

    /* wait for the host pipe, then verify the host is alive with PING */
    HANDLE pipe = cc_connect(ctx->pid, 300);
    if (pipe == INVALID_HANDLE_VALUE) {
        snprintf(ma->text, sizeof ma->text,
                 "injected via %s but the host pipe never appeared",
                 cc_method_name(used));
        log_line("attach FAIL pid=%lu: %s", (unsigned long)ctx->pid,
                 ma->text);
        PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
        free(ctx);
        return 0;
    }
    char *pong = NULL;
    size_t ponglen = 0;
    int pingrc = cc_command(pipe, "PING", NULL, 0, &pong, &ponglen);
    cc_free(pong);
    if (pingrc != 0) {
        snprintf(ma->text, sizeof ma->text,
                 "injected via %s but the host did not answer PING — "
                 "check lua_host_%lu.log",
                 cc_method_name(used),
                 (unsigned long)ctx->pid);
        log_line("attach FAIL pid=%lu: %s", (unsigned long)ctx->pid,
                 ma->text);
        CloseHandle(pipe);
        PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
        free(ctx);
        return 0;
    }

    EnterCriticalSection(&g_lock);
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = pipe;
    g_pid  = ctx->pid;
    LeaveCriticalSection(&g_lock);

    snprintf(ma->text, sizeof ma->text,
             "attached to pid %lu via %s (host @ 0x%llX, PING ok)%s",
             (unsigned long)ctx->pid, cc_method_name(used),
             (unsigned long long)base,
             used == CC_METHOD_MANUAL ?
             " — manual map: host log goes to %TEMP%\\lua_host_<pid>.log" :
             "");
    ma->ok = 1;
    log_line("attach OK pid=%lu", (unsigned long)ctx->pid);
    PostMessage(g_hwnd, WM_APP_ATTACHED, 0, (LPARAM)ma);
    free(ctx);
    return 0;
}

typedef struct cmd_ctx { HANDLE pipe; char *data; size_t n; char cmd[8]; } cmd_ctx;

static DWORD WINAPI command_worker(LPVOID p) {
    cmd_ctx *ctx = (cmd_ctx *)p;
    char *out = NULL;
    size_t olen = 0;
    int rc = -1;
    EnterCriticalSection(&g_lock);
    if (ctx->pipe != INVALID_HANDLE_VALUE && ctx->pipe == g_pipe)
        rc = cc_command(ctx->pipe, ctx->cmd, ctx->data, ctx->n,
                        &out, &olen);
    LeaveCriticalSection(&g_lock);

    if (strcmp(ctx->cmd, "EVAL") == 0) {
        char preview[96];
        snprintf(preview, sizeof preview, "%.80s", ctx->data ? ctx->data : "");
        log_line("eval: %.80s -> rc=%d", preview, rc);
    } else if (strcmp(ctx->cmd, "FILE") == 0) {
        log_line("script: %s -> rc=%d", ctx->data ? ctx->data : "(null)", rc);
    } else {
        log_line("cmd %s -> rc=%d", ctx->cmd, rc);
    }

    msg_result *mr = (msg_result *)calloc(1, sizeof *mr);
    if (mr) {
        strncpy(mr->tag, ctx->cmd, 7);
        mr->rc = rc;
        if (rc == 0) {
            if (strcmp(ctx->cmd, "EXIT") == 0) {
                snprintf(mr->text, sizeof mr->text, "host unloaded.\n");
            } else {
                snprintf(mr->text, sizeof mr->text, "> %s\n%.*s%s",
                         ctx->data,
                         (int)(olen > 8000 ? 8000 : olen),
                         out ? out : "",
                         (olen == 0 || out[olen - 1] == '\n') ? "" : "\n");
            }
        } else if (rc == 1) {
            snprintf(mr->text, sizeof mr->text, "Lua error: %.*s\n",
                     (int)(olen > 8000 ? 8000 : olen), out ? out : "");
        } else {
            snprintf(mr->text, sizeof mr->text,
                     "connection lost (host unloaded or process exited)\n");
        }
        PostMessage(g_hwnd, WM_APP_RESULT, 0, (LPARAM)mr);
    }
    cc_free(out);
    free(ctx->data);
    free(ctx);
    return 0;
}

typedef struct unload_ctx { HANDLE pipe; } unload_ctx;

static DWORD WINAPI unload_worker(LPVOID p) {
    unload_ctx *ctx = (unload_ctx *)p;
    char *out = NULL;
    size_t olen = 0;
    EnterCriticalSection(&g_lock);
    if (ctx->pipe == g_pipe && ctx->pipe != INVALID_HANDLE_VALUE)
        cc_command(ctx->pipe, "EXIT", NULL, 0, &out, &olen);
    if (ctx->pipe != INVALID_HANDLE_VALUE) CloseHandle(ctx->pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    g_pid = 0;
    LeaveCriticalSection(&g_lock);
    cc_free(out);
    log_line("detach: host unloaded, pipe closed");

    msg_result *mr = (msg_result *)calloc(1, sizeof *mr);
    if (mr) {
        mr->rc = 0;
        strcpy(mr->tag, "EXIT");
        snprintf(mr->text, sizeof mr->text, "host unloaded. pipe closed.\n");
        PostMessage(g_hwnd, WM_APP_RESULT, 0, (LPARAM)mr);
    }
    free(ctx);
    return 0;
}

static void start_command(const char *code) {
    cmd_ctx *ctx = (cmd_ctx *)calloc(1, sizeof *ctx);
    if (!ctx) return;
    ctx->pipe = g_pipe;
    strcpy(ctx->cmd, "EVAL");
    if (code) {
        ctx->n = strlen(code);
        ctx->data = (char *)malloc(ctx->n + 1);
        if (ctx->data) memcpy(ctx->data, code, ctx->n + 1);
    }
    HANDLE t = CreateThread(NULL, 0, command_worker, ctx, 0, NULL);
    if (t) CloseHandle(t);
}

static void run_script_file(const char *path) {
    /* resolve to an absolute path: the host resolves the path against the
     * TARGET's working directory, so a relative path would break */
    char abs[MAX_PATH];
    if (!GetFullPathNameA(path, MAX_PATH, abs, NULL))
        snprintf(abs, sizeof abs, "%s", path);
    cmd_ctx *ctx = (cmd_ctx *)calloc(1, sizeof *ctx);
    if (!ctx) return;
    ctx->pipe = g_pipe;
    strcpy(ctx->cmd, "FILE");
    ctx->n = strlen(abs);
    ctx->data = (char *)malloc(ctx->n + 1);
    if (!ctx->data) { free(ctx); return; }
    memcpy(ctx->data, abs, ctx->n + 1);
    HANDLE t = CreateThread(NULL, 0, command_worker, ctx, 0, NULL);
    if (t) CloseHandle(t);
}

/* ------------------------------------------------------------------ */
/* window proc                                                        */
/* ------------------------------------------------------------------ */

static void create_controls(HWND hwnd, HINSTANCE hInst) {
    g_font = CreateFontA(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    g_fontBold = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    HFONT font = g_font, bold = g_fontBold;

    /* process list */
    g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "SysListView32", "",
                             WS_CHILD | WS_VISIBLE | LVS_REPORT |
                             LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             12, 12, 560, 300, hwnd, (HMENU)IDC_LIST,
                             hInst, NULL);
    SendMessageA(g_list, WM_SETFONT, (WPARAM)font, TRUE);
    LVCOLUMNA col;
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = "PID";    col.cx = 80;  ListView_InsertColumn(g_list, 0, &col);
    col.pszText = "BITS";   col.cx = 60;  ListView_InsertColumn(g_list, 1, &col);
    col.pszText = "IMAGE";  col.cx = 380; ListView_InsertColumn(g_list, 2, &col);

    /* right side: actions */
    HWND lbl = CreateWindowA("STATIC", "Target process",
                             WS_CHILD | WS_VISIBLE, 584, 12, 180, 18,
                             hwnd, NULL, hInst, NULL);
    SendMessageA(lbl, WM_SETFONT, (WPARAM)bold, TRUE);

    CreateWindowA("STATIC", "PID:", WS_CHILD | WS_VISIBLE,
                  584, 36, 36, 20, hwnd, (HMENU)IDC_PID_LABEL, hInst, NULL);
    g_pidEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                WS_CHILD | WS_VISIBLE | ES_NUMBER,
                                624, 34, 100, 22, hwnd, (HMENU)IDC_PID_EDIT,
                                hInst, NULL);

    CreateWindowA("STATIC", "Method:", WS_CHILD | WS_VISIBLE,
                  584, 64, 56, 20, hwnd, (HMENU)IDC_METHOD_LABEL, hInst, NULL);
    g_method = CreateWindowA("COMBOBOX", "",
                             WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                             644, 62, 120, 120, hwnd, (HMENU)IDC_METHOD,
                             hInst, NULL);
    SendMessageA(g_method, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_method, CB_ADDSTRING, 0, (LPARAM)"auto (default)");
    SendMessageA(g_method, CB_ADDSTRING, 0, (LPARAM)"LoadLibrary");
    SendMessageA(g_method, CB_ADDSTRING, 0, (LPARAM)"Manual map");
    SendMessageA(g_method, CB_SETCURSEL, 0, 0);

    g_attachBtn = CreateWindowA("BUTTON", "Attach",
                                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                584, 92, 120, 30, hwnd, (HMENU)IDC_ATTACH,
                                hInst, NULL);
    g_detachBtn = CreateWindowA("BUTTON", "Detach (unload host)",
                                WS_CHILD | WS_VISIBLE,
                                712, 92, 152, 30, hwnd, (HMENU)IDC_DETACH,
                                hInst, NULL);
    g_replBtn = CreateWindowA("BUTTON", "Open console REPL...",
                              WS_CHILD | WS_VISIBLE,
                              584, 128, 280, 26, hwnd, (HMENU)IDC_REPL,
                              hInst, NULL);
    HWND logBtn = CreateWindowA("BUTTON", "Open error log...",
                                WS_CHILD | WS_VISIBLE,
                                584, 158, 280, 26, hwnd, (HMENU)IDC_OPENLOG,
                                hInst, NULL);
    EnableWindow(g_detachBtn, FALSE);
    EnableWindow(g_replBtn, FALSE);

    /* script row */
    CreateWindowA("STATIC", "Script:", WS_CHILD | WS_VISIBLE,
                  12, 320, 48, 20, hwnd, NULL, hInst, NULL);
    g_scriptEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   64, 318, 420, 22, hwnd,
                                   (HMENU)IDC_SCRIPT_EDIT, hInst, NULL);
    g_scriptRun = CreateWindowA("BUTTON", "Run script",
                                WS_CHILD | WS_VISIBLE,
                                492, 316, 92, 26, hwnd, (HMENU)IDC_SCRIPT_RUN,
                                hInst, NULL);
    CreateWindowA("BUTTON", "Browse...", WS_CHILD | WS_VISIBLE,
                  588, 316, 88, 26, hwnd, (HMENU)IDC_SCRIPT_BROWSE,
                  hInst, NULL);

    /* eval box: multiline so you can paste whole scripts */
    CreateWindowA("STATIC", "Lua code (paste anything, Ctrl+Enter to run):",
                  WS_CHILD | WS_VISIBLE, 12, 348, 420, 18, hwnd, NULL,
                  hInst, NULL);
    g_evalEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                 ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                                 12, 368, 620, 66, hwnd, (HMENU)IDC_EVAL_EDIT,
                                 hInst, NULL);
    SendMessageA(g_evalEdit, EM_SETLIMITTEXT, (WPARAM)(1 << 20), 0);
    g_evalRun = CreateWindowA("BUTTON", "Run",
                              WS_CHILD | WS_VISIBLE,
                              640, 368, 88, 30, hwnd, (HMENU)IDC_EVAL_RUN,
                              hInst, NULL);
    EnableWindow(g_evalRun, FALSE);
    EnableWindow(g_scriptRun, FALSE);

    /* output */
    CreateWindowA("STATIC", "Output:", WS_CHILD | WS_VISIBLE,
                  12, 444, 80, 18, hwnd, NULL, hInst, NULL);
    g_output = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                               WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                               ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                               12, 464, 852, 150, hwnd, (HMENU)IDC_OUTPUT,
                               hInst, NULL);
    SendMessageA(g_output, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_output, EM_SETLIMITTEXT, (WPARAM)(8 << 20), 0);

    /* status */
    g_status = CreateWindowA("STATIC",
                             "Pick a process (or type a PID), choose a "
                             "method, click Attach.",
                             WS_CHILD | WS_VISIBLE, 12, 622, 852, 20, hwnd,
                             (HMENU)IDC_STATUS, hInst, NULL);
    SendMessageA(g_status, WM_SETFONT, (WPARAM)font, TRUE);

    SendMessageA(g_list, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_pidEdit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_evalEdit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_scriptEdit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_attachBtn, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_detachBtn, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_evalRun, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_scriptRun, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_replBtn, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(logBtn, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(g_scriptEdit, WM_SETFONT, (WPARAM)font, TRUE);
}

static int read_pid_edit(void) {
    char buf[64];
    GetWindowTextA(g_pidEdit, buf, sizeof buf);
    return atoi(buf);
}

/* is a pid still alive? (used to explain "connection lost") */
static int process_alive(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

/* the host stopped answering: close the dead pipe, reset to detached
 * state, and explain what probably happened + where the log is */
static void handle_connection_lost(void) {
    DWORD pid = g_pid;
    EnterCriticalSection(&g_lock);
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
    g_pid = 0;
    LeaveCriticalSection(&g_lock);

    EnableWindow(g_detachBtn, FALSE);
    EnableWindow(g_evalRun, FALSE);
    EnableWindow(g_scriptRun, FALSE);
    EnableWindow(g_replBtn, FALSE);
    set_busy(0);

    char msg[512];
    if (pid && process_alive(pid)) {
        snprintf(msg, sizeof msg,
                 "connection lost: the Lua host in pid %lu stopped "
                 "responding (it crashed or was unloaded). details in "
                 "lua_host_%lu.log (next to the DLL, or %%TEMP%% for "
                 "manual maps). re-attach to start fresh.",
                 (unsigned long)pid, (unsigned long)pid);
    } else {
        snprintf(msg, sizeof msg,
                 "connection lost: the target process exited, so its host "
                 "went with it. restart the app and re-attach.");
    }
    log_line("connection lost: %s", msg);
    set_status(msg);
    append_output(msg);
    append_output("\n");
    notify("lua-injector", msg, 1);
}

static void do_attach(HWND hwnd) {
    if (g_busy) return;
    int pid = read_pid_edit();
    if (pid <= 0) {
        set_status("type a PID first (or select a row — it fills the box)");
        return;
    }
    int sel = (int)SendMessageA(g_method, CB_GETCURSEL, 0, 0);
    int method = sel == 2 ? CC_METHOD_MANUAL :
                 sel == 1 ? CC_METHOD_LOADLIBRARY : CC_METHOD_AUTO;

    set_busy(1);
    set_status("attaching...");
    attach_ctx *ctx = (attach_ctx *)malloc(sizeof *ctx);
    if (!ctx) { set_busy(0); return; }
    ctx->pid = (DWORD)pid;
    ctx->method = method;
    HANDLE t = CreateThread(NULL, 0, attach_worker, ctx, 0, NULL);
    if (!t) { free(ctx); set_busy(0); }
    else CloseHandle(t);
    (void)hwnd;
}

static void do_detach(void) {
    if (g_busy || g_pipe == INVALID_HANDLE_VALUE) return;
    set_status("unloading host...");
    set_busy(1);
    unload_ctx *ctx = (unload_ctx *)malloc(sizeof *ctx);
    if (!ctx) { set_busy(0); return; }
    ctx->pipe = g_pipe;
    HANDLE t = CreateThread(NULL, 0, unload_worker, ctx, 0, NULL);
    if (!t) { free(ctx); set_busy(0); }
    else CloseHandle(t);
}

static void do_browse_script(HWND hwnd) {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Lua scripts (*.lua)\0*.lua\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) SetWindowTextA(g_scriptEdit, path);
}

static void do_open_repl(HWND hwnd) {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        set_status("attach first, then open the REPL");
        return;
    }
    char exe[MAX_PATH], dir[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return;
    strcpy(dir, exe);
    char *s = strrchr(dir, '\\');
    if (s) *s = '\0';
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof cmd, "\"%s\" --pid %lu", exe,
             (unsigned long)g_pid);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NEW_CONSOLE, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        set_status("console REPL opened in a new window");
    } else {
        set_status("could not open the console REPL");
    }
    (void)hwnd;
}

static void do_open_log(void) {
    ShellExecuteA(NULL, "open", g_logpath, NULL, NULL, SW_SHOWNORMAL);
}

static void run_eval(void) {
    if (g_pipe == INVALID_HANDLE_VALUE) {
        set_status("attach first");
        return;
    }
    char code[1 << 20];
    int n = GetWindowTextA(g_evalEdit, code, sizeof code);
    if (n <= 0) return;
    start_command(code);
    SetWindowTextA(g_evalEdit, "");
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd, ((LPCREATESTRUCT)lp)->hInstance);
        refresh_list();
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDC_REFRESH:  refresh_list(); break;
        case IDC_ATTACH:   do_attach(hwnd); break;
        case IDC_DETACH:   do_detach(); break;
        case IDC_EVAL_RUN: run_eval(); break;
        case IDC_SCRIPT_RUN: {
            if (g_pipe == INVALID_HANDLE_VALUE) {
                set_status("attach first");
                break;
            }
            char path[MAX_PATH];
            GetWindowTextA(g_scriptEdit, path, sizeof path);
            if (!path[0]) break;
            run_script_file(path);
            break;
        }
        case IDC_SCRIPT_BROWSE: do_browse_script(hwnd); break;
        case IDC_REPL: do_open_repl(hwnd); break;
        case IDC_OPENLOG: do_open_log(); break;
        }
        return 0;
    }

    /* Ctrl+Enter in the eval box runs the code */
    case WM_KEYDOWN:
        if (wp == VK_RETURN && GetFocus() == g_evalEdit &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            run_eval();
            return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK)
            fill_pid_from_selection();
        return 0;
    }

    case WM_APP_ATTACHED: {
        msg_attach *ma = (msg_attach *)lp;
        set_busy(0);
        if (ma->ok) {
            set_status(ma->text);
            EnableWindow(g_detachBtn, TRUE);
            EnableWindow(g_evalRun, TRUE);
            EnableWindow(g_scriptRun, TRUE);
            EnableWindow(g_replBtn, TRUE);
            notify("lua-injector", ma->text, 0);
        } else {
            set_status(ma->text);
            notify("lua-injector: attach failed", ma->text, 1);
            MessageBoxA(hwnd, ma->text, "lua-injector: attach failed",
                        MB_OK | MB_ICONERROR);
        }
        append_output(ma->ok ? "== attached ==\n" : "== attach failed ==\n");
        append_output(ma->text);
        append_output("\n");
        free(ma);
        return 0;
    }

    case WM_APP_RESULT: {
        msg_result *mr = (msg_result *)lp;
        if (mr->rc == -1) {
            /* host died: reset state and explain why */
            handle_connection_lost();
            append_output(mr->text);
            free(mr);
            return 0;
        }
        if (strcmp(mr->tag, "EXIT") == 0) {
            EnableWindow(g_detachBtn, FALSE);
            EnableWindow(g_evalRun, FALSE);
            EnableWindow(g_scriptRun, FALSE);
            EnableWindow(g_replBtn, FALSE);
            set_status("detached");
        } else if (strcmp(mr->tag, "FILE") == 0) {
            if (mr->rc == 0) {
                set_status("script finished");
                notify("lua-injector", "script finished successfully", 0);
            } else if (mr->rc == 1) {
                char brief[384];
                snprintf(brief, sizeof brief, "script failed: %.300s",
                         mr->text);
                set_status(brief);
                notify("lua-injector: script failed", brief, 1);
            }
        } else { /* EVAL */
            set_status(mr->rc == 1 ? "Lua error (see output)" : "ok");
        }
        append_output(mr->text);
        free(mr);
        return 0;
    }

    case WM_APP_TRAY:
        return 0; /* tray notifications need no handling */

    case WM_CLOSE:
        if (g_pipe != INVALID_HANDLE_VALUE) {
            char *out; size_t olen;
            EnterCriticalSection(&g_lock);
            cc_command(g_pipe, "EXIT", NULL, 0, &out, &olen);
            cc_free(out);
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&g_lock);
            log_line("GUI closing: host unloaded");
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_font) DeleteObject(g_font);
        if (g_fontBold) DeleteObject(g_fontBold);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* entry                                                              */
/* ------------------------------------------------------------------ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_lock);
    SetUnhandledExceptionFilter(crash_handler);

    /* error log next to the exe */
    {
        char dir[MAX_PATH];
        exe_dir(dir, sizeof dir);
        snprintf(g_logpath, sizeof g_logpath, "%s\\%s", dir, GUI_LOG);
    }
    log_line("=== lua-injector GUI started ===");

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = GUI_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc)) {
        log_line("FATAL: RegisterClass failed (%lu)",
                 (unsigned long)GetLastError());
        DeleteCriticalSection(&g_lock);
        return 1;
    }

    HWND hwnd = CreateWindowW(GUI_CLASS, GUI_TITLE,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                              WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              892, 680, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        log_line("FATAL: CreateWindow failed (%lu)",
                 (unsigned long)GetLastError());
        DeleteCriticalSection(&g_lock);
        return 1;
    }
    g_hwnd = hwnd;

    /* tray icon (used for balloon notifications) */
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_ID;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"lua-injector");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    log_line("=== lua-injector GUI closed ===");
    DeleteCriticalSection(&g_lock);
    return (int)msg.wParam;
}
