-- Run with:
--   lua-injection host
--   lua-injection attach --script examples/hello.lua --show-state

print('connected to ' .. host.name())
local before = host.get('counter') or 0
host.set('counter', before + 1)
host.emit('counter.changed', { value = before + 1 })
print('counter is now ' .. tostring(host.get('counter')))
