/*
 * luacore.c — platform-independent core of the Lua host (see luacore.h).
 *
 * The scripts that run through this API execute inside the target process
 * with the full rights of that process, so the API deliberately exposes
 * the things a modder / debugger / instrumentation script actually needs:
 *
 *   print(...)          Lua's print, captured and returned to the client
 *   host.log(...)       write a line to the host's log file
 *   host.send(...)      append text to the reply sent back to the client
 *   host.sleep(ms)      sleep the host thread
 *   host.peek(addr,n)   read n bytes from the process address space
 *   host.poke(addr,s)   write bytes into the process address space
 *   host.alloc(n)       allocate RW memory (VirtualAlloc/mmap)
 *   host.free(addr,n)   release memory (VirtualFree/munmap)
 *   host.protect(a,n,m) change memory protection (m: 0/1/2/4/8)
 *   host.module(name)   base address of a loaded module
 *   host.modules()      list of {name=,base=,size=} loaded modules
 *   host.proc(mod,name) address of an exported function
 *   host.scan(pat,..)   AOB pattern scan ("48 8B ?? 05") -> address
 *   host.pid()          process id we are inside
 *   host.arch()/os()    "x64"/"x86", "windows"/"linux"
 *   host.version()      host + Lua version string
 *
 * All addresses are plain Lua integers (64-bit on every supported build),
 * so scripts can store them, do arithmetic, and pass them back in.
 */
#include "luacore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#else
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#endif

/* portable strdup (not part of C89/C99, MSVC keeps it private as _strdup) */
static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------------ */
/* growable byte buffer used for print()/send() capture               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n;
    size_t cap;
} buf_t;

static void buf_put(buf_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) return; /* drop output on OOM rather than crash */
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void buf_reset(buf_t *b) { b->n = 0; if (b->p) b->p[0] = '\0'; }

static void buf_free(buf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

struct hc_state {
    lua_State *L;
    buf_t      cap;      /* captured print()/send() output */
    hc_log_fn  logfn;
    void      *logud;
};

/* The hc_state* is stashed in the lua_State's extra space so the C
 * functions registered in the state can reach it without globals. */
static hc_state *state_of(lua_State *L) {
    hc_state **slot = (hc_state **)lua_getextraspace(L);
    return *slot;
}

/* ------------------------------------------------------------------ */
/* capture helpers                                                    */
/* ------------------------------------------------------------------ */

/* tostring() every argument and append them tab-separated + newline. */
static void append_arguments(lua_State *L) {
    hc_state *s = state_of(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t l;
        const char *str = luaL_tolstring(L, i, &l);
        if (i > 1) buf_put(&s->cap, "\t", 1);
        buf_put(&s->cap, str, l);
        lua_pop(L, 1); /* luaL_tolstring leaves its result on the stack */
    }
    buf_put(&s->cap, "\n", 1);
}

static int host_print(lua_State *L) { append_arguments(L); return 0; }
static int host_send(lua_State *L)  { append_arguments(L); return 0; }

static int host_log(lua_State *L) {
    hc_state *s = state_of(L);
    if (s->logfn) {
        /* build one line, then hand it to the callback */
        int n = lua_gettop(L);
        buf_t tmp = {0};
        for (int i = 1; i <= n; i++) {
            size_t l;
            const char *str = luaL_tolstring(L, i, &l);
            if (i > 1) buf_put(&tmp, "\t", 1);
            buf_put(&tmp, str, l);
            lua_pop(L, 1);
        }
        s->logfn(s->logud, tmp.p ? tmp.p : "");
        buf_free(&tmp);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* process memory: peek / poke / alloc / free / protect               */
/* ------------------------------------------------------------------ */

static int host_peek(lua_State *L) {
    lua_Integer base = luaL_checkinteger(L, 1);
    lua_Integer len  = luaL_checkinteger(L, 2);
    if (len < 0 || len > (64 << 20))
        return luaL_error(L, "host.peek: length out of range (0..64MB)");
    char *buf = (char *)malloc((size_t)(len ? len : 1));
    if (!buf) return luaL_error(L, "host.peek: out of memory");
#ifdef _WIN32
    SIZE_T rd = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)base,
                           buf, (SIZE_T)len, &rd)) {
        free(buf);
        lua_pushnil(L);
        lua_pushstring(L, "read failed");
        return 2;
    }
    lua_pushlstring(L, buf, (size_t)rd);
#else
    memcpy(buf, (const void *)(uintptr_t)base, (size_t)len);
    lua_pushlstring(L, buf, (size_t)len);
#endif
    free(buf);
    return 1;
}

