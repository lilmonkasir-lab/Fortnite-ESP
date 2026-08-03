-- A slightly more advanced, still safe host smoke test.
local function sum(values)
  local total = 0
  for _, value in ipairs(values) do
    total = total + value
  end
  return total
end

local values = { 2, 4, 6, 8 }
local total = sum(values)
host.patch({ last_total = total, status = 'tested' })
host.emit('batch.completed', { count = #values, total = total })
print(json.encode(host.snapshot()))
return total
