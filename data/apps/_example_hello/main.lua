-- Watch OS Hello — live session demo (Lua 5.4)
-- Needs firmware with luaHostOpen / watch.button / watch.on_tick.

local counter = 0
local last_sec = -1

watch.set_title("Hello Lua")
watch.set_text("Live session demo\nTap +1, watch the clock tick.")

-- Small accent mark (absolute screen coords)
watch.circle(360, 48, 10, 62, 224, 240)

local function refresh()
  local t = watch.time()
  local body = string.format(
    "count=%d\n%02d:%02d:%02d\nwday=%d  %04d-%02d-%02d\n%dx%d  millis=%d",
    counter,
    t.hour, t.min, t.sec,
    t.wday, t.year, t.month, t.day,
    watch.width(), watch.height(), watch.millis()
  )
  watch.set_text(body)
end

watch.button("+1", function()
  counter = counter + 1
  refresh()
end)

watch.on_tick(function()
  local t = watch.time()
  if t.sec ~= last_sec then
    last_sec = t.sec
    refresh()
  end
end)

refresh()
print("Hello live session ready")
