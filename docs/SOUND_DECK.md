# Sound Deck — mic record / speaker play

Demo app: **`sound_deck`** (`data/apps/sound_deck/`).

## Hardware

| Part | Role | Notes |
|------|------|--------|
| ES7210 @ I2C `0x40` | Mic ADC | DIN = GPIO42 (`I2S_ASDOUT`) |
| ES8311 @ I2C `0x18` | Speaker DAC | DOUT = GPIO40 (`I2S_DSDIN`) |
| NS4150B PA | Amp enable | GPIO46 (`PA_CTRL`) — HIGH only while playing |
| I2S clocks | Shared | MCLK=16, BCLK=41, WS=45 |

Mic and speaker share one I2S bus; the host **stops** the other path before starting a new one (not full-duplex).

## SD layout

Recordings land under:

```
/sd/recordings/*.wav
```

Format: **PCM WAV**, mono, int16 LE, default **16 kHz** (also 8 / 48 kHz).

## Lua API

```lua
-- Mic (ES7210) — existing
watch.audio_ready()
watch.mic_start({ rate = 16000 })
watch.mic_read(256)          -- fine PCM string
watch.mic_spectrum(64)       -- coarse FFT
watch.mic_stop()

-- Record whole clip to SD (blocking, max 20 s)
watch.mic_record_file("/sd/recordings/a.wav", 3, 16000)  -- ok | false, err
watch.mic_record(path, seconds)  -- alias when path given

-- Speaker (ES8311) — unstubbed best-effort
watch.speaker_ready()
watch.speaker_play("/sd/recordings/a.wav")  -- WAV mono/stereo int16, or raw .pcm
watch.speaker_start({ rate = 16000 })
watch.speaker_write(stereo_pcm_string)      -- int16 LE frames, # % 4 == 0
watch.speaker_stop()

-- Volume (session; apps own UX — no global Settings/BOOT/PWR)
watch.speaker_volume()      -- get 0..100
watch.speaker_volume(70)    -- set → true | false, err
watch.speaker_volume_raw()  -- optional lab: ES8311 reg 0x32 (0..0xBF)
```

`sd_write` alone is capped at 8 KiB per call, so multi-second captures use **`mic_record_file`** (native streaming write) rather than app-level chunk loops.

## Sim

```bash
python3 sim/test_watch_api.py
python3 sim/run.py sound_deck --click Vol- --click Vol+ --click Record --click Play --out /tmp/sound_deck.png
```

Sim synthesizes a tone into the WAV and treats `speaker_play` as success without audible output.

## Firmware note

ES8311 bring-up is **best-effort** (Waveshare-style register sequence + Espressif 256×Fs coeffs).

**DAC volume (reg `0x32`)** — datasheet: `0x00` = **−95.5 dB** (near silent), `0xBF` = **0 dB**, `0xFF` = +32 dB (boost unused — host clamps max at `0xBF`). Lua `speaker_volume` maps **0..100% linearly** onto `0x00..0xBF`. Default session level is 100% / `0xBF`. Host applies the current session volume (unmute `0x31`) on every speaker start/play. **Not persisted globally** — each app provides its own Vol UI (Sound Deck: **Vol-** / **Vol+** in ~10% steps). An earlier bug wrote `0x00` thinking it was “max volume”; Play returned success with no audible output.

**PA:** GPIO46 (`PA_CTRL`) driven **HIGH** for the entire play (asserted at start, re-held during stream, dropped only in `speaker_stop`). NS4150B settle ~80 ms after enable.

Serial breadcrumbs: `audio: speaker start`, `audio: ES8311 vol unmute ACK…`, `audio: speaker play begin … PA=`, `audio: played … peak=`.

## App UI (sound_deck 1.2+)

- Status line shows `vol:N%` alongside mic/spk/sd/length.
- **Vol-** / **Vol+** adjust by 10%, clamped 0..100, via `watch.speaker_volume`.
- No dependency on BOOT/PWR or a global Settings volume screen.

## Failure modes (device)

| Symptom | Likely cause | Error / check |
|---------|----------------|---------------|
| Record fails immediately | No SD / USB MSC owns card | `sd_not_ready`, `sd_busy_usb` |
| Record fails after wait | No I2S PCM / wrong slot | `mic_no_pcm`, `mic_near_silence` |
| Play says ok but silent | Old firmware with reg32=`0x00`, PA off, empty WAV | Flash fix; GPIO46 HIGH; serial `reg32=0xBF`; `wav_near_silence` |
| Play fails near-silence | Empty/zero PCM clip | `wav_empty`, `wav_near_silence` |
| UI frozen during Rec/Play | Expected | Native APIs block; status says so |
| Device reboot on Rec/Play | loopTask stack overflow / TWDT | Fixed: PCM buffers off stack, 16 KiB loop stack, WDT pump, no paint before block |
| Mic/speaker conflict | Shared I2S | Host stops the other path first |

Firmware rejects near-silent captures (peak &lt; 64) and deletes the incomplete file so the list is not littered with empty WAVs. Play also rejects near-silent WAVs (`wav_near_silence`).