static int host_poke(lua_State *L) {
    lua_Integer base = luaL_checkinteger(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > (64 << 20))
        return luaL_error(L, "host.poke: length out of range (0..64MB)");
#ifdef _WIN32
    SIZE_T wr = 0;
    BOOL ok = WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)base,
                                 data, (SIZE_T)len, &wr);
    lua_pushboolean(L, ok && wr == len);
#else
    memcpy((void *)(uintptr_t)base, data, len);
    lua_pushboolean(L, 1);
#endif
    return 1;
}

static int host_alloc(lua_State *L) {
    lua_Integer size = luaL_checkinteger(L, 1);
    if (size <= 0 || size > (1u << 30))
        return luaL_error(L, "host.alloc: bad size");
#ifdef _WIN32
    LPVOID p = VirtualAlloc(NULL, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE,
                            PAGE_READWRITE);
#else
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (!p) lua_pushnil(L);
    else    lua_pushinteger(L, (lua_Integer)(uintptr_t)p);
    return 1;
}

static int host_free(lua_State *L) {
    lua_Integer base = luaL_checkinteger(L, 1);
    lua_Integer size = luaL_optinteger(L, 2, 0);
#ifdef _WIN32
    BOOL ok = VirtualFree((LPVOID)(uintptr_t)base, 0, MEM_RELEASE);
    lua_pushboolean(L, ok);
#else
    lua_pushboolean(L, munmap((void *)(uintptr_t)base, (size_t)size) == 0);
#endif
    return 1;
}

static int host_protect(lua_State *L) {
    lua_Integer base = luaL_checkinteger(L, 1);
    lua_Integer size = luaL_checkinteger(L, 2);
    lua_Integer mode = luaL_checkinteger(L, 3);
#ifdef _WIN32
    static const DWORD map[9] = {
        PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, 0,
        PAGE_EXECUTE_READ, 0, 0, 0, PAGE_EXECUTE_READWRITE
    };
    if (mode < 0 || mode > 8 || map[mode] == 0)
        return luaL_error(L, "host.protect: mode must be 0,1,2,4 or 8");
    DWORD old = 0;
    BOOL ok = VirtualProtect((LPVOID)(uintptr_t)base, (SIZE_T)size,
                             map[mode], &old);
    lua_pushboolean(L, ok);
#else
    static const int map[9] = {
        PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, 0,
        PROT_READ | PROT_EXEC, 0, 0, 0,
        PROT_READ | PROT_WRITE | PROT_EXEC
    };
    if (mode < 0 || mode > 8 || map[mode] == 0)
        return luaL_error(L, "host.protect: mode must be 0,1,2,4 or 8");
    lua_pushboolean(L, mprotect((void *)(uintptr_t)base, (size_t)size,
                                map[mode]) == 0);
#endif
    return 1;
}

/* ------------------------------------------------------------------ */
/* modules and exports                                                */
/* ------------------------------------------------------------------ */

