-- Mesh Chat v1.2.0 — Meshtastic BLE GATT text chat
-- Default channel 1 (Family). Exit keeps BLE for bg beep/banner.
-- Fast input: phrase chips + letter_pad compose + keyboard

local screen = "home" -- home | pin | chat | type
local status = "Tap Scan"
local hits = {}
local sel = 1
local pin = ""
local msgs = {}
local MAX_MSG = 40
local compose = ""


local function ch_name(n)
  n = tonumber(n) or 1
  if n == 0 then return "LongFast" end
  if n == 1 then return "Family" end
  return "ch" .. tostring(n)
end

local function cur_ch()
  if watch.mesh_channel then return watch.mesh_channel() end
  return 1
end

local function set_ch(n)
  if watch.mesh_channel then watch.mesh_channel(n) end
  status = "ch:" .. tostring(cur_ch()) .. " " .. ch_name(cur_ch())
end

local function beep_on()
  if watch.mesh_alert_beep then return watch.mesh_alert_beep() end
  return true
end

local function vib_on()
  if watch.mesh_alert_vibrate then return watch.mesh_alert_vibrate() end
  return true
end

local PHRASES = {
  "OK", "Yes", "No", "Help", "Ping",
}

local THREAD_PATH = "/sd/mesh_chat/thread.txt"

