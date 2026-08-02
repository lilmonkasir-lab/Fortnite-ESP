# lua-injector

Attach a private **Lua 5.4** scripting VM to any running Windows process,
then script that process live from a console — read/write its memory, walk
its loaded modules, resolve exported functions, scan for byte patterns,
allocate memory, and run arbitrary logic *inside* the process.

```
C:\lua-injector> lua-injector-x64.exe --pid 4820

  PID       BITS  IMAGE NAME
  ...
  4820      x64   notepad.exe
  ...

injected lua-host-x64.dll into pid 4820 (host module at 0x7FFC2E1A0000)
connected. type Lua code (host.* API available) or !help.
lua> print("inside", host.pid(), "on", host.os(), host.arch())
inside 4820 on windows x64
lua> print(host.module("kernel32.dll"))
140729877889024
lua> print(host.peek(host.module("kernel32.dll"), 2))
MZ
```

---

## Table of contents

1. [What it is / how it works](#how-it-works)
2. [Quick start](#quick-start)
3. [Building](#building)
4. [Command line reference](#command-line-reference)
5. [The `host.*` API reference](#the-host-api-reference)
6. [Example scripts](#example-scripts)
7. [Troubleshooting & bug fixes (with YouTube videos)](#troubleshooting--bug-fixes-with-youtube-videos)
8. [Limitations](#limitations)
9. [Ethics & tos](#ethics--tos)

---

## How it works

Four moving pieces, all in this repo:

```
┌──────────────────────────────┐        named pipe
│  lua-injector-x64.exe        │  ◄──────────────────────┐
│  (client / REPL)             │                         │
└──────────────────────────────┘                         │
        │ CreateRemoteThread(LoadLibraryA)               │
        ▼                                                 ▼
┌──────────────────────────────┐        ┌────────────────────────────┐
│  target process              │        │  lua-host-x64.dll          │
│  ┌────────────────────────┐  │  loads │  (inside the process)      │
│  │ kernel32!LoadLibraryA  │◄─┼────────┤  ┌──────────────────────┐  │
│  │ (resolved per-arch)    │  │        │  │ Lua 5.4 VM + host.*  │  │
│  └────────────────────────┘  │        │  └──────────────────────┘  │
└──────────────────────────────┘        └────────────────────────────┘
```

1. **`lua-injector-x64.exe` / `lua-injector-x86.exe`** — the console client.
   Lists processes, lets you pick a PID, injects the host DLL, and talks to
   it over a named pipe.

2. **`lua-host-x64.dll` / `lua-host-x86.dll`** — the *host*. Loaded into the
   target process via the classic `CreateRemoteThread` + `LoadLibraryA`
   trick. On `DLL_PROCESS_ATTACH` it spawns one worker thread that:
   - creates a private Lua 5.4 state (a full, untouched Lua interpreter —
     everything from `string`/`table`/`io` to `coroutine`),
   - replaces `print` so script output is captured instead of spamming the
     target's stdout,
   - installs the `host.*` table (memory, modules, exports, scan, …),
   - creates the named pipe `\\.\pipe\lua_host_<pid>` and serves the
     protocol until told to unload.

3. **`host/luacore.c`** — the platform-independent heart shared by the
   Windows DLL and the Linux test harness. This is where the `host.*` API
   lives.

4. **`injector/injector.c`** — the client side: process enumeration,
   `LoadLibraryA` resolution (including 64-bit-injector → 32-bit-target via
   the WOW64 PEB module list), injection, pipe client, REPL.

5. **`test_game/`** — the bundled **Lua Injector Test Game** (win32 GDI
   shooter). A safe, single-player target with a documented in-memory state
   struct you find by AOB-scanning its marker string — see
   [test_game/GAME.md](test_game/GAME.md). The same game logic runs
   headless on Linux (`build/game-sim-test`) so `make run-test` verifies
   it without Windows.

### The wire protocol

Simple newline-framed text (all captured output is pure text, so nothing
binary is needed for the common case):

```
client → host:   EVAL\n<len>\n<source bytes>\n
                 FILE\n<path>\n
                 PING\n
                 EXIT\n
host   → client: OK\n<len>\n<payload>\n   |   ERR\n<len>\n<message>\n
```

The payload on `OK` is everything the script printed (via `print` /
`host.send`) — so scripts behave like a normal Lua program run from a shell.

### Injection methods (`--method auto | loadlibrary | manual`)

Two injection engines are built in; `auto` (the default) tries the first
and falls back to the second automatically.

* **`loadlibrary` (classic)** — resolve `LoadLibraryA` in the target,
  write the DLL path there with `VirtualAllocEx` + `WriteProcessMemory`,
  and `CreateRemoteThread` it. Simple, reliable, and the most widely
  understood technique.
* **`manual` (manual map)** — the injector reads the DLL image from disk,
  allocates space in the target, copies headers + sections, fixes up
  relocations, resolves every import against the modules already loaded in
  the target (Toolhelp snapshot + in-target export walk), flags the host's
  `lua_host_manual_map` export, and runs `DllMain` directly at
  `AddressOfEntryPoint`. No `LoadLibrary` call, no loader involvement.
  This is the technique people usually mean by "stealth injection"; the
  host DLL supports it natively (skips loader-only calls, logs to the temp
  directory).

### Cross-architecture injection

* **same bitness** — `kernel32.dll` is loaded at the same base address in
  every process of a given bitness, so the injector resolves
  `LoadLibraryA` on itself and reuses that address in the target.
* **64-bit injector → 32-bit target** — the injector walks the target's
  WOW64 PEB (`NtQueryInformationProcess(ProcessWow64Information)`) to find
  the 32-bit `kernel32.dll`, then walks its PE export table to locate
  `LoadLibraryA`. The manual mapper handles 64→32 and same-bitness alike
  (it resolves imports against the target's own loaded modules).
* **32-bit injector → 64-bit target** — refused with a clear message; the
  reverse direction isn't possible.

---

## Quick start

**Requirements:** Windows 7+ (x64 or x86), an **Administrator** console
(plain DLL injection needs `PROCESS_CREATE_THREAD` + VM access to the
target; if the target runs elevated, so must you).

> **Tip:** run the injector from an **already-open Command Prompt**
> (Start → type `cmd` → Run as administrator), not by double-clicking the
> exe. A double-clicked console window is destroyed on exit, so errors
> vanish instantly — see **Bug #0** in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
> The injector also pauses on errors and shows a crash screen now, so
> nothing can silently disappear.

**Option A — use the prebuilt binaries** in `bin/` (built from this source,
verified x64/x86 PE32+):

```
cd lua-injector
lua-injector-x64.exe --list                 # find the PID
lua-injector-x64.exe --pid <PID>            # attach + interactive REPL
```

**Option B — attach to a fresh notepad:**

```
examples\notepad_demo.bat
```

This starts notepad, injects, dumps every module notepad has loaded
(`examples\process_info.lua`), and leaves the host inside notepad so you
can reconnect:

```
lua-injector-x64.exe --pid <notepad-pid>
```

**Option B2 — the GUI client (easiest):**

```
lua-injector-gui-x64.exe
```

A real window: a live process list (PID/bitness/image), a method picker
(**auto / LoadLibrary / manual map**), **Attach** / **Detach** buttons, a
**multiline Lua box you can paste whole scripts into** (Ctrl+Enter to
run), a script-file runner with a Browse dialog (paths are resolved to
absolute before sending), a scrolling output log, and an "Open console
REPL" button that spawns the console client in a new window. Click a
process row and it fills the PID box for you.

It keeps you informed, too:

* attach success/failure and script success/failure pop up as **Windows
  tray notifications** (attach failure also shows a message box),
* after injecting it **PINGs the host** to verify it's really alive
  before reporting "attached",
* every action and error is written to **`lua-injector-gui.log`** next to
  the exe (with timestamps) — the **Open error log...** button opens it,
* if the GUI ever crashes, it logs the exception and shows a dialog
  instead of silently dying.

**Option C — the bundled test game (best for learning):**

```
test_game\run_game_demo.bat
```

This starts the bundled **Lua Injector Test Game** (`bin\lua-test-game-x64.exe`,
an Invaders-style single-player game built specifically as a safe target),
attaches the host, and runs `test_game\scripts\game_demo.lua` — which
AOB-scans for the game's marker string, prints its live state, and writes a
message into the game window. Then try:

```
lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_set_score.lua --stay
lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_freeze_lives.lua --stay
lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_autopilot.lua --stay
```

See **[test_game/GAME.md](test_game/GAME.md)** for the game's full memory
map (offsets, the marker signature, and what each script does).

**Option D — one-liners and script files:**

```lua
lua-injector-x64.exe --pid 4820 --eval "print('hi from', host.os())"
lua-injector-x64.exe --pid 4820 --script examples\demo.lua
echo 'print(host.arch())' | lua-injector-x64.exe --pid 4820
```

The host survives client disconnects — reconnect any time with `--pid`. To
remove it from the process, send `!unload` in the REPL.

---

## Building

The toolchain is `zig cc`, which bundles a MinGW-w64 cross-compiler — so
the Windows binaries build from Linux, macOS, or Windows with zero extra
setup. A MinGW-w64 gcc works too (`make CC64=x86_64-w64-mingw32-gcc CC32=i686-w64-mingw32-gcc`),
and `build.bat` covers the MSVC route.

```
make              # Windows binaries (incl. test game) + Linux tests
make windows      # just the Windows binaries (bin/*.exe, bin/*.dll)
make run-test     # build + run the Lua core test suite + game simulation
make clean
```

Output lands in `bin/`:

| file                       | what it is                                   |
|----------------------------|----------------------------------------------|
| `lua-injector-x64.exe`     | 64-bit console client / REPL                 |
| `lua-injector-x86.exe`     | 32-bit console client / REPL                 |
| `lua-injector-gui-x64.exe` | 64-bit GUI client (list, attach, eval, scripts, log) |
| `lua-injector-gui-x86.exe` | 32-bit GUI client                            |
| `lua-host-x64.dll`         | 64-bit host module (injected into targets)   |
| `lua-host-x86.dll`         | 32-bit host module (injected into targets)   |
| `lua-test-game-x64.exe`    | bundled test game (64-bit) — safe target to script |
| `lua-test-game-x86.exe`    | bundled test game (32-bit)                   |

The console client, host DLLs, and test game import **only kernel32.dll**
(the CRT is statically linked), so they run on stock Windows with no
redistributables. The GUI additionally links the standard Windows UI
libraries (`user32`/`gdi32`/`comctl32`/`comdlg32`), which ship with every
Windows install.

---

## Command line reference

```
lua-injector-x64.exe --list
lua-injector-x64.exe --pid <pid> [options]
lua-injector-x64.exe                    # interactive: list + pick PID

options:
  --list              list running processes (pid, bits, image)
  --pid <pid>         target process id
  --dll <path>        host DLL to inject (default: lua-host-x64.dll /
                      lua-host-x86.dll next to this executable)
  --method <m>        injection method: auto (default), loadlibrary,
                      manual
  --script <file.lua> run a script file right after attaching
  --eval "<code>"     run a one-liner right after attaching
  --attach-only       inject the host but open no session
  --stay              with --script/--eval: keep the host attached
  --help              show help
```

The REPL commands:

```
lua> print("hello")        any Lua code is evaluated in the target
lua> !help                 list commands
lua> !file foo.lua         run a script file from disk
lua> !unload               unload the host from the process
lua> !quit / !exit         disconnect (host stays attached)
```

---

## The `host.*` API reference

All addresses are Lua integers, 64-bit capable on every build. Errors are
raised as Lua errors (catchable with `pcall`).

| function | description |
|---|---|
| `print(...)` | captured and returned to the client (does not touch the target's stdout) |
| `host.send(...)` | same, but appends to the reply without a trailing newline (partial lines) |
| `host.log(...)` | write to `lua_host_<pid>.log` next to the DLL |
| `host.version()` | host + Lua version string |
| `host.pid()` | current process id |
| `host.os()` / `host.arch()` | `"windows"` / `"x64"` etc. |
| `host.sleep(ms)` | sleep the host thread inside the process |
| `host.peek(addr, n)` | read `n` bytes → string |
| `host.poke(addr, s)` | write string bytes → bool |
| `host.alloc(n)` | allocate RW memory → address (or nil) |
| `host.free(addr, n)` | release memory → bool |
| `host.protect(addr, n, mode)` | set protection; `mode` 0=none, 1=R, 2=RW, 4=RX, 8=RWX → bool |
| `host.module(name)` | base address of a loaded module (case-insensitive, `.dll` optional) |
| `host.modules()` | array of `{name=, base=, size=}` for every loaded module |
| `host.proc(mod, func)` | address of an exported function (`host.proc("kernel32.dll","LoadLibraryA")`) |
| `host.scan(pattern, addr, span)` | AOB scan → first match address or nil |

### `host.scan` patterns

Plain hex bytes with two wildcard syntaxes:

```lua
host.scan("48 8B 05 ?? ?? ?? ??", base, size)   -- byte wildcards
host.scan("4? 8? ??", base, size)               -- nibble wildcards
host.scan("48,8B,05,??", base, size)            -- commas accepted too
```

`span` is optional; without it the scan walks every readable committed
region of the process (via `VirtualQuery`), skipping guard and
non-readable pages.

### Idioms

```lua
-- find the main .exe module
local main
for _, m in ipairs(host.modules()) do
  if m.name:lower():match("%.exe$") then main = m end
end

-- run a callback on every module (the "coroutine" idiom):
-- spawn a fresh coroutine per module, yield to keep the host responsive
```

---

## Example scripts

| file | what it shows |
|---|---|
| `examples/demo.lua` | hello-world tour: modules, exports, peek, alloc/poke/free, sleep |
| `examples/process_info.lua` | dump every module of the target, biggest first |
| `examples/scan_demo.lua` | AOB-scan the main module for its `MZ` DOS header and the DOS-stub string |
| `examples/notepad_demo.bat` | one-command first run: spawn notepad, attach, dump modules |
| `test_game/scripts/game_demo.lua` | find the test game's state struct by marker scan, print it, show an in-game message |
| `test_game/scripts/game_set_score.lua` | poke score + level into the game |
| `test_game/scripts/game_freeze_lives.lua` | pin lives at 9 for ~10 s |
| `test_game/scripts/game_autopilot.lua` | autopilot + god mode (2 pokes/frame) for ~10 s |

---

## Troubleshooting & bug fixes (with YouTube videos)

See **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)** — the full manual:

* how to use the tool step by step (with the first-run checklist),
* how the injection actually works,
* a bug-by-bug fix list — access denied, bitness mismatch, missing host
  DLL, antivirus quarantines, SmartScreen "Windows protected your PC",
  dead pipes, locked files, and the common Lua script errors — and every
  fix is paired with a **YouTube tutorial video** that shows it visually
  (run-as-admin videos, 32/64-bit check videos, Defender exclusion videos,
  SmartScreen fix video).

---

## Limitations

* **Same Windows session / same integrity level.** You must be able to
  `OpenProcess` the target with VM+thread rights — i.e. run as
  Administrator for elevated targets. Protected processes (PPL) and
  anti-cheat-guarded processes (which use exactly this technique to detect
  themselves being tampered with) are out of scope by design.
* **One host per process.** The pipe name embeds the PID; a second
  injection attempt logs an error and idles rather than double-embedding.
* **The host keeps running after you disconnect.** Use `!unload` to remove
  it; the DLL unloads via `FreeLibraryAndExitThread`.
* **No debugger features** (breakpoints, single-step, hooking). This is a
  scripting/automation layer, not a debugger — you can *build* those on
  top of `host.poke`/`host.protect`, but they aren't included.
* The Linux test harness exercises `luacore.c` only (no injection, no
  pipes) — the Windows-only paths (`host.modules`, `host.module`,
  `host.proc`, injection) are compiled for Windows but can't be executed in
  this environment; they follow the documented Win32 semantics.

---

## Ethics & tos

This tool's legitimate uses are: debugging your own software, game
modding/automation on servers that allow it, reverse-engineering
interop/security research, and learning how OS process mechanics work. Its
core API is exactly the read/write/scan primitives every debugger and
profiler ships.

Please respect: (1) the ToS of any online game or service you attach to —
server-side games can and do treat client-side instrumentation as
cheating; (2) other people's processes — only instrument software you own
or have permission to inspect; (3) your local laws. The author of this
tool provides it for educational purposes and accepts no liability for
how it's used.

---

*Lua 5.4.7 is vendored under `third_party/lua/` (MIT, © Lua.org, PUC-Rio).*
