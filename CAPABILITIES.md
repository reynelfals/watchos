# WatchOS capabilities matrix

Hardware target: Waveshare ESP32-S3-Touch-AMOLED-2.06.

| Capability | Status | Demo / notes |
|------------|--------|--------------|
| Display (CO5300 AMOLED) | done | Native shell + Lua draw |
| Touch (FT3168) | done | `watch.touch` |
| Battery / charging (AXP2101) | **done** | **`power_lab`** — `watch.battery`/`battery_pct`/`battery_mv`/`charging`/`usb_power`/`battery_connected` |
| Haptics (GPIO18 motor) | done | `watch.vibrate` / Counter |
| RTC (PCF85063) | done | Clock cover |
| IMU (QMI8658) | done | `watch.imu` / KidCoder shake / Tilt Maze |
| Wi-Fi | **done (API)** | Off by default; SoftAP/STA OTA + `watch.wifi_connect` / `http_get` (explicit) |
| LittleFS + **SD apps** | done | LittleFS `/apps/` + microSD `/sd/apps/` (merge, LFS wins on id); Counter, Squish ID, KidCoder, Tilt Maze, SD Lab, UART Chat, Power Lab, Waterfall, PCM Lab, Sound Deck; **uninstall** Manage / SoftAP; see `docs/SD_APPS.md` |
| **microSD (1-bit SDMMC)** | **done** | **`sd_lab`** — lazy `watch.sd_*` mount under `/sd` |
| **USB SD (MSC)** | **optional `-msc` env** | Install → **USB SD** / Lua `usb_sd` — only on `waveshare-amoled-206-msc` (TinyUSB); default build keeps ACM0 flash |
| Mic / spectrum (ES7210) | **done (API)** | **`waterfall`** (coarse FFT) + **`pcm_lab`** (fine PCM); `audio_ready` / `mic_start` / `mic_stop` / `mic_info` / `mic_read` / `mic_read_table` / `mic_spectrum` |
| Speaker (ES8311) + SD WAV | **done (best-effort)** | **`sound_deck`** — `mic_record_file` → `/sd/recordings/*.wav`; `speaker_play` / `speaker_start`/`write`/`stop`; `speaker_volume` 0..100%; PA=GPIO46; see `docs/SOUND_DECK.md` |
| **LoRa UART pads** | **done (API)** | **`uart_chat`** — `watch.uart_*` on TX=43 RX=44 |
| Zip install | **done** | SoftAP `POST /upload_zip` + `watch.install_zip`; see `docs/ZIP_INSTALL.md` |
| **WASM apps (wasm3)** | **done (v1)** | **`wasm_hello`** — `kind:wasm` / `main.wasm`; host imports audio/canvas/label/log; runs on UI core (dual-core OOS); see `docs/WASM_APPS.md` |
| **Background jobs** | **done (v1)** | **`job_lab`** — `watch.job_start/status/result/cancel`; named `fft`/`mic_fft`/`rms` on non-UI core (core 0); no core IDs in Lua |

## microSD details

- Pins: CLK=2 CMD=1 DATA=3 CS=17 (1-bit SD_MMC; Waveshare `07_LVGL_SD_Test`)
- Mount point: `/sd`
- Mount is **lazy** (Lua `watch.sd_mount()`); not at boot
- Paths confined under `/sd/...` (no escape to LittleFS)

## Lua SD API

- `watch.sd_ready()` -> bool
- `watch.sd_mount()` -> bool (idempotent)
- `watch.sd_unmount()` -> bool
- `watch.sd_info()` -> `{total_mb, used_mb, total_bytes, used_bytes}` or nil
- `watch.sd_list(path)` -> `{ {name, size, is_dir}, ... }` (max 64) or nil
- `watch.sd_read(path, max_bytes?)` -> string or nil
- `watch.sd_write(path, data)` -> bool
- `watch.sd_mkdir(path)` -> bool (create dir under `/sd/...`)
- `watch.sd_put_lfs(lfs_src, sd_dst)` -> bool (binary copy LittleFS → SD)
- `watch.lfs_list(path)` -> `{ {name, size, is_dir}, ... }` for `/apps/...` only
- `watch.safe_inset()` / `watch.corner_inset()` -> layout numbers