local function thread_body()
  local parts = {}
  for i = 1, #msgs do
    local m = msgs[i]
    local fr = tostring(m.from or "?"):gsub("[\n\t]", " ")
    local tx = tostring(m.text or ""):gsub("[\n\t]", " ")
    parts[#parts + 1] = fr .. "\t" .. tx
  end
  return table.concat(parts, "\n")
end

local function parse_thread(s)
  if s == nil or s == "" then return end
  msgs = {}
  for line in string.gmatch(s, "[^\n]+") do
    local fr, tx = string.match(line, "^([^\t]*)\t(.*)$")
    if fr and tx and tx ~= "" then
      msgs[#msgs + 1] = { from = fr, text = tx }
    end
  end
  while #msgs > MAX_MSG do table.remove(msgs, 1) end
end

local function save_msgs()
  local body = thread_body()
  -- SD is the real store (prefs max 128 bytes — too small for a thread).
  if watch.sd_mount then watch.sd_mount() end
  if watch.sd_ready and watch.sd_ready() and watch.sd_write then
    if watch.sd_mkdir then watch.sd_mkdir("/sd/mesh_chat") end
    if watch.sd_write(THREAD_PATH, body) then return true end
  end
  if watch.prefs_set and #body <= 128 then
    return watch.prefs_set("mesh_chat_thread", body) and true or false
  end
  return false
end

local function load_msgs()
  if watch.sd_mount then watch.sd_mount() end
  if watch.sd_ready and watch.sd_ready() and watch.sd_read then
    local s = watch.sd_read(THREAD_PATH, 8192)
    if type(s) == "string" and s ~= "" then
      parse_thread(s)
      return
    end
  end
  if watch.prefs_get then
    local s = watch.prefs_get("mesh_chat_thread", "")
    if s and s ~= "" then parse_thread(s) end
  end
end

local function push_msg(from, text)
  -- Chronological: oldest at top, newest at bottom.
  table.insert(msgs, { from = from or "?", text = text or "" })
  while #msgs > MAX_MSG do table.remove(msgs, 1) end
  save_msgs()
end

local function trunc(s, n)
  s = tostring(s or "")
  if #s <= n then return s end
  return string.sub(s, 1, n - 2) .. ".."
end

local show_home
local show_pin
local show_chat
local show_type

local function drain_poll()
  if not watch.mesh_poll then return end
  local list = watch.mesh_poll()
  if list == nil then return end
  local i = 1
  while list[i] ~= nil do
    local m = list[i]
    local fr = m.from or m["from"] or "?"
    local tx = m.text or ""
    if tx ~= "" then push_msg(fr, tx) end
    i = i + 1
  end
end

local function refresh_home()
  local ch = cur_ch()
  local lines = {
    "Mesh Chat",
    "ch:" .. tostring(ch) .. " " .. ch_name(ch),
    "st=" .. tostring(watch.ble_status and watch.ble_status() or "?"),
    status, "",
  }
  if #hits == 0 then
    table.insert(lines, "(no devices — Scan)")
  else
    for i = 1, #hits do
      local h = hits[i]
      local mark = (i == sel) and ">" or " "
      local nm = h.name
      if nm == nil or nm == "" then nm = "mesh" end
      table.insert(lines, string.format("%s%d %s %ddBm", mark, i, trunc(nm, 14), h.rssi or 0))
      table.insert(lines, "  " .. trunc(h.addr or "", 20))
    end
  end
  table.insert(lines, "")
  table.insert(lines, "Exit keeps link (bg alert)")
  table.insert(lines, "Beep=" .. (beep_on() and "on" or "off")
    .. " Vib=" .. (vib_on() and "on" or "off"))
  watch.set_text(table.concat(lines, "\n"))
end

local function refresh_pin()
  local stars = string.rep("*", #pin) .. string.rep("_", math.max(0, 6 - #pin))
  watch.set_text("Enter BLE PIN\n\n" .. stars .. "\n\n" .. status .. "\n\ndigits + OK (auto at 6)")
end

local function refresh_chat()
  local conn = watch.mesh_connected and watch.mesh_connected()
  local ch = cur_ch()
  -- Compact header so message lines stay above the button strip.
  local lines = {
    (conn and "up" or "down") .. " ch" .. tostring(ch) .. " " .. trunc(status, 18),
    "draft: " .. trunc(compose, 28),
    "",
  }
  if #msgs == 0 then
    table.insert(lines, "(no messages)")
  else
    -- Chronological window: show only the newest VISIBLE lines (oldest→newest).
    local VISIBLE = 4
    local start_i = #msgs - VISIBLE + 1
    if start_i < 1 then start_i = 1 end
    if start_i > 1 then table.insert(lines, "…") end
    for i = start_i, #msgs do
      local m = msgs[i]
      table.insert(lines, trunc(tostring(m.from), 10) .. ": " .. trunc(m.text, 22))
    end
  end
  watch.set_text(table.concat(lines, "\n"))
end

local function refresh_type()
  watch.set_text("Type message\n\n" .. (compose ~= "" and compose or "_") .. "\n\n" .. status)
end

local function send_text(t)
  if t == nil or t == "" then
    status = "Empty"
    refresh_chat()
    return
  end
  if not watch.mesh_connected or not watch.mesh_connected() then
    status = "Not connected"
    refresh_chat()
    return
  end
  local ok, err = watch.mesh_send(t)
  if ok then
    status = "Sent"
    compose = ""
    push_msg("me", t)
    drain_poll()
  else
    status = "Send fail: " .. tostring(err)
  end
  refresh_chat()
end

local function do_connect(use_pin)
  if #hits == 0 then
    status = "Scan first"
    refresh_home()
    return
  end
  if sel < 1 or sel > #hits then sel = 1 end
  local addr = hits[sel].addr
  if not addr then
    status = "bad addr"
    refresh_home()
    return
  end
  status = "Connecting..."
  if use_pin then refresh_pin() else refresh_home() end
  local ok, err
  if use_pin then
    ok, err = watch.ble_connect(addr, pin)
  else
    ok, err = watch.ble_connect(addr, nil)
  end
  if ok then
    status = "Connected"
    show_chat()
  else
    status = "Fail: " .. tostring(err or watch.ble_status())
    if use_pin then refresh_pin() else refresh_home() end
  end
end

show_home = function()
  screen = "home"
  watch.clear_ui()
  watch.set_title("Mesh Chat")
  watch.fill(10, 18, 28)
  if watch.button_layout then watch.button_layout("grid2") end

  watch.button("Scan", function()
    status = "Scanning..."
    refresh_home()
    local raw = watch.ble_scan(4000) or {}
    local list = {}
    if type(raw) == "table" then
      local i = 1
      while raw[i] ~= nil do
        table.insert(list, raw[i])
        i = i + 1
      end
    end
    hits = list
    sel = 1
    status = string.format("Found %d", #hits)
    refresh_home()
  end)

  watch.button("Next", function()
    if #hits > 0 then sel = (sel % #hits) + 1 end
    refresh_home()
  end)

  watch.button("Connect", function()
    do_connect(false)
  end)

  watch.button("PIN…", function()
    if #hits == 0 then
      status = "Scan first"
      refresh_home()
      return
    end
    pin = ""
    status = "Enter PIN"
    show_pin()
  end)

  watch.button("Ch0", function()
    set_ch(0)
    refresh_home()
  end)

  watch.button("Ch1", function()
    set_ch(1)
    refresh_home()
  end)

  watch.button("Beep", function()
    if watch.mesh_alert_beep then
      watch.mesh_alert_beep(not beep_on())
    end
    refresh_home()
  end)

  watch.button("Vib", function()
    if watch.mesh_alert_vibrate then
      watch.mesh_alert_vibrate(not vib_on())
    end
    refresh_home()
  end)

  -- Exit keeps BLE so background beep/banner still work
  watch.button("Exit", function()
    save_msgs()
    watch.back()
  end)

  refresh_home()
end

show_pin = function()
  screen = "pin"
  watch.clear_ui()
  watch.set_title("Mesh PIN")
  watch.fill(10, 18, 28)
  if watch.button_layout then watch.button_layout("grid2") end
  if watch.on_back then
    watch.on_back(function()
      show_home()
    end)
  end

  local function digit(d)
    return function()
      if #pin < 6 then
        pin = pin .. d
        refresh_pin()
        if #pin >= 6 then
          do_connect(true)
        end
      end
    end
  end
  for _, d in ipairs({"1","2","3","4","5","6","7","8","9","0"}) do
    watch.button(d, digit(d))
  end
  watch.button("BKSP", function()
    if #pin > 0 then pin = string.sub(pin, 1, #pin - 1) end
    refresh_pin()
  end)
  watch.button("OK", function()
    if #pin < 4 then
      status = "Need 4-6 digits"
      refresh_pin()
      return
    end
    do_connect(true)
  end)
  status = "PIN then OK (or 6 digits)"
  refresh_pin()
end

show_type = function()
  screen = "type"
  watch.clear_ui()
  watch.set_title("Compose")
  watch.fill(10, 18, 28)
  if watch.button_layout then watch.button_layout("grid2") end
  if watch.on_back then
    watch.on_back(function()
      show_chat()
    end)
  end

  local function do_send()
    local t = compose
    if not t or t == "" then
      status = "Empty"
      refresh_type()
      return
    end
    show_chat()
    send_text(t)
  end

  -- Squish order: letter_pad FIRST (sizes the button strip), then Spc/Send.
  if watch.letter_pad then
    local ok, err = pcall(function()
      watch.letter_pad(function(key)
        if key == "BKSP" then
          if #compose > 0 then compose = string.sub(compose, 1, #compose - 1) end
        elseif key == "CLR" then
          compose = ""
        elseif key == "SPC" then
          if #compose < 48 then compose = compose .. " " end
        elseif key == "SEND" then
          do_send()
          return
        elseif type(key) == "string" and #key == 1 then
          if #compose < 48 then compose = compose .. key end
        end
        refresh_type()
      end)
    end)
    if not ok then
      status = "Pad fail: " .. tostring(err)
    else
      status = "Type · Spc/Send above pad"
    end
  else
    status = "No letter_pad"
  end

  watch.button("Spc", function()
    if #compose < 48 then compose = compose .. " " end
    refresh_type()
  end)
  watch.button("Send", function()
    do_send()
  end)

  refresh_type()
end

show_chat = function()
  screen = "chat"
  watch.clear_ui()
  watch.set_title("Mesh Chat")
  watch.fill(10, 18, 28)
  if watch.button_layout then watch.button_layout("grid2") end

  -- Phrase chips: tap sends immediately
  for _, ph in ipairs(PHRASES) do
    local label = ph
    if #label > 8 then label = string.sub(label, 1, 7) .. "…" end
    local full = ph
    watch.button(label, function()
      send_text(full)
    end)
  end

  watch.button("Type", function()
    show_type()
  end)

  watch.button("Send", function()
    send_text(compose)
  end)

  watch.button("Poll", function()
    drain_poll()
    status = "Polled"
    refresh_chat()
  end)

  watch.button("Disc", function()
    if watch.ble_disconnect then watch.ble_disconnect() end
    status = "Disconnected"
    show_home()
  end)

  -- Exit keeps BLE connected for background alerts
  watch.button("Exit", function()
    save_msgs()
    watch.back()
  end)

  if watch.on_tick then
    watch.on_tick(function()
      if screen == "chat" then
        drain_poll()
        refresh_chat()
      end
    end)
  end

  refresh_chat()
end

-- Prefer channel 1 (Family) when host exposes API
if watch.mesh_channel then
  local ch = watch.mesh_channel()
  if ch == nil then watch.mesh_channel(1) end
end

-- Notif swipe-down deep link: open chat + compose ready to reply.
local launch = watch.launch_action and watch.launch_action() or nil
local ltext = watch.launch_text and watch.launch_text() or nil
load_msgs()
drain_poll()

if launch == "reply" then
  if ltext and ltext ~= "" then
    -- Avoid dup if drain_poll already pulled the same inbound.
    local last = msgs[#msgs]
    if not last or last.text ~= ltext then
      push_msg("inbox", ltext)
    else
      save_msgs()
    end
  end
  if watch.mesh_connected and watch.mesh_connected() then
    status = "Reply — Type to answer"
  else
    status = "Reply (link down)"
  end
  -- Show conversation (not blank Compose). Type is one tap away.
  show_chat()
else
  show_home()
end

print("Mesh Chat v1.2.0 ready (SD thread + reply opens chat)")
