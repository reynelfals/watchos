-- Sound Deck: record mic → /sd/recordings/*.wav, list, play via speaker.
-- Uses watch.mic_record_file + watch.speaker_play (ES7210 / ES8311).
-- Record/play are blocking (UI freezes for the duration).
-- Do not call paint()/clear_ui immediately before mic_record_file/speaker_play
-- (deep LVGL rebuild on the same loopTask stack caused device reboots).
-- Volume: app-owned Vol-/Vol+ via watch.speaker_volume (0..100%); no BOOT/PWR UX.

local REC_DIR = "/sd/recordings"
local status = "init"
local files = {}
local sel = 1
local rec_secs = 3
local vol_pct = 100
local VOL_STEP = 10

local ERR_HINT = {
  sd_not_ready = "no SD card / mount failed",
  sd_busy_usb = "SD busy (USB mass storage) — eject first",
  sd_open_failed = "cannot open file on SD",
  sd_write_failed = "SD write failed (card full?)",
  bad_path = "path must be under /sd/",
  es7210_missing = "mic chip missing (ES7210 @0x40)",
  es7210_init_failed = "mic init failed",
  es8311_missing = "speaker chip missing (ES8311 @0x18)",
  es8311_init_failed = "speaker init failed",
  es8311_vol_failed = "speaker volume write failed",
  i2s_begin_failed = "I2S bus start failed",
  i2s_write_short = "I2S underrun during play",
  mic_no_pcm = "mic gave no samples (I2S/pins?)",
  mic_near_silence = "recording nearly silent — check mic/I2S",
  mic_not_running = "mic not started",
  bad_wav = "not a RIFF/WAVE file",
  wav_not_pcm = "WAV must be PCM",
  wav_unsupported = "need 16-bit mono/stereo WAV",
  wav_empty = "WAV has no audio data",
  wav_near_silence = "WAV nearly silent (empty clip?)",
  speaker_not_running = "speaker not started",
  bad_args = "bad arguments",
}

local function friendly_err(err)
  if err == nil or err == "" or err == "?" then
    return "unknown error"
  end
  local e = tostring(err)
  return ERR_HINT[e] or e
end

local function sync_vol()
  if watch.speaker_volume then
    local v = watch.speaker_volume()
    if type(v) == "number" then
      vol_pct = math.max(0, math.min(100, math.floor(v + 0.5)))
    end
  end
end

local function set_vol(pct)
  pct = math.max(0, math.min(100, math.floor(pct + 0.5)))
  if watch.speaker_volume then
    local ok, err = watch.speaker_volume(pct)
    if ok == false then
      status = "vol: " .. friendly_err(err)
      return
    end
  end
  vol_pct = pct
  status = string.format("volume %d%%", vol_pct)
end

local function ensure_sd()
  if watch.usb_msc_active and watch.usb_msc_active() then
    status = "SD busy (USB) — eject first"
    return false
  end
  if not watch.sd_ready or not watch.sd_ready() then
    if not watch.sd_mount or not watch.sd_mount() then
      status = "SD mount failed — insert card"
      return false
    end
  end
  if watch.sd_mkdir then watch.sd_mkdir(REC_DIR) end
  return true
end

local function is_wav(name)
  if type(name) ~= "string" then return false end
  return string.lower(string.sub(name, -4)) == ".wav"
end

local function refresh_list()
  files = {}
  if not ensure_sd() then return end
  local ents = watch.sd_list and watch.sd_list(REC_DIR) or nil
  if type(ents) ~= "table" then
    status = "list empty / fail (mkdir ok?)"
    return
  end
  for _, e in ipairs(ents) do
    if e and not e.is_dir and is_wav(e.name) then
      table.insert(files, { name = e.name, size = e.size or 0 })
    end
  end
  table.sort(files, function(a, b) return a.name < b.name end)
  if sel > #files then sel = #files end
  if sel < 1 then sel = 1 end
  status = string.format("%d wav(s) — Rec/Play block UI", #files)
end

local function selected_path()
  if #files < 1 or not files[sel] then return nil end
  return REC_DIR .. "/" .. files[sel].name
end

local function make_name()
  local n = watch.now and watch.now() or nil
  local stamp = "clip"
  if type(n) == "table" and n.epoch then
    stamp = string.format("%d", n.epoch)
  elseif watch.millis then
    stamp = string.format("%d", watch.millis())
  end
  return REC_DIR .. "/rec_" .. stamp .. ".wav"
end

local function do_record()
  if not ensure_sd() then return end
  if not watch.audio_ready or not watch.audio_ready() then
    status = "mic not ready (ES7210?)"
    return
  end
  if not watch.mic_record_file and not watch.mic_record then
    status = "no mic_record_file API"
    return
  end
  local path = make_name()
  status = "recording " .. tostring(rec_secs) .. "s (UI frozen)..."
  -- Status only — full paint()/clear_ui before blocking I/O spikes loopTask stack.
  if watch.set_text then watch.set_text(status) end
  local ok, err
  if watch.mic_record_file then
    ok, err = watch.mic_record_file(path, rec_secs, 16000)
  else
    ok, err = watch.mic_record(path, rec_secs)
  end
  if ok then
    status = "saved " .. (path:match("[^/]+$") or path)
    refresh_list()
    for i, f in ipairs(files) do
      if f.name == (path:match("[^/]+$") or "") then sel = i break end
    end
  else
    status = "rec: " .. friendly_err(err)
  end
end

local function do_play()
  local path = selected_path()
  if not path then
    status = "nothing selected — Record or Refresh"
    return
  end
  if not watch.speaker_play then
    status = "no speaker_play API"
    return
  end
  if watch.speaker_ready and not watch.speaker_ready() then
    status = "speaker not ready (ES8311 / PA46?)"
    return
  end
  -- Re-apply app volume before play (session; host also applies on start).
  if watch.speaker_volume then watch.speaker_volume(vol_pct) end
  status = "playing (UI frozen)..."
  if watch.set_text then watch.set_text(status) end
  local ok, err = watch.speaker_play(path)
  if ok then
    status = string.format("played ok @ %d%%", vol_pct)
  else
    status = "play: " .. friendly_err(err)
  end
end

local function do_delete()
  local path = selected_path()
  if not path then
    status = "nothing selected"
    return
  end
  if watch.sd_remove and watch.sd_remove(path) then
    status = "deleted"
    refresh_list()
  else
    status = "delete failed"
  end
end

local function do_secs()
  if rec_secs == 3 then
    rec_secs = 5
  elseif rec_secs == 5 then
    rec_secs = 10
  else
    rec_secs = 3
  end
  status = "record length " .. tostring(rec_secs) .. "s"
  paint()
end

function paint()
  watch.clear_ui()
  if watch.button_layout then watch.button_layout("grid2") end
  watch.fill(8, 12, 28)
  watch.set_title("Sound Deck")

  local mic = (watch.audio_ready and watch.audio_ready()) and "mic:ok" or "mic:?"
  local spk = (watch.speaker_ready and watch.speaker_ready()) and "spk:ok" or "spk:?"
  local sd = "?"
  if watch.usb_msc_active and watch.usb_msc_active() then
    sd = "usb"
  elseif watch.sd_ready and watch.sd_ready() then
    sd = "ok"
  else
    sd = "off"
  end
  local lines = {
    string.format("%s %s sd:%s %ds vol:%d%%", mic, spk, sd, rec_secs, vol_pct),
    status,
    "",
  }
  if #files == 0 then
    table.insert(lines, "(no recordings)")
  else
    local lo = math.max(1, sel - 2)
    local hi = math.min(#files, lo + 4)
    for i = lo, hi do
      local mark = (i == sel) and ">" or " "
      local f = files[i]
      local kb = math.floor((f.size or 0) / 1024)
      table.insert(lines, string.format("%s %s %dK", mark, f.name, kb))
    end
  end
  watch.set_text(table.concat(lines, "\n"))

  watch.button("Record", function()
    do_record()
    paint()
  end)
  watch.button("Play", function()
    do_play()
    paint()
  end)
  watch.button("Prev", function()
    if #files > 0 then
      sel = sel - 1
      if sel < 1 then sel = #files end
    end
    paint()
  end)
  watch.button("Next", function()
    if #files > 0 then
      sel = sel + 1
      if sel > #files then sel = 1 end
    end
    paint()
  end)
  watch.button("Refresh", function()
    refresh_list()
    paint()
  end)
  watch.button("Delete", function()
    do_delete()
    paint()
  end)
  watch.button("Vol-", function()
    set_vol(vol_pct - VOL_STEP)
    paint()
  end)
  watch.button("Vol+", function()
    set_vol(vol_pct + VOL_STEP)
    paint()
  end)
  -- Length cycle (9 buttons total; kMaxButtons=10)
  if watch.button then
    watch.button(string.format("%ds", rec_secs), function()
      do_secs()
    end)
  end
  if watch.z_raise then watch.z_raise("buttons") end
end

if watch.on_back then
  watch.on_back(function()
    if watch.mic_stop then watch.mic_stop() end
    if watch.speaker_stop then watch.speaker_stop() end
    watch.back()
  end)
end

ensure_sd()
sync_vol()
refresh_list()
paint()
