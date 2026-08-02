-- test_scripts.lua — fed to the Linux test harness line-by-line.
-- IMPORTANT: every line is evaluated as its own Lua chunk, so each test
-- must be self-contained (no locals carried across lines).
print("hello from host:", host.version())
print("pid/os/arch:", host.pid(), host.os(), host.arch())

-- peek / poke roundtrip on the TESTBUF global
print("peek:", host.peek(TESTBUF, 6))
host.poke(TESTBUF + 6, "WORLD")
print("after poke:", host.peek(TESTBUF, 11))

-- aob scan: full bytes, byte-wildcards, nibble-wildcards
print("scan 'HELLO':", host.scan("48 45 4C 4C 4F", TESTBUF, 64) == TESTBUF)
print("scan 'HE??O':", host.scan("48 45 ?? ?? 4F", TESTBUF, 64) == TESTBUF)
print("scan '4? ?C':", host.scan("48 45 4? ?C 4F", TESTBUF, 64) == TESTBUF)
print("scan no-match:", host.scan("DE AD BE EF", TESTBUF, 64) == nil)
print("scan bad pat:", (function() local ok, err = pcall(host.scan, "zz", TESTBUF, 64) return ok and "no-error" or "raised" end)())

-- alloc / poke / peek / free (single line: each line is one chunk)
local a = host.alloc(4096); print("alloc:", a ~= nil); if a then host.poke(a, "XXXX"); print("alloc roundtrip:", host.peek(a, 4) == "XXXX"); host.free(a, 4096) end

-- protect: use a fresh page-aligned allocation (mprotect needs alignment;
-- VirtualProtect on Windows works on any address)
local p = host.alloc(4096); print("protect RO:", host.protect(p, 4096, 1) == true); print("protect RW:", host.protect(p, 4096, 2) == true); host.free(p, 4096)

-- modules/proc are Windows-only; must not crash on Linux
print("modules:", #host.modules())
print("module kernel32:", host.module("kernel32.dll") == nil)

-- sleep
host.sleep(5)
print("slept 5ms")

-- print capture: multiple args, nil, table
print("args:", 1, nil, "three", { x = 42 })

-- host.send appends to the same reply buffer
host.send("send works too")

-- host.log goes to stderr via the log callback
host.log("this is a log line")

-- string.pack/unpack helpers used by the test-game scripts
print("f32 roundtrip:", string.unpack("<f", string.pack("<f", 123.5)) == 123.5)
print("i32 roundtrip:", string.unpack("<i4", string.pack("<i4", -42)) == -42)
print("pack->hex:", (string.gsub(string.pack("<i4", 305419896), ".", function(c) return string.format("%02X ", string.byte(c)) end)))

-- end-to-end: the exact recipe the test-game scripts use to find the game
-- struct (marker scan + offset math + field access), against a simulated
-- game_state_t in a scratch buffer. (One line: each line is one chunk.)
local buf = host.alloc(160); host.poke(buf, string.pack("<i4", 0x4C55414C)); host.poke(buf + 4, "LUA-INJECTOR-TEST-GAME-4C2F9A1E"); host.poke(buf + 0x24, string.pack("<i4", 1337)); local pat = (string.gsub("LUA-INJECTOR-TEST-GAME-4C2F9A1E", ".", function(c) return string.format("%02X ", string.byte(c)) end)); local found = host.scan(pat, buf, 160); print("game marker scan:", found == buf + 4); local base = found - 4; print("game magic check:", string.format("0x%08X", select(1, string.unpack("<i4", host.peek(base, 4)))) == "0x4C55414C"); print("game score read:", select(1, string.unpack("<i4", host.peek(base + 0x24, 4))) == 1337); host.poke(base + 0x24, string.pack("<i4", 9999)); print("game score write:", select(1, string.unpack("<i4", host.peek(base + 0x24, 4))) == 9999); host.poke(base + 0x50, "HELLO FROM LUA"); print("game lua_msg:", host.peek(base + 0x50, 14) == "HELLO FROM LUA"); host.free(buf, 160)

-- error path: pcall inside Lua, then a real error chunk
print("pcall boom:", pcall(error, "inner boom"))
error("intentional boom")
