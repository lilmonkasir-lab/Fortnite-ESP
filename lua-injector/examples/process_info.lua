-- process_info.lua — dump every module loaded in the target process,
-- biggest first.  Great first script: it proves the host is really
-- running inside the process.
--
--   lua-injector-x64.exe --pid <pid> --script examples\process_info.lua --stay

local mods = host.modules()
table.sort(mods, function(a, b) return a.size > b.size end)

print(string.format("%-36s %-11s %s", "MODULE", "SIZE", "BASE"))
print(string.rep("-", 70))
for _, m in ipairs(mods) do
  print(string.format("%-36s %-11d 0x%llX", m.name, m.size, m.base))
end
print(#mods, "modules loaded")
