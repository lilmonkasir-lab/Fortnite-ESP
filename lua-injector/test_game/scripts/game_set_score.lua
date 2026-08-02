-- game_set_score.lua — poke a new score and level into the game.
--
--   lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_set_score.lua --stay
--
-- Uses the same marker-scan as game_demo.lua (see test_game/GAME.md).

local MARKER = "LUA-INJECTOR-TEST-GAME-4C2F9A1E"
local function marker_pattern(s)
  return (string.gsub(s, ".", function(c)
    return string.format("%02X ", string.byte(c))
  end))
end
local function i32(a)     return select(1, string.unpack("<i4", host.peek(a, 4))) end
local function seti32(a, v) host.poke(a, string.pack("<i4", v)) end

local base = host.scan(marker_pattern(MARKER), 0, 0)
if not base then
  print("marker not found — is the test game running?")
  return
end
base = base - 4

print("score before:", i32(base + 0x24))
seti32(base + 0x24, 9999)   -- score
seti32(base + 0x2C, 9)      -- level (also speeds up the aliens!)
seti32(base + 0x4C, 1)      -- lua message mode on
host.poke(base + 0x50, "SCORE HACKED TO 9999")
print("score after :", i32(base + 0x24))
print("(the HUD in the game window updates on its next frame)")