static int host_module(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
#ifdef _WIN32
    wchar_t want[128], wantdll[128 + 4];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, want, 128) == 0)
        return luaL_error(L, "host.module: bad name");
    if (wcschr(want, L'.') == NULL)
        swprintf(wantdll, 132, L"%s.dll", want);
    else
        wcscpy(wantdll, want);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                           TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { lua_pushnil(L); return 1; }
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    uintptr_t found = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (lstrcmpiW(me.szModule, want) == 0 ||
                lstrcmpiW(me.szModule, wantdll) == 0) {
                found = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (found) lua_pushinteger(L, (lua_Integer)found);
    else       lua_pushnil(L);
#else
    (void)name;
    lua_pushnil(L);
#endif
    return 1;
}

static int host_modules(lua_State *L) {
    lua_newtable(L);
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                           TH32CS_SNAPMODULE32,
                                           GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 1;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    int i = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1,
                                name, sizeof name, NULL, NULL);
            lua_pushinteger(L, ++i);
            lua_newtable(L);
            lua_pushstring(L, name);
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, (lua_Integer)(uintptr_t)me.modBaseAddr);
            lua_setfield(L, -2, "base");
            lua_pushinteger(L, (lua_Integer)me.modBaseSize);
            lua_setfield(L, -2, "size");
            lua_settable(L, -3);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
#else
    (void)0;
#endif
    return 1;
}

static int host_proc(lua_State *L) {
    const char *mod = luaL_checkstring(L, 1);
    const char *fn  = luaL_checkstring(L, 2);
#ifdef _WIN32
    HMODULE h = GetModuleHandleA(mod);
    if (!h) { lua_pushnil(L); return 1; }
    FARPROC p = GetProcAddress(h, fn);
    if (p) lua_pushinteger(L, (lua_Integer)(uintptr_t)p);
    else   lua_pushnil(L);
#else
    (void)mod; (void)fn;
    lua_pushnil(L);
#endif
    return 1;
}

/* ------------------------------------------------------------------ */
/* AOB scan                                                           */
/* ------------------------------------------------------------------ */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "48 8B ?? 05" (spaces/commas optional, "??" or nibble '?' ok).
 * Returns 0 on success; *pat and *mask are malloc'd. */
static int parse_pattern(const char *txt, unsigned char **pat,
                         unsigned char **mask, size_t *len) {
    size_t n = strlen(txt);
    unsigned char *p  = (unsigned char *)malloc(n / 2 + 2);
    unsigned char *m  = (unsigned char *)malloc(n / 2 + 2);
    if (!p || !m) { free(p); free(m); return -1; }
    int hi = -1, lo = -1;  /* -1 = missing, -2 = wildcard */
    size_t count = 0;

    for (size_t i = 0; i <= n; i++) {
        char c = txt[i];
        if (c == '\0' || c == ' ' || c == '\t' || c == ',' || c == ';' ||
            c == ':' || c == '-') {
            if (hi != -1 || lo != -1) {  /* flush a nibble */
                if (hi == -1) hi = lo, lo = -1;   /* "?F" handled below */
                if (lo == -1) lo = -2;
                p[count] = (unsigned char)((hi == -2 ? 0 : hi) << 4 |
                                           (lo == -2 ? 0 : lo));
                m[count] = (unsigned char)(((hi == -2 ? 0 : 0xF) << 4) |
                                           ((lo == -2 ? 0 : 0xF)));
                count++;
                hi = lo = -1;
            }
            if (c == '\0') break;
            continue;
        }
        int v = (c == '?') ? -2 : hexval(c);
        if (v == -1) { free(p); free(m); return -2; }  /* bad char */
        if (hi == -1) hi = v;
        else if (lo == -1) lo = v;
        else { free(p); free(m); return -2; }
    }
    *pat = p; *mask = m; *len = count;
    return count ? 0 : -1;
}

