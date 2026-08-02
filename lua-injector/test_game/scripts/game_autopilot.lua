-- game_autopilot.lua — a Lua autopilot + god mode for the test game.
--
--   lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_autopilot.lua --stay
--
-- Every frame it:
--   * reads target_x (the nearest alive alien, maintained by the game),
--   * moves the player toward it by writing player_x,
--   * holds lives at 9 and clears game_over ("god mode"),
--   * and lets the game itself fire continuously (the script holds space
--     via... no — it just steers; hold space yourself or enjoy the show).
--
-- The loop is 600 frames * 16 ms ≈ 10 seconds, then it stops and reports.

local MARKER = "LUA-INJECTOR-TEST-GAME-4C2F9A1E"
local function marker_pattern(s)
  return (string.gsub(s, ".", function(c)
    return string.format("%02X ", string.byte(c))
  end))
end
local function i32(a)     return select(1, string.unpack("<i4", host.peek(a, 4))) end
local function f32(a)     return select(1, string.unpack("<f",  host.peek(a, 4))) end
local function seti32(a, v) host.poke(a, string.pack("<i4", v)) end
local function setf32(a, v) host.poke(a, string.pack("<f",  v)) end

local base = host.scan(marker_pattern(MARKER), 0, 0)
if not base then
  print("marker not found — is the test game running?")
  return
end
base = base - 4

seti32(base + 0x4C, 1)
host.poke(base + 0x50, "AUTOPILOT ON")
print("autopilot engaging (10 seconds)...")

-- aim so the bullet (spawns at player_x + 18) hits the alien center
local start_score = i32(base + 0x24)
for i = 1, 600 do
  local tx = f32(base + 0x90)                 -- where the nearest alien is
  local px = f32(base + 0x38)                 -- where the player is
  local aim = tx - 18.0                       -- bullet spawns at player_x+18
  local step = 8
  if math.abs(aim - px) > step then
    if aim > px then setf32(base + 0x38, px + step)
    else             setf32(base + 0x38, px - step) end
  end
  seti32(base + 0x28, 9)                      -- god mode: lives pinned
  seti32(base + 0x30, 0)                      -- god mode: clear game over
  host.sleep(16)                              -- one frame
end

seti32(base + 0x4C, 0)
print("autopilot done. score went",
      start_score, "->", i32(base + 0x24),
      "(hold space yourself to shoot)")
