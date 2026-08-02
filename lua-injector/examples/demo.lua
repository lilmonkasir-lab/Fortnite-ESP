-- demo.lua — the "hello world" tour of the host API.
-- Run from the lua-injector folder:
--   lua-injector-x64.exe --pid <pid> --script examples\demo.lua --stay
--
-- Every print() lands in the reply the injector prints for you.

print("== lua-injector demo ==")
print("version:", host.version())
print("inside pid", host.pid(), "on", host.os(), host.arch())

-- module base + exported function lookup
local kernel = host.module("kernel32.dll")
print("kernel32.dll base:", kernel and string.format("0x%llX", kernel) or "nil")
local lp = host.proc("kernel32.dll", "LoadLibraryA")
print("LoadLibraryA @   :", lp and string.format("0x%llX", lp) or "nil")

-- the first two bytes of any PE module are 'M' 'Z' (0x4D 0x5A)
if kernel then
  print("kernel32 magic   :", host.peek(kernel, 2))
end

-- allocate a scratch buffer inside the process, use it, free it
local buf = host.alloc(64)
host.poke(buf, "lua-injector scratch buffer")
print("scratch buffer   :", host.peek(buf, 24))
host.free(buf, 64)

-- host.sleep really sleeps the host thread (inside the target)
host.sleep(10)
print("demo done")