static int host_scan(lua_State *L) {
    const char *pattxt = luaL_checkstring(L, 1);
    lua_Integer start  = luaL_checkinteger(L, 2);
    lua_Integer span   = luaL_optinteger(L, 3, 0);
    if (span < 0) span = 0;

    unsigned char *pat = NULL, *mask = NULL;
    size_t plen = 0;
    if (parse_pattern(pattxt, &pat, &mask, &plen) != 0) {
        free(pat); free(mask);
        return luaL_error(L, "host.scan: bad pattern");
    }
    if (plen == 0) { free(pat); free(mask); lua_pushnil(L); return 1; }

    uintptr_t base = (uintptr_t)start;
    uintptr_t end  = span ? base + (uintptr_t)span : UINTPTR_MAX;
    if (end < base) end = UINTPTR_MAX;
    lua_Integer found = 0;

#ifdef _WIN32
    uintptr_t addr = base;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi)) break;
        uintptr_t r0 = (uintptr_t)mbi.BaseAddress;
        uintptr_t r1 = r0 + mbi.RegionSize;
        if (r0 < addr) r0 = addr;
        if (r1 > end)  r1 = end;
        DWORD prot = mbi.Protect & 0xFF;
        int readable = mbi.State == MEM_COMMIT &&
                       !(mbi.Protect & PAGE_GUARD) &&
                       (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                        prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ ||
                        prot == PAGE_EXECUTE_READWRITE ||
                        prot == PAGE_EXECUTE_WRITECOPY);
        if (readable && r1 > r0) {
            const unsigned char *rg = (const unsigned char *)r0;
            size_t rlen = (size_t)(r1 - r0);
            for (size_t i = 0; i + plen <= rlen; i++) {
                int ok = 1;
                for (size_t j = 0; j < plen; j++) {
                    if ((rg[i + j] & mask[j]) != (pat[j] & mask[j])) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) { found = (lua_Integer)(r0 + i); break; }
            }
        }
        if (found) break;
        addr = r1;
        if (mbi.RegionSize == 0) break;
    }
#else
    const unsigned char *rg = (const unsigned char *)base;
    size_t rlen = (size_t)(end == UINTPTR_MAX ? 0 : (end - base));
    for (size_t i = 0; i + plen <= rlen; i++) {
        int ok = 1;
        for (size_t j = 0; j < plen; j++) {
            if ((rg[i + j] & mask[j]) != (pat[j] & mask[j])) { ok = 0; break; }
        }
        if (ok) { found = (lua_Integer)(base + i); break; }
    }
#endif
    free(pat); free(mask);
    if (found) lua_pushinteger(L, found);
    else       lua_pushnil(L);
    return 1;
}

/* ------------------------------------------------------------------ */
/* misc                                                               */
/* ------------------------------------------------------------------ */

static int host_sleep(lua_State *L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
#endif
    return 0;
}

static int host_pid(lua_State *L) {
#ifdef _WIN32
    lua_pushinteger(L, (lua_Integer)GetCurrentProcessId());
#else
    lua_pushinteger(L, (lua_Integer)getpid());
#endif
    return 1;
}

static int host_arch(lua_State *L) {
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
    lua_pushliteral(L, "x64");
#else
    lua_pushliteral(L, "x86");
#endif
    return 1;
}

static int host_os(lua_State *L) {
#ifdef _WIN32
    lua_pushliteral(L, "windows");
#else
    lua_pushliteral(L, "linux");
#endif
    return 1;
}

static int host_version(lua_State *L) {
    lua_pushliteral(L, "lua-injector host 1.0 (Lua " LUA_VERSION_MAJOR "."
                        LUA_VERSION_MINOR "." LUA_VERSION_RELEASE ")");
    return 1;
}

/* ------------------------------------------------------------------ */
/* state lifecycle + chunk execution                                  */
/* ------------------------------------------------------------------ */

