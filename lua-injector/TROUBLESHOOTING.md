# lua-injector — Complete User Guide & Bug-Fix Manual

Everything you need: **how to use it**, **how it works**, and a
**bug-by-bug fix list**, each common problem paired with a **YouTube video**
that walks you through the fix visually.

---

## Table of contents

- [Part 1 — How to use it](#part-1--how-to-use-it)
- [Part 2 — How it works](#part-2--how-it-works)
- [Part 3 — First-run checklist](#part-3--first-run-checklist)
- [Part 4 — Bug index (fix with videos)](#part-4--bug-index-fix-with-videos)
- [Part 5 — Script bugs you will hit](#part-5--script-bugs-you-will-hit)
- [Part 6 — Things that are NOT bugs](#part-6--things-that-are-not-bugs)
- [Part 7 — The video playlist at a glance](#part-7--the-video-playlist-at-a-glance)

---

## Part 1 — How to use it

### 1. Put the binaries somewhere safe

Keep the four files together in one folder (the injector looks for the host
DLL *next to itself*):

```
C:\lua-injector\
├── lua-injector-x64.exe
├── lua-injector-x86.exe
├── lua-host-x64.dll
└── lua-host-x86.dll
```

> **Bug #0 prevention:** if the `.exe` and `.dll` are in different folders,
> you get `error: host DLL not found: ...`. Keep them together, or pass the
> path explicitly: `--dll C:\full\path\lua-host-x64.dll`.

### 2. Open an Administrator console

Right-click **Command Prompt** (or PowerShell/Terminal) → **Run as
administrator** → **Yes** on the UAC prompt.

> **Important:** run the injector from that already-open window —
> **don't double-click the .exe**. If the exe is double-clicked and
> something goes wrong, Windows destroys the console window the instant the
> program exits, so the error message vanishes with it (see **Bug #0**).
> With an open Command Prompt the window stays and you can read every
> message.

Why admin: DLL injection needs `PROCESS_VM_*` + `PROCESS_CREATE_THREAD`
rights on the target. If the target runs elevated (or as a service/another
user) and your console isn't, Windows refuses with error 5 (access denied).

**Watch it:**
- [How To Run Program As Administrator On Windows 11 — MDTechVideos](https://www.youtube.com/watch?v=tHimmOzrsfo)
- [How to Run Any Program as Administrator on Windows 11 (Easy Guide)](https://www.youtube.com/watch?v=lIoGXTe_PRc)
- [How to Always Run a Program as Administrator in Windows 11/10](https://www.youtube.com/watch?v=aVYfMlQFJYM)

### 2b. Or just use the GUI

```
lua-injector-gui-x64.exe
```

1. Wait for the process list to fill (or click **Refresh**).
2. **Click the target's row** — its PID goes into the PID box.
3. Pick a **method**: `auto` (default) is the safe choice; `manual` maps
   the DLL without `LoadLibrary` if you want that.
4. Click **Attach** — the status bar, a **tray notification**, and the
   output log tell you what happened. The GUI **PINGs the host** after
   injecting, so "attached" really means the host is alive.
5. Paste Lua straight into the big **Lua code** box (it's multiline —
   whole scripts paste fine) and press **Ctrl+Enter** to run, or
   **Browse...** for a `.lua` script and **Run script**.
6. **Open console REPL...** spawns the full console client in a new window
   for anything the GUI can't do.

> **The error log:** every attach, script run, eval, and error is written
> with a timestamp to **`lua-injector-gui.log`** next to the GUI's exe.
> Click **Open error log...** to view it, or open it in Notepad. If the
> GUI crashes, the exception code + address are logged there too, and a
> dialog shows instead of a silent exit.

> The GUI needs the same privileges as the console tool: if the target is
> elevated, run the GUI **as administrator** too, or you'll get access
> denied on attach.

### 3. Find your target's PID

```
cd C:\lua-injector
lua-injector-x64.exe --list
```

You get a table: `PID  BITS  IMAGE NAME`. Note the `BITS` column — you must
match the architecture (x64 process → x64 injector, x86 process → x86
injector). **The tool refuses the wrong pairing**, so this column tells you
which injector to use.

**Watch it (reading the BITS/Platform column):**
- [How to Check If a File or Program Is 32-Bit or 64-Bit on Windows](https://www.youtube.com/watch?v=BjOqUJjZoIg)
- [How to Check Whether an Installed Program is 32-bit or 64-bit in Windows 10](https://www.youtube.com/watch?v=-76TouMqwW8)

### 4. Attach and script

```bat
:: one-liner
lua-injector-x64.exe --pid 4820 --eval "print('hi from', host.os())"

:: script file, keep the host attached afterwards
lua-injector-x64.exe --pid 4820 --script examples\demo.lua --stay

:: interactive REPL
lua-injector-x64.exe --pid 4820
```

In the REPL:

```
lua> print(host.module("kernel32.dll"))     any Lua code
lua> !help                                  list REPL commands
lua> !file mymod.lua                        run a script file
lua> !unload                                remove the host from the process
lua> !quit                                  disconnect (host stays inside)
```

The host **stays inside the process** after you disconnect. Reconnect any
time with `lua-injector-x64.exe --pid <same-pid>` — the injector detects the
existing host pipe and skips re-injection.

### 5. The script API (one-line cheat sheet)

```lua
print(...)                 -- output comes back to your console
host.peek(addr, n)         -- read n bytes -> string
host.poke(addr, str)       -- write bytes
host.alloc(n) / host.free(a, n) / host.protect(a, n, 0|1|2|4|8)
host.module("kernel32")    -- module base address
host.modules()             -- {name=, base=, size=} list
host.proc("kernel32.dll", "LoadLibraryA")   -- export address
host.scan("48 8B ?? ?? ?? ??", addr, size)  -- AOB scan
host.sleep(ms) / host.pid() / host.version()
```

---

## Part 2 — How it works

Three layers, bottom to top:

```
┌────────────────────────────────────────────────────────┐
│  YOU                                                   │
│  lua-injector-x64.exe  (console client + REPL)         │
│  · lists processes, takes a PID                        │
│  · injects the host DLL                                │
│  · talks to it over a named pipe                       │
└──────────────────────────┬─────────────────────────────┘
                           │ CreateRemoteThread(LoadLibraryA)
                           ▼
┌────────────────────────────────────────────────────────┐
│  TARGET PROCESS                                       │
│  ┌──────────────────────────────────────────────────┐  │
│  │ lua-host-x64.dll   (the "host")                  │  │
│  │ · spawns 1 worker thread on load                 │  │
│  │ · creates a private Lua 5.4 VM                  │  │
│  │ · installs the host.* API table                 │  │
│  │ · opens pipe \\.\pipe\lua_host_<pid>            │  │
│  │ · runs EVAL/FILE/PING/EXIT commands              │  │
│  └──────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────┘
```

**The injection step** (the part most likely to hit bugs):

1. `OpenProcess(...)` with `VM_READ|VM_WRITE|VM_OPERATION|CREATE_THREAD`
   rights — **this is where "error 5 / access denied" happens** when you
   aren't admin or the target is protected.
2. Resolve `LoadLibraryA` inside the target:
   - same bitness → reuse our own kernel32's `LoadLibraryA` address
     (kernel32 loads at the same base in every process of a bitness);
   - 64-bit injector → 32-bit target → walk the WOW64 PEB module list, then
     the 32-bit kernel32's export table.
3. `VirtualAllocEx` room in the target, `WriteProcessMemory` the DLL path,
   `CreateRemoteThread(LoadLibraryA, path)` → the DLL loads → DllMain fires
   → worker thread starts.
4. The worker thread creates the named pipe and waits. The injector
   connects and you're scripting.

**The protocol** is simple newline-framed text:

```
you → host:   EVAL\n<len>\n<lua source>\n    (or FILE\n<path>\n)
host → you:   OK\n<len>\n<print output>\n    (or ERR\n<len>\n<message>\n)
```

`print()` output is captured by the host (never printed into the target's
stdout) and returned as the reply payload — so scripts behave like a normal
Lua program run from a shell.

---

## Part 3 — First-run checklist

Run through these in order when something doesn't work:

1. [ ] Am I in an **Administrator** console? (see Part 1.2)
2. [ ] Did I use the right **bitness** injector for the target? (`--list`
       shows the BITS column)
3. [ ] Are the **.exe and .dll in the same folder**?
4. [ ] Is the target actually **still running**? (notepad closed? then its
       PID is dead)
5. [ ] Did an **antivirus quarantine** the DLL? Check Protection history.
6. [ ] Is there a **`lua_host_<pid>.log`** next to the DLL? It contains
       everything the host logged — read it for clues.
7. [ ] For online games: this is **detectable** — anti-cheat treats exactly
       this technique as tampering. Don't use it there; use a spare VM or
       your own single-player apps to learn.

---

## Part 4 — Bug index (fix with videos)

> **New in the latest build:** the injector now pauses on errors
> ("Press Enter to close this window...") and prints a crash screen
> ("*** lua-injector crashed ***") instead of silently vanishing — so the
> fixes below are actually readable now.

### Bug #0 — "The window closes instantly after I enter the PID"

**Symptom:** you double-click the exe, the process list appears, you type a
PID, press Enter — and the window is gone before you can read anything.

**Why it happens:** a console program exits (success *or* error), and when
you launched it by double-clicking, Windows destroys the console window at
the same moment. The tool was almost certainly printing an error — you just
never got to see it. The most common real errors underneath are #1 (access
denied), a stale PID, or an AV block.

**Fixes, in order:**

1. **Run it from an already-open Command Prompt instead of double-clicking.**
   Start → type `cmd` → open it (as administrator) → then run
   `lua-injector-x64.exe` inside it. The window stays, and you'll see the
   exact error message. This alone fixes the confusion.
   **Watch it:** [How To Run Program As Administrator On Windows 11 — MDTechVideos](https://www.youtube.com/watch?v=tHimmOzrsfo)
   · [Easy Guide](https://www.youtube.com/watch?v=lIoGXTe_PRc)
2. **The newest build pauses on errors** — if you're running it and it
   stops at "Press Enter to close this window...", the text above that line
   is your real error. Copy it down.
3. **Check the PID is still alive.** Processes end (notepad closed, game
   closed). Re-run `--list` and pick a PID from the fresh list.
4. **Are you an administrator?** Elevated targets refuse non-elevated
   injectors with error 5 (see Bug #1).
5. **Antivirus may have quarantined the host DLL** mid-run (see Bug #6) —
   the injection then fails silently and the client exits.

If you ever see **`*** lua-injector crashed ***`**, that's a genuine bug:
report the `exception code` + `address` line, plus the contents of
`lua_host_<pid>.log` next to the DLL.

### Bug #1 — `error: cannot open process (5)` / Access is denied

**Symptom:**

```
error: cannot open process 4820 (5).
       the process may not exist, or it runs with higher
       privileges than this console — run as administrator.
```

**Cause:** The target is elevated, a service, or owned by another user, and
your console doesn't have the rights to open it with VM+thread access.
(Error 5 = `ERROR_ACCESS_DENIED`.)

**Fix, in order of likelihood:**

1. Right-click your terminal → **Run as administrator** → Yes.
2. If the target was started elevated (e.g. an admin-launched game), close
   it and launch it from a *normal* (non-elevated) session — then your
   admin console can open it.
3. If the target is a service, it runs as a different account — you'd need
   the service to run as your user, or run your console as SYSTEM
   (`psexec -s`), which is beyond normal use.
4. If it's a **protected process** (PPL — some AV, DRM, anti-cheat), no
   fix; that's the OS deliberately refusing this technique.

**Watch it:**
- [How To Run Program As Administrator On Windows 11 — MDTechVideos](https://www.youtube.com/watch?v=tHimmOzrsfo)
- [How to Run Any Program as Administrator on Windows 11 (Easy Guide)](https://www.youtube.com/watch?v=lIoGXTe_PRc)

---

### Bug #2 — `error: host DLL not found: ...`

**Symptom:** the injector can't find `lua-host-x64.dll`.

**Cause:** You ran the exe from a different working directory, or moved the
DLL away.

**Fix:**

```
:: run from inside the folder that has both files
cd C:\lua-injector
lua-injector-x64.exe --pid 4820

:: or point at the DLL explicitly
lua-injector-x64.exe --pid 4820 --dll C:\lua-injector\lua-host-x64.dll
```

---

### Bug #3 — `error: LoadLibraryA inside the target returned NULL`

**Symptom:** injection "ran" but the host never loaded.

**Causes & fixes:**

| cause | fix |
|---|---|
| **Wrong bitness DLL** — you injected x86 into an x64 process | Use the matching pair: x64 process → `lua-host-x64.dll`; x86 → `lua-host-x86.dll`. Check with `--list` (BITS column). |
| **Old injector build** — same-bitness attach fails with `could not resolve LoadLibraryA in the target process` | This was a bug in earlier builds (a boolean-vs-bitness comparison made every same-arch attach take the 32-bit path). Grab the rebuilt `lua-injector-x64.exe`/`x86.exe` from `bin/`. |
| **DLL deleted/quarantined between check and load** | Rebuild or restore the DLL; add an AV exclusion (Bug #6). |
| **DLL depends on something missing** (e.g. a missing MSVC runtime) | These binaries statically link the CRT, so this shouldn't happen — if it does, tell us what's in the log file. |
| **Path problem inside the target** (relative path that doesn't exist in the target's working dir) | Use an absolute path: `--dll C:\lua-injector\lua-host-x64.dll`. |

**Watch it (checking bitness):**
- [How to Check If a File or Program Is 32-Bit or 64-Bit on Windows](https://www.youtube.com/watch?v=BjOqUJjZoIg)
- [How to Check Whether an Installed Program is 32-bit or 64-bit in Windows 10](https://www.youtube.com/watch?v=-76TouMqwW8)

---

### Bug #4 — `error: 32-bit injector cannot inject into a 64-bit process`

**Symptom:** the x86 injector refuses the x64 target.

**Cause:** A 32-bit process cannot call into a 64-bit address space; the
reverse direction (64-bit injector → 32-bit target) IS supported via the
WOW64 PEB walk, but 32→64 is physically impossible.

**Fix:** Use `lua-injector-x64.exe` for anything marked `x64` in `--list`.

---

### Bug #5 — `error: injected, but the host pipe never became available`

**Symptom:** LoadLibrary succeeded, but the client can't connect to
`\\.\pipe\lua_host_<pid>`.

**Causes & fixes:**

1. **The host thread crashed during startup** → check `lua_host_<pid>.log`
   next to the DLL. Common cause: the DLL was quarantined *while* loading.
2. **A second host was already running** → only one pipe per PID is
   allowed; reconnect instead (`--pid <pid>` reuses the existing host).
3. **The DLL was loaded but its DllMain thread was killed** (rare; happens
   when the target itself is hostile) → nothing to fix; restart target.

---

### Bug #6 — Antivirus / Windows Defender quarantines the DLL (very common)

**Symptom:** the DLL vanishes, or Windows shows "Threat detected", or the
injector prints a NULL-load error. Injection tools look like malware to
heuristic AV — this is a *false positive* on code that does exactly what a
debugger does.

**Fix:**

1. **Restore the file**: Windows Security → *Virus & threat protection* →
   *Protection history* → select the item → *Actions* → *Restore*.
2. **Add an exclusion** so it stops re-quarantining:
   Windows Security → *Virus & threat protection* → *Manage settings* →
   *Exclusions* → *Add or remove exclusions* → *Add an exclusion* →
   **Folder** → pick your `C:\lua-injector` folder.

**Watch it:**
- [How to Add or Remove Exclusions in Microsoft Defender — 2025 Guide](https://www.youtube.com/watch?v=mUQG9ybM93Q)
- [How To Set Up Exclusions on Windows Defender in Windows 10 — MDTechVideos](https://www.youtube.com/watch?v=nRaGvYL2lwk)
- [How to Exclude a File or Folder from Windows 11 Security](https://www.youtube.com/watch?v=_Eyc2lzvnHM)

> Do this only for files you built/trust. Adding exclusions for random
> downloaded exes is how machines get owned.

---

### Bug #7 — SmartScreen: "Windows protected your PC"

**Symptom:** the first time you run the injector, Windows shows
*"Windows protected your PC — Microsoft Defender SmartScreen prevented an
unrecognized app from starting."*

**Cause:** The binaries aren't code-signed (no publisher reputation yet).
Normal for self-built tools.

**Fix:** Click **More info** → **Run anyway**. (Optionally: right-click the
file → Properties → General → **Unblock**.)

**Watch it:**
- [How to Fix "Windows protected your PC" on Windows 10/11](https://www.youtube.com/watch?v=viZt4jVx8Eo)

---

### Bug #8 — `connection lost` right after connecting / REPL dies

**New in the latest build:** the GUI now tells you *why* the connection
died and where to look:

* if the **target process exited**, it says so explicitly — restart the
  app and re-attach;
* if the **host crashed** (a CPU exception in the host thread), the GUI
  says the host stopped responding and points at the host log; the host
  writes `[crash] exception 0x... at 0x... while serving 'EVAL'` to
  `lua_host_<pid>.log` (next to the DLL, or in `%TEMP%` for manual maps)
  before it exits, and only the host thread dies — the target keeps
  running.
* the console client prints the same log hint on connection loss.

**Causes & fixes:**

| cause | fix |
|---|---|
| The **target process exited** (notepad closed, game closed) | Restart it and re-attach. |
| The host was **unloaded** by a previous `!unload` | Re-inject: run the injector again (it will inject a fresh host). |
| A script **crashed the host thread** | Read `lua_host_<pid>.log` — it now logs the exception code + address + which command crashed. Fix the script (e.g. wrap risky calls in `pcall`), then re-attach. An infinite loop won't crash — it just blocks that command; keep loops bounded. |

> If you ever see a `[crash]` line in the host log, paste it here — that
> exception code + address is exactly what tells us the root cause.

### Bug #8c — Manual-map attach succeeds but no REPL/scripts work

**Symptom:** `--method manual` (or auto-fallback) reports success, but the
pipe never connects or the host logs nothing.

**Causes & fixes:**

| cause | fix |
|---|---|
| The DLL imports something not loaded in the target | The manual mapper resolves imports against the target's loaded modules. `lua-host-*.dll` only imports kernel32, so this should not happen — if it does, check the error message; it names the module. |
| Log location | A manually mapped DLL has no real module handle, so its log goes to `%TEMP%\lua_host_<pid>.log` instead of next to the DLL. Check there. |
| Host "unload" leaves the manual image mapped | Manual images can't be `FreeLibrary`'d (they were never loaded); the host thread exits and the image stays until the target exits. Re-attach simply re-injects. |

---

### Bug #8b — GUI: nothing happens when I click Attach

**Symptom:** clicking Attach in the GUI does nothing; the status bar keeps
saying "attaching...".

**Causes & fixes:**

| cause | fix |
|---|---|
| The PID box is empty | Click a row in the list (it fills the PID box) or type a PID manually. |
| The target is elevated and the GUI isn't | Close the GUI, right-click it → **Run as administrator**. |
| A host is already attached | The GUI reconnects automatically — check the output log. |
| The process exited | Refresh the list and pick a live process. |

Every attach result is written to the **Output** box (and the status bar),
so a failed attach always shows *why*.

### Bug #9 — The DLL file is "in use" / can't be overwritten

**Symptom:** rebuilding fails, or Windows says the DLL is locked.

**Cause:** The host is still loaded inside a running process (or an AV
scanner is holding it).

**Fix:**

1. Disconnect clients and run `!unload` inside the target, or close the
   target process.
2. Wait a second, then rebuild/copy again.
3. If an AV still holds it, add the exclusion (Bug #6) and retry.

---

## Part 5 — Script bugs you will hit

These are Lua-side mistakes, not tool bugs. The host returns them as
`Lua error: ...`.

| error message | meaning & fix |
|---|---|
| `attempt to index a nil value` | A module wasn't found. `host.module("foo")` returned nil — check the exact name, or loop `host.modules()` and print names. |
| `bad argument #1 to 'peek' (number expected, got nil)` | You passed nil as an address — usually a failed `host.module`/`host.scan` result. Guard it: `local m = host.module("x.dll"); if not m then return end`. |
| `bad argument #1 to 'poke' (string expected, got number)` | `poke` takes a *string* of bytes, not a number. Convert: `host.poke(a, string.pack("<I4", 1234))` or `string.char(...)`. |
| `host.scan: bad pattern` | Pattern has a non-hex character. Use `0-9 A-F ?` and separators only. |
| `attempt to call a nil value` | You used a `host.*` function name that doesn't exist. Check the spelling against the API list. |
| `stack overflow` / `too many C levels` | Infinite recursion or a runaway loop. Add a counter / use `host.sleep` in loops. |
| Everything runs but no output | Your script only *defined* functions — nothing called them. Or you used `return` at top level (not allowed in a chunk run through EVAL; use `print` instead). |

**Debugging recipe for any script:**

```lua
-- wrap risky lookups so one failure doesn't kill the whole script
local ok, res = pcall(host.peek, addr, 4)
print("peek ok?", ok, "result:", res)
```

---

## Part 6 — Things that are NOT bugs

| behavior | why it's normal |
|---|---|
| The host stays after you disconnect | By design — reconnect with `--pid` any time; `!unload` removes it. |
| `host.module("kernel32.dll")` returns nil in the Linux test harness | The Windows-only API returns nil off-Windows. On Windows it works. |
| One host per process | The pipe name embeds the PID; a second load just logs and idles. |
| A message box never appears | This host is headless — no UI. Proof of life is the REPL reply and the `lua_host_<pid>.log`. |
| The target is an online game and the game kicks/bans you | Anti-cheat detects this exact technique. Not a bug in the tool — a policy you accepted in the game's ToS. Use single-player or VM targets. |

---

## Part 7 — The video playlist at a glance

| problem | video |
|---|---|
| Run as administrator / access denied | [How To Run Program As Administrator On Windows 11](https://www.youtube.com/watch?v=tHimmOzrsfo) · [Easy Guide](https://www.youtube.com/watch?v=lIoGXTe_PRc) · [Always run as admin](https://www.youtube.com/watch?v=aVYfMlQFJYM) |
| Check 32-bit vs 64-bit (bitness mismatch fixes) | [Check if a program is 32/64-bit](https://www.youtube.com/watch?v=BjOqUJjZoIg) · [Another walkthrough](https://www.youtube.com/watch?v=-76TouMqwW8) |
| Defender quarantine / exclusions | [Add or Remove Exclusions in Microsoft Defender](https://www.youtube.com/watch?v=mUQG9ybM93Q) · [MDTechVideos Win10 version](https://www.youtube.com/watch?v=nRaGvYL2lwk) · [Exclude a folder on Win11](https://www.youtube.com/watch?v=_Eyc2lzvnHM) |
| "Windows protected your PC" SmartScreen | [How to Fix Windows protected your PC](https://www.youtube.com/watch?v=viZt4jVx8Eo) |

---

*Video links are to publicly indexed YouTube tutorials as of 2026-08; if a
link 404s, search the exact video title on YouTube — these are popular
how-to videos that are re-uploaded under the same titles.*
