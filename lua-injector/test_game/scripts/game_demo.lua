-- game_demo.lua — the "hello world" script for the bundled test game.
--
--   lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_demo.lua --stay
--
-- 1. finds the game's state struct by AOB-scanning for its marker string,
-- 2. prints every documented field,
-- 3. writes a message that the game window displays in cyan.
--
-- The offsets below match test_game/game_logic.h (see test_game/GAME.md).

local MARKER = "LUA-INJECTOR-TEST-GAME-4C2F9A1E"

-- helper: turn "LUA-..." into "4C 55 41 2D ..." scan bytes
local function marker_pattern(s)
  return (string.gsub(s, ".", function(c)
    return string.format("%02X ", string.byte(c))
  end))
end

-- helpers: read/write the two field types in the state struct
local function i32(a)  return select(1, string.unpack("<i4", host.peek(a, 4))) end
local function f32(a)  return select(1, string.unpack("<f",  host.peek(a, 4))) end
local function seti32(a, v) host.poke(a, string.pack("<i4", v)) end
local function setf32(a, v) host.poke(a, string.pack("<f",  v)) end

-- find the struct: scan the whole process for the marker
local base = host.scan(marker_pattern(MARKER), 0, 0)
if not base then
  print("marker not found — is the test game running?")
  return
end
base = base - 4   -- the marker lives 4 bytes into the struct

print("== lua-injector test game ==")
print("state struct @ " .. string.format("0x%llX", base))
print("magic       : " .. string.format("0x%08X", i32(base + 0x00)))
print("score       : " .. i32(base + 0x24))
print("lives       : " .. i32(base + 0x28))
print("level       : " .. i32(base + 0x2C))
print("game_over   : " .. i32(base + 0x30))
print("paused      : " .. i32(base + 0x34))
print(string.format("player      : %.0f, %.0f", f32(base + 0x38), f32(base + 0x3C)))
print(string.format("speed       : %.2f", f32(base + 0x40)))
print("aliens left : " .. i32(base + 0x44))
print("bullets     : " .. i32(base + 0x48))
print(string.format("nearest alien: %.0f, %.0f", f32(base + 0x90), f32(base + 0x94)))

-- write a message the game window will draw in cyan
seti32(base + 0x4C, 1)
host.poke(base + 0x50, "HELLO FROM LUA! (demo)")
print("look at the game window — it now shows a Lua message")