## UART pads (TX=43 RX=44)

- Peripheral: ESP32-S3 UART1 on board pads **TX=GPIO43 RX=GPIO44**
- Demo app: **`uart_chat`** (Open 115200 / Close / Send hello / PING / WATCHOS / Read / Clear)
- **Loopback:** jumper TX to RX for echo. Without loopback, `uart_write` still returns byte count (`w=N`) but Read stays empty unless a peer replies.
- USB CDC console (`Serial`) is unchanged.

### Lua UART API

- `watch.uart_ready()` -> bool
- `watch.uart_open(baud?)` -> bool (default 115200)
- `watch.uart_close()` -> bool
- `watch.uart_available()` -> int
- `watch.uart_write(data)` -> int bytes or nil
- `watch.uart_read(max_bytes?)` -> string or nil (cap 512)


## Battery / charging (AXP2101)

- PMIC: **AXP2101** on shared I2C (SDA=15 SCL=14, addr 0x34)
- Demo app: **`power_lab`** (live % / mV / charging / USB, tick or Refresh)
- All status flags are **real PMIC register reads**; if PMIC init failed, APIs return **nil** (never faked)

### Lua power API

- `watch.battery()` / `watch.battery_pct()` -> int 0..100 or nil
- `watch.battery_mv()` -> int millivolts (AXP2101 ADC) or nil
- `watch.charging()` -> bool or nil (`power.isCharging()`)
- `watch.usb_power()` -> bool or nil (`power.isVbusIn()`)
- `watch.battery_connected()` -> bool or nil (`power.isBatteryConnect()`)

### Gaps

- No separate GPIO charge-LED sense (PMIC LED mode unused)
- `usb_power` is VBUS-good via PMIC, not a distinct USB-data detect
- Fuel-gauge % can fall back to a voltage estimate when gauge percent is out of range

## Squish ID images

- Format: custom **WRGB** (magic `WRGB` + RGB565 LE); max 160x160
- Paths: `/sd/squish/img/<slug>.wrgb` then `/apps/squish_id/img/<slug>.wrgb`
- Lua: `watch.image` / `watch.image_clear` / `watch.image_exists` / `watch.squish_image_path`
- See `data/apps/squish_id/IMAGES.md`

## USB SD (mass storage)

- **Default firmware** (`waveshare-amoled-206`, `USB_MODE=1`): HW CDC/JTAG for reliable ACM0 esptool; USB SD button reports need for `-msc` build
- **Optional** `waveshare-amoled-206-msc` (`USB_MODE=0`): TinyUSB MSC + SD_MMC `readRAW`/`writeRAW`
- Enter/Exit: **Install → USB SD** (native) or Lua app **`usb_sd`** (MSC build only)
- While active: FatFS file APIs blocked; CDC may pause — exit before flash
- Workflow: enter MSC → PC mounts FAT → copy to `squish/img/*.wrgb` → eject → Exit MSC → unplug
- See `docs/USB_SD.md` and `docs/USB_FLASH.md`

### Lua MSC API

- `watch.usb_msc_active()` -> bool
- `watch.usb_msc_enter()` -> bool
- `watch.usb_msc_exit()` -> bool
- `watch.usb_msc_status()` -> string


## Expanded Lua API (2026-09)

### Layout widgets (LVGL on stub screen)

- `watch.label(x,y,text,r?,g?,b?)` -> bool — draw-layer label (same layer as `text_at`)
- `watch.panel(x,y,w,h,r?,g?,b?)` -> bool — rounded panel on draw layer
- `watch.progress(x,y,w,h,value0_100?)` -> id|nil — LVGL bar (max 4)
- `watch.progress_set(id, value)` -> bool
- `watch.slider(x,y,w,h,min?,max?,value?,cb?)` -> id|nil — clickable on stub (above draw); cb(value)
- `watch.z_raise("draw"|"buttons"|"back"|"all")` — stack helper
- **Z-order:** draw (`luaLayer`: rect/circle/label/panel/progress/image) under buttons column + native Back; sliders on stub with buttons. Use `z_raise` after heavy draw.

