-- Watch OS Counter — second demo using battery / back / vibrate APIs

local count = 0
local last_sec = -1

watch.set_title("Counter")
watch.fill(5, 5, 8)

local function battery_line()
  local b = watch.battery()
  if b == nil then
    return "batt=—"
  end
  return string.format("batt=%d%%", b)
end

local function refresh()
  local t = watch.time()
  local body = string.format(
    "count = %d\n\n%02d:%02d:%02d\n%s\n\n+1 / −1 / Reset\nBack closes app",
    count,
    t.hour, t.min, t.sec,
    battery_line()
  )
  watch.set_text(body)
end

watch.button("+1", function()
  count = count + 1
  watch.vibrate()  -- GPIO18 motor
  refresh()
end)

watch.button("−1", function()
  count = count - 1
  refresh()
end)

watch.button("Reset", function()
  count = 0
  refresh()
end)

watch.button("Exit", function()
  -- Deferred: firmware returns to launcher after the Lua call finishes.
  watch.back()
end)

watch.on_tick(function()
  local t = watch.time()
  if t.sec ~= last_sec then
    last_sec = t.sec
    refresh()
  end
end)

refresh()
print("Counter ready — battery/back/vibrate demo")
