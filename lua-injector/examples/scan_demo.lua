-- scan_demo.lua — find the "MZ" DOS magic of the process's main .exe
-- module by pattern-scanning its whole image.  host.scan works on any
-- committed, readable memory, so it is the general-purpose "AOB scan"
-- tool for finding signatures in a process.
--
--   lua-injector-x64.exe --pid <pid> --script examples\scan_demo.lua --stay

local main = nil
for _, m in ipairs(host.modules()) do
  if m.name:lower():match("%.exe$") then main = m end
end

if not main then
  print("no .exe module found in the process")
  return
end

print("main module:", main.name)
print("scanning", main.size, "bytes for '4D 5A' (MZ) ...")

-- 4D 5A = 'MZ'; the DOS header lives at the very start of the image
local hit = host.scan("4D 5A", main.base, main.size)
if hit then
  print(string.format("first MZ at 0x%llX (matches base? %s)",
                      hit, tostring(hit == main.base)))
else
  print("not found")
end

-- a fancier example: the DOS stub usually contains "This program cannot
-- be run in DOS mode." — find it with a wildcarded signature
local stub = host.scan("54 68 69 73 20 70 72 6F 67 72 61 6D", main.base, main.size)
print("DOS stub text:", stub and string.format("found at 0x%llX", stub) or "not found")
