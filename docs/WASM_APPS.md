# WatchOS WASM apps (v1)

Firmware embeds **wasm3** (MIT) under `lib/wasm3` and runs `.wasm` packages
on the **same app/UI core as Lua**. Dual-core offload is **out of scope for v1**.

## Packaging

Folder under LittleFS `/apps/<id>/` or SD `/sd/apps/<id>/`:

```
app.json
main.wasm          # or whatever "entry" names
icon.wrgb          # optional launcher icon
```

`app.json` example:

```json
{
  "id": "wasm_hello",
  "name": "WASM Hello",
  "version": "0.1.0",
  "kind": "wasm",
  "entry": "main.wasm",
  "icon": "icon.wrgb"
}
```

Detection rules (first match wins for kind):

1. `"kind": "wasm"` → WASM (default entry `main.wasm` if omitted)
2. `"entry"` ends with `.wasm` → WASM
3. otherwise → Lua (default entry `main.lua`)

Launcher icons and uninstall behave like Lua packages. Max module size: **256 KiB**
(loaded into PSRAM when available).

## Lifecycle

1. User opens app → host loads bytes from LFS/SD into RAM/PSRAM
2. wasm3 parse/load + link imports under modules **`env`** and **`watch`**
3. Call export `init` (fallbacks: `_start`, `start`)
4. Each frame (~50 ms): call `tick(dt_ms)` if exported
5. Back / edge-swipe: call `on_back` / `onBack` if exported; non-zero i32 = handled
6. Unload runtime + free bytes; mic stopped

## Host imports (ABI)

Linked in both `env` and `watch`. Pointers are offsets into WASM linear memory.
Strings for `set_text` / `set_title` / `label*` are NUL-terminated.

| Import | Signature | Notes |
|--------|-----------|--------|
| `log` / `print` | `v(ii)` / `i(ii)` | ptr, len → Serial |
| `set_text` | `v(i)` | stub body text |
| `set_title` | `v(i)` | stub title |
| `label` | `i(iiiiii)` | x,y,ptr,r,g,b |
| `label3` | `i(iii)` | x,y,ptr (light gray) |
| `now` | `i()` | `millis()` |
| `battery` | `i()` | 0..100 or -1 |
| `back` | `v()` | request host back |
| `audio_ready` | `i()` | ES7210 probe |
| `mic_start` | `i(i)` | rate Hz (default 16000) |
| `mic_stop` | `i()` | |
| `mic_info` | `i(i)` | write 4×i32: rate, bits, ch, running |
| `mic_read` | `i(ii)` | ptr, max_samples → count; int16 LE |
| `mic_spectrum` | `i(ii)` | ptr to f32 bins, bins (8..128) |
| `canvas` | `i(iiii)` | x,y,w,h |
| `canvas_clear` | `i(iii)` | r,g,b |
| `canvas_scroll` | `i(i)` | dy (negative = content up) |
| `canvas_row` | `i(iii)` | y, ptr to f32 mags, n |

## Guest exports

| Export | Signature | Required |
|--------|-----------|----------|
| `init` / `_start` / `start` | `v()` | optional |
| `tick` / `on_tick` | `v(i)` dt_ms | recommended |
| `on_back` / `onBack` | `i()` | optional |

## Compiling C → wasm

```bash
# wasi-sdk clang (or any clang with wasm32 + wasm-ld)
export WASI_SDK_PATH=/path/to/wasi-sdk
./tools/build_wasm_hello.sh
```

Flags used by the sample:

```
clang --target=wasm32 -nostdlib -Os \
  -Wl,--no-entry -Wl,--export=init -Wl,--export=tick -Wl,--export=on_back \
  -Wl,--allow-undefined -Wl,--export-memory \
  -o main.wasm main.c
```

No WASI syscalls — only the host imports above. A prebuilt `main.wasm` is
vendored for `wasm_hello` so the image builds without a wasm toolchain.

## Dual-core note (v1)

WASM runs on the **app/UI core**, same as Lua. A future dual-core split
(heavy DSP / game loop on a second core) is explicitly **out of scope for v1**.

## Simulator

The Python LVGL/Lua sim does **not** execute WASM (firmware-only runtime).
Lua sim tests remain the regression gate; keep them green. Optional
wasmtime/wasm3 Python bindings can be added later without blocking firmware.

## Background jobs (non-UI core)

Lua (and later WASM) can enqueue **named** jobs that run on the **non-UI
FreeRTOS core** (ESP32-S3: Arduino/LVGL loop is core 1; worker is pinned to
**core 0**). Core IDs are **not** exposed to apps.

```lua
local id, err = watch.job_start("fft", { bins = 64 })  -- or "mic_fft", "rms"
local st = watch.job_status(id)   -- queued|running|done|error|cancelled
local res, err = watch.job_result(id)  -- table (fft) or number (rms); once
watch.job_cancel(id)
```

Queue depth is small (2). Demo: **`job_lab`**. Dual-core **app** split (WASM on
a second core) remains out of scope for v1 — only these named host jobs use the
second core.
