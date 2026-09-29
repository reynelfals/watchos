-- Mic Waterfall: realtime scrolling spectrum (newest row at BOTTOM).
-- Device: ES7210 mic + native FFT via watch.mic_spectrum / mic_fft.
-- Sim: synthetic noise + tone from watch_host.
-- For custom DSP / Lua FFT use watch.mic_read (raw int16 LE) — see pcm_lab.

local BINS = 64
local status = "init"
local running = false
local frames = 0
local CW, CH = 360, 280
local CX, CY = 25, 70

local function heat_ok()
  return watch.canvas and watch.canvas_row and watch.canvas_scroll
end

local function show_status()
  watch.set_text(string.format("%s\nframes=%d bins=%d", status, frames, BINS))
end

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
    status = "no mic_start API"
    return false
  end
  local ok, err = watch.mic_start({ rate = 16000 })
  if not ok then
    status = "start fail: " .. tostring(err or "?")
    return false
  end
  running = true
  status = "listening"
  return true
end

local function paint()
  watch.clear_ui()
  if watch.button_layout then watch.button_layout("column") end
  watch.fill(6, 8, 14)
  watch.set_title("Waterfall")

  if heat_ok() then
    watch.canvas(CX, CY, CW, CH)
    watch.canvas_clear(4, 6, 12)
  end
  if watch.label then
    watch.label(CX, CY - 28, "newest at bottom", 120, 200, 255)
  else
    watch.text_at(CX, CY - 28, "newest at bottom", 120, 200, 255)
  end
  show_status()

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
    if not watch.mic_spectrum then return end
    local spec, err = watch.mic_spectrum(BINS)
    if not spec then
      status = "spec: " .. tostring(err or "?")
      show_status()
      return
    end
    frames = frames + 1
    if heat_ok() then
      -- Scroll content up; newest spectrum on the bottom row.
      watch.canvas_scroll(-1)
      watch.canvas_row(CH - 1, spec)
    elseif watch.gfx_clear and watch.rect then
      watch.gfx_clear()
      local bar_w = math.max(2, math.floor(CW / BINS))
      for i = 1, BINS do
        local v = spec[i] or 0
        local h = math.floor(v * (CH - 4))
        if h < 1 then h = 1 end
        local x = CX + (i - 1) * bar_w
        local y = CY + CH - h
        watch.rect(x, y, bar_w - 1, h,
          math.floor(40 + v * 215),
          math.floor(80 + v * 100),
          math.floor(255 - v * 200))
      end
    end
    if frames % 5 == 0 then
      status = "live"
      show_status()
    end
  end)
end
