-- game_freeze_lives.lua — keep lives maxed out for ~10 seconds.
--
--   lua-injector-x64.exe --pid <game-pid> --script test_game\scripts\game_freeze_lives.lua --stay
--
-- Shows the classic "write a value every frame so the game can't lower it"
-- pattern.  The loop is finite (200 * 50 ms) so the session ends cleanly.
-- Note: EVAL blocks until the script finishes, so keep loops bounded.

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

seti32(base + 0x4C, 1)
host.poke(base + 0x50, "LIVES FROZEN BY LUA")
for i = 1, 200 do
  seti32(base + 0x28, 9)            -- lives = 9
  seti32(base + 0x30, 0)            -- also clear game-over, just in case
  if i % 20 == 0 then print("tick", i, "lives =", i32(base + 0x28)) end
  host.sleep(50)
end
seti32(base + 0x4C, 0)
print("freeze released")