### Input

- `watch.on_back(fn)` — native Back + left-edge swipe-right call `fn` first (return false to fall through to default exit)
- `watch.on_long_press(fn)` — fn(x,y) after ~600ms hold
- `watch.gesture()` -> `{type,dx,dy}` once or nil (`swipe_left/right/up/down`)
- `watch.touch_delta()` -> `{dx,dy,pressed,long}`

### Storage

- `watch.prefs_get(key, default?)` / `watch.prefs_set(key, value)` — NVS ns `wlua` (key≤15, value≤128)
- `watch.sd_remove(path)` — recursive under `/sd` (path-safe)
- `watch.sd_rename(from,to)` — within `/sd`

### Time

- `watch.now()` -> `{epoch,year,month,day,hour,min,sec,wday,rtc}` from PCF85063 when available
- `watch.set_alarm(...)` -> `false, "planned"` stub

### Net (Wi‑Fi **off by default**; never auto at boot)

- `watch.wifi_state()` -> `"off"|"ota"|"on"|"connecting"`
- `watch.wifi_connect(ssid, pass?, timeout_ms?)` -> ok, ip_or_err
- `watch.wifi_disconnect()` -> bool
- `watch.http_get(url, max_bytes?)` -> body, code | nil, err (timeout 8s, default 32KB, hard cap 64KB)

### Audio (ES7210 mic) — coarse vs fine

Capture is mono int16 @ 8/16/48 kHz (default 16 kHz). `mic_spectrum` / `mic_fft` and
`mic_read` / `mic_read_table` share the same I2S stream (competing consumers).

**Coarse (fast convenience — native FFT in firmware):**

- `watch.audio_ready()` -> bool (ES7210 probed)
- `watch.mic_start({rate=16000}|rate?)` -> true | false, err
- `watch.mic_stop()` -> true
- `watch.mic_spectrum(bins?)` / `watch.mic_fft(bins?)` -> `{m0..}` magnitudes in 0..1 (bins 8..128, default 64) | nil, err
  - Error when mic not started: nil, `"mic_not_running"`

**Fine (apps implement their own DSP / FFT; WASM apps use the same mic_* host imports (see docs/WASM_APPS.md)):**

- `watch.mic_info()` -> `{sample_rate, bits=16, channels=1, running}` (always a table; `running=false` / `sample_rate=0` when stopped)
- `watch.mic_read(max_samples?)` -> `pcm_string, count` | nil, err
  - `pcm_string` = raw **little-endian int16** mono bytes (`#pcm == count*2`)
  - `max_samples` default 256, hard cap **1024**
  - Mic not started: nil, `"mic_not_running"` (may return `count=0` with empty string if running but no I2S data yet)
- `watch.mic_read_table(max_samples?)` -> `{s0,s1,...}, count` | nil, err (optional; prefer `mic_read` for speed)

**Record / speaker (SD WAV):**

- `watch.mic_record_file(path, seconds?, rate?)` -> true | false, err (mono int16 WAV under `/sd/...`, max 20 s)
- `watch.mic_record(path, seconds?)` — alias for `mic_record_file` when path given; no-args still returns nil, `"use_mic_spectrum_or_mic_read"`
- `watch.speaker_ready()` -> bool (ES8311 probed)
- `watch.speaker_play(path)` -> true | false, err (WAV mono/stereo int16 or raw mono `.pcm`/`.raw`)
- `watch.speaker_start({rate=16000}|rate?)` / `speaker_write(pcm)` / `speaker_stop()` — streaming stereo int16 frames
- `watch.speaker_volume()` -> pct (0..100); `watch.speaker_volume(n)` -> true | false, err — linear map onto ES8311 DAC reg `0x32` (`0x00`=-95.5dB … `0xBF`=0dB). Session only; **no global Settings/BOOT/PWR volume**. Apps own UX.
- `watch.speaker_volume_raw()` / `speaker_volume_raw(n)` — optional lab access to raw reg (`0..0xBF`, clamped)
- Demo: **`sound_deck`** (Vol-/Vol+); details in `docs/SOUND_DECK.md`

