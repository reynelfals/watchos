-- Settings: PIN, soft-lock, brightness (NVS). ASCII only.

local function show()
  local pin = watch.get_setting("pin") or "?"
  local soft = watch.get_setting("soft_lock") or 0
  local br = watch.get_setting("brightness") or 0
  local soft_s = (soft == 0) and "off" or (tostring(soft) .. "s")
  watch.set_title("Settings")
  watch.set_text(
    "PIN: " .. pin .. "\n" ..
    "Soft-lock: " .. soft_s .. "\n" ..
    "Brightness: " .. tostring(br) .. "%\n\n" ..
    "Buzz: GPIO18 parked\n" ..
    "(motor likely missing)."
  )
end

watch.clear_ui()
watch.button_layout("grid2")
show()

watch.button("Bright -", function()
  local br = watch.get_setting("brightness") or 80
  br = br - 10
  if br < 5 then br = 5 end
  watch.set_setting("brightness", br)
  show()
end)

watch.button("Bright +", function()
  local br = watch.get_setting("brightness") or 80
  br = br + 10
  if br > 100 then br = 100 end
  watch.set_setting("brightness", br)
  show()
end)

watch.button("Lock 30s", function()
  watch.set_setting("soft_lock", 30)
  show()
end)

watch.button("Lock 60s", function()
  watch.set_setting("soft_lock", 60)
  show()
end)

watch.button("Lock off", function()
  watch.set_setting("soft_lock", 0)
  show()
end)

watch.button("PIN 1234", function()
  watch.set_setting("pin", "1234")
  show()
end)