static void setfn(lua_State *L, const char *name, lua_CFunction fn) {
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

hc_state *hc_create(void) {
    hc_state *s = (hc_state *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    lua_State *L = luaL_newstate();
    if (!L) { free(s); return NULL; }
    s->L = L;
    *(hc_state **)lua_getextraspace(L) = s;

    luaL_openlibs(L);

    /* capture Lua's print() instead of writing to stdout of the target */
    lua_pushcfunction(L, host_print);
    lua_setglobal(L, "print");

    /* the host.* API table */
    lua_newtable(L);
    setfn(L, "log",     host_log);
    setfn(L, "send",    host_send);
    setfn(L, "sleep",   host_sleep);
    setfn(L, "peek",    host_peek);
    setfn(L, "poke",    host_poke);
    setfn(L, "alloc",   host_alloc);
    setfn(L, "free",    host_free);
    setfn(L, "protect", host_protect);
    setfn(L, "module",  host_module);
    setfn(L, "modules", host_modules);
    setfn(L, "proc",    host_proc);
    setfn(L, "scan",    host_scan);
    setfn(L, "pid",     host_pid);
    setfn(L, "arch",    host_arch);
    setfn(L, "os",      host_os);
    setfn(L, "version", host_version);
    lua_setglobal(L, "host");
    return s;
}

void *hc_lua(hc_state *s) { return s ? (void *)s->L : NULL; }

void hc_destroy(hc_state *s) {
    if (!s) return;
    if (s->L) lua_close(s->L);
    buf_free(&s->cap);
    free(s);
}

void hc_set_logfn(hc_state *s, hc_log_fn fn, void *ud) {
    s->logfn = fn;
    s->logud = ud;
}

void hc_log(hc_state *s, const char *line) {
    if (s && s->logfn) s->logfn(s->logud, line);
}

/* copy the current capture buffer into a fresh malloc'd NUL-terminated
 * string, then reset the capture buffer for the next chunk */
static char *take_capture(hc_state *s, size_t *outlen) {
    size_t n = s->cap.n;
    char *out = (char *)malloc(n + 1);
    if (!out) { *outlen = 0; buf_reset(&s->cap); return dup_str(""); }
    memcpy(out, s->cap.p ? s->cap.p : "", n);
    out[n] = '\0';
    *outlen = n;
    buf_reset(&s->cap);
    return out;
}

int hc_eval(hc_state *s, const char *src, size_t srclen, char **out,
            size_t *outlen) {
    lua_State *L = s->L;
    buf_reset(&s->cap);

    int rc = luaL_loadbuffer(L, src, srclen, "=(injected)");
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (!msg) msg = "(unknown error)";
        size_t l = strlen(msg);
        char *e = (char *)malloc(l + 1);
        if (e) memcpy(e, msg, l + 1);
        lua_pop(L, 1);
        *out = e ? e : dup_str("(out of memory)");
        *outlen = e ? l : 0;
        return -1;
    }
    rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        if (!msg) msg = "(unknown error)";
        size_t l = strlen(msg);
        char *e = (char *)malloc(l + 1);
        if (e) memcpy(e, msg, l + 1);
        lua_pop(L, 1);
        *out = e ? e : dup_str("(out of memory)");
        *outlen = e ? l : 0;
        return -1;
    }
    lua_settop(L, 0);
    *out = take_capture(s, outlen);
    return 0;
}

int hc_loadfile(hc_state *s, const char *path, char **out, size_t *outlen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "cannot open script file: %s", path);
        *out = dup_str(tmp);
        *outlen = strlen(tmp);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (16 << 20)) {
        fclose(f);
        *out = dup_str("script file too large or unreadable");
        *outlen = strlen(*out);
        return -1;
    }
    char *src = (char *)malloc((size_t)sz + 1);
    if (!src) {
        fclose(f);
        *out = dup_str("out of memory reading script");
        *outlen = strlen(*out);
        return -1;
    }
    size_t rd = fread(src, 1, (size_t)sz, f);
    fclose(f);
    src[rd] = '\0';
    int rc = hc_eval(s, src, rd, out, outlen);
    free(src);
    return rc;
}