### BLE / Meshtastic mesh chat

- `watch.ble_ready()` -> bool (NimBLE central up)
- `watch.ble_status()` -> `"idle"` \| `"scanning"` \| `"connecting"` \| `"connected"` \| error string
- `watch.ble_scan(timeout_ms?)` -> list of `{addr,name,rssi}` (Meshtastic service advertisers)
- `watch.ble_connect(addr, pin?)` -> true \| false, err — omit/`nil` pin = **NO_PIN**; string = 4–6 digit passkey
- `watch.ble_disconnect()` -> bool
- `watch.mesh_connected()` -> bool
- `watch.mesh_send(text)` -> true | false, err — TEXT_MESSAGE_APP on current channel (default **1**)
- `watch.mesh_poll()` -> list of `{from,text,time?}` (drain host queue)
- `watch.mesh_channel()` / `(0..7)` -> get/set channel (NVS, default 1)
- `watch.mesh_alert_beep()` / `(bool)` -> inbound speaker beep (default true)
- `watch.mesh_alert_vibrate()` / `(bool)` -> inbound vibrate (default true; needs motor)
- `watch.notify(app_id, text)` -> bool — top banner tied to `app_id`; **swipe-down opens that app**
- `watch.beep([ms],[hz])` -> bool — short ES8311 tone (defaults 100ms / 880Hz, full vol)
- `watch.alert_uh_oh()` -> bool — ICQ-style ascending-then-descending alert (ES8311)
- Legacy: `ble_start` → short scan; `ble_stop` → disconnect
- `watch.speech_to_text(secs?)` -> text \| nil, err — legacy host API retained if already built; unused by Mesh Chat
- Demo: **`mesh_chat`** 1.1.0 — ch1 default, Exit keeps BLE, uh-oh beep+banner (swipe-down opens); see `docs/MESH_CHAT.md`

### Zip / sprites / power / GPIO

- `watch.install_zip(path, app_id, dest?)` -> ok, err (`dest`=`lfs`|`sd`)
- SoftAP: `POST /upload_zip?app=&dest=lfs|sd&k=` — see `docs/ZIP_INSTALL.md`
- `watch.sprite` / `watch.image_frame(path,x,y,fw,fh,frame?,max_w?,max_h?)` — WRGB sheet frame blit
- `watch.brightness()` get / `watch.brightness(0..100)` set
- `watch.idle_timeout()` get / `watch.idle_timeout(sec)` set (runtime soft-lock on launcher/shell only; **not** while Lua/WASM app open; 0 disables)
- `watch.gpio_pwm/adc/write/read` — whitelist pins **10,16,19,20** only

### Demos

- `prefs_lab` — prefs / now / brightness / idle / stubs / on_back
- `layout_lab` — panel / label / progress / slider / gesture / long-press
- `waterfall` — mic spectrum scrolling waterfall (`python3 sim/run.py waterfall --ticks 20`)
- `pcm_lab` — fine `mic_read` RMS meter (`python3 sim/run.py pcm_lab --ticks 20`)
- `sound_deck` — record/play WAV on SD + Vol-/Vol+ (`python3 sim/run.py sound_deck --click Vol- --click Vol+ --click Record --click Play`)
- `brainfuck` (BF Lab) 1.2 — BF editor/runner; SD `/sd/bf/*.bf`; GPIO 60–63; pixel I/O 48–58 (plot + 16×16 FB canvas) (`docs/BRAINFUCK.md`)

### Sim

- `sim/watch_host.py` + `sim/run.py` stub every new API
- `python3 sim/test_watch_api.py`
