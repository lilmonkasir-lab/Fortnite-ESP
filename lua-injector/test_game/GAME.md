# The Lua Injector Test Game

A tiny, self-contained Windows game built **specifically as a safe target**
for lua-injector. It's a single-player "Invaders"-style shooter: move with
the **arrow keys**, shoot with **space**, pause with **P**, restart with
**R** after game over.

Its whole point is to be scripted: the game's state lives in one
documented struct in memory, findable by a unique marker string, so the
example Lua scripts can read it, write it, freeze it, and drive it — all
live, while the game runs.

```
┌──────────────────────────────┐        ┌──────────────────────────────┐
│  lua-test-game-x64.exe       │        │  lua-injector-x64.exe        │
│  (windowed, 60 fps)          │◄──────►│  (console client + REPL)     │
│  state struct in .data       │  pipe  │  scripts scan for the marker │
│  marker: LUA-INJECTOR-...    │        │  and peek/poke the struct    │
└──────────────────────────────┘        └──────────────────────────────┘
```

## Files

| file | what it is |
|---|---|
| `game_logic.h` / `game_logic.c` | platform-independent gameplay + the `game_state_t` struct (the scripting surface) |
| `win_game.c` | Win32 window, GDI drawing, input, timer |
| `game_sim.c` | Linux headless simulator (used by `make run-test` to verify the logic) |
| `scripts/game_demo.lua` | find the struct, print every field, show a message in-game |
| `scripts/game_set_score.lua` | poke score + level |
| `scripts/game_freeze_lives.lua` | pin lives at 9 for ~10 s |
| `scripts/game_autopilot.lua` | steer the ship toward aliens + god mode for ~10 s |
| `run_game_demo.bat` | start the game, attach, run the demo script |

## Build

```
make                  # builds the game binaries too (bin/lua-test-game-*.exe)
make windows
make run-test         # also runs the headless logic simulation
```

Or on Windows with MSVC: `build.bat` (and `build.bat 32`) build
`bin\lua-test-game-x64.exe` / `x86` as well.

## Run & attach

```bat
start bin\lua-test-game-x64.exe          :: or just double-click it
bin\lua-injector-x64.exe --list          :: find its PID (window title shows it too)
bin\lua-injector-x64.exe --pid <pid> --script test_game\scripts\game_demo.lua --stay
bin\lua-injector-x64.exe --pid <pid>     :: or open a REPL
```

Or one command: `test_game\run_game_demo.bat`.

The window title is `Lua Injector Test Game [pid N]`, and the HUD prints
the live address of the state struct — handy when you don't want to scan.

## The memory map (what scripts read/write)

The struct is a static global in the game's `.data` section. Layout is
padding-free (all fields 4-byte aligned) and compile-time asserted; the
Linux simulator prints the offsets so you can verify them yourself.

| offset | type | field | meaning |
|---|---|---|---|
| 0x00 | int32 | `magic` | always `0x4C55414C` ("LUAL") |
| 0x04 | char[32] | `marker` | `"LUA-INJECTOR-TEST-GAME-4C2F9A1E"` |
| 0x24 | int32 | `score` | score (10 per alien) |
| 0x28 | int32 | `lives` | lives (0 = game over) |
| 0x2C | int32 | `level` | level (higher = faster aliens) |
| 0x30 | int32 | `game_over` | 1 when the game is over |
| 0x34 | int32 | `paused` | 1 when paused |
| 0x38 | float | `player_x` | player top-left x (pixels) |
| 0x3C | float | `player_y` | player top-left y (pixels) |
| 0x40 | float | `speed` | alien descent speed multiplier |
| 0x44 | int32 | `alien_count` | aliens still alive |
| 0x48 | int32 | `bullet_count` | bullets in flight |
| 0x4C | int32 | `lua_msg_mode` | 1 = draw `lua_msg` in the window |
| 0x50 | char[64] | `lua_msg` | text shown in cyan (NUL-terminated) |
| 0x90 | float | `target_x` | nearest alive alien center x (game-updated) |
| 0x94 | float | `target_y` | nearest alive alien center y (game-updated) |
| 0x98 | int32[2] | `reserved` | spare |

Total size: **0xA0 (160) bytes**. The offsets are identical on x64 and
x86 builds, so the same scripts work either way.

## How scripts find it

The marker is a unique 31-byte ASCII string, so scripts locate the struct
with an AOB scan (this is exactly what `host.scan` is for):

```lua
local pattern = (string.gsub("LUA-INJECTOR-TEST-GAME-4C2F9A1E", ".",
  function(c) return string.format("%02X ", string.byte(c)) end))
local base = host.scan(pattern, 0, 0)   -- whole address space
base = base - 4                          -- marker is 4 bytes into the struct
```

Then read/write with `host.peek`/`host.poke` and `string.unpack`/`string.pack`:

```lua
local function i32(a) return select(1, string.unpack("<i4", host.peek(a, 4))) end
local function f32(a) return select(1, string.unpack("<f",  host.peek(a, 4))) end
local function seti32(a, v) host.poke(a, string.pack("<i4", v)) end
local function setf32(a, v) host.poke(a, string.pack("<f",  v)) end

seti32(base + 0x28, 9)      -- infinite lives
setf32(base + 0x38, 300.0)  -- teleport the ship
seti32(base + 0x4C, 1)      -- turn on the in-game Lua message
host.poke(base + 0x50, "hi from Lua")  -- ...with this text
```

## Notes & caveats

* **Single-player test target only.** The game is deliberately simple and
  race-tolerant (the Lua host thread may write state mid-frame; at worst a
  frame renders mid-update, which is harmless here). Don't expect it to be
  production-grade — that's not its job.
* **Keep loops finite.** `--script` blocks until the script returns, so an
  `while true do` loop in a script means the client waits forever. The
  bundled scripts all use bounded loops.
* **God mode is just two pokes a frame** (`lives` and `game_over`) — see
  `game_autopilot.lua`; it's the whole "cheat engine" pattern in 6 lines.
* To verify the logic and offsets without Windows: `make run-test` runs
  `build/game-sim-test`, which simulates 2000 frames and prints the struct
  layout.
