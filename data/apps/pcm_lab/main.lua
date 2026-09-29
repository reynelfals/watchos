-- PCM Lab: fine mic PCM via watch.mic_read (raw int16 LE).
-- Coarse path remains watch.mic_spectrum (native FFT) — see waterfall.
-- Future WASM apps would import the same fine PCM surface.

local status = "init"
local running = false
local frames = 0
local last_rms = 0
local last_n = 0

local function stop_mic()
  if watch.mic_stop then watch.mic_stop() end
  running = false
end

local function start_mic()
  if not watch.audio_ready or not watch.audio_ready() then
    status = "mic not ready"
    return false
  end
  if not watch.mic_start then
    status = "no mic_start"
    return false
  end
  local ok, err = watch.mic_start({ rate = 16000 })
  if not ok then
    status = "start: " .. tostring(err or "?")
    return false
  end
  running = true
  status = "listening"
  return true
end

local function rms_from_pcm(pcm)
  if type(pcm) ~= "string" or #pcm < 2 then return 0, 0 end
  local n = math.floor(#pcm / 2)
  local sum = 0
  for i = 1, n do
    local lo = string.byte(pcm, (i - 1) * 2 + 1) or 0
    local hi = string.byte(pcm, (i - 1) * 2 + 2) or 0
    local v = lo + hi * 256
    if v >= 32768 then v = v - 65536 end
    sum = sum + v * v
  end
  return math.sqrt(sum / n), n
end

local function refresh()
  local info = watch.mic_info and watch.mic_info() or nil
  local rate = info and info.sample_rate or 0
  local bits = info and info.bits or 16
  local ch = info and info.channels or 1
  local run = info and tostring(info.running) or "?"
  local body = string.format(
    "PCM Lab\n\n%s\nframes=%d n=%d\nrms=%.1f\nrate=%d bits=%d ch=%d\nrunning=%s\n\nmic_read = int16 LE string\nmic_spectrum = native FFT",
    status, frames, last_n, last_rms, rate, bits, ch, run)
  watch.set_text(body)
end

local function paint()
  watch.clear_ui()
  if watch.button_layout then watch.button_layout("column") end
  watch.fill(10, 14, 22)
  watch.set_title("PCM Lab")
  refresh()
  watch.button(running and "Stop" or "Start", function()
    if running then
      stop_mic()
      status = "stopped"
    else
      start_mic()
    end
    paint()
  end)
  if watch.z_raise then watch.z_raise("buttons") end
end

if watch.on_back then
  watch.on_back(function()
    stop_mic()
    watch.back()
  end)
end

if watch.audio_ready and watch.audio_ready() then
  start_mic()
else
  status = "mic not ready"
end
paint()

if watch.on_tick then
  watch.on_tick(function()
    if not running then return end
    if not watch.mic_read then
      status = "no mic_read"
      refresh()
      return
    end
    local pcm, n = watch.mic_read(256)
    if not pcm then
      status = "read: " .. tostring(n or "?")
      refresh()
      return
    end
    last_rms, last_n = rms_from_pcm(pcm)
    if type(n) == "number" then last_n = n end
    frames = frames + 1
    if frames % 3 == 0 then
      status = "live"
      refresh()
    end
  end)
end
