# BF Lab — on-watch Brainfuck

App id: **`brainfuck`** (launcher name **BF Lab**).  
Version **1.2.0** — create, store, load, and run Brainfuck **on the watch**, with GPIO + pixel I/O.

## UI modes

| Mode | What |
|------|------|
| **Home** | Edit / Files / Run / Map / Ex / Exit |
| **Edit** | Compact keypad: `>` `<` `+` `-` `.` `,` `[` `]` plus **BS** / **CLR**. Program text + cursor (`\|`). System Back → Home. |
| **Files** | List `/sd/bf/*.bf`. **New** / **Save** / **Load** / **Del** / Prev / Next. Auto names `prog001.bf`, … |
| **Run** | **Go** / **Stop** / **Step** / **Reset**. Tape window; live **64×64** draw canvas (16×16 FB ×4); `.` output; `,` input (`In+A` / `InClr`). |
| **Map** | GPIO + pixel register map; Pulse0/Low0; ClrDraw. |
| **Ex** | Examples: ASCII `A`, GPIO60 poke, **Plot**, **FB chk**, loop. Loads program → **Home**. |

No QWERTY — BF only needs eight ops. Edit uses `watch.button_layout("grid2", 4)`.

## SD layout

```
/sd/bf/
  prog001.bf
  …
```

- Mount via `watch.sd_mount()`; `watch.sd_mkdir("/sd/bf")` on first Files/Save.
- Plain-text `.bf` (non-ops stripped on load). Max editor length: **192** ops.

## Runtime

- Tape **64** cells (0–255 wrap), pointer wraps.
- Filtered source; bracket jump table on Reset/Go.
- Up to **32** steps per `on_tick` burst.

### GPIO-mapped tape (whitelist only)

Same safe pins as `watch.gpio_*` / gpio_lab: **10, 16, 19, 20**.

| Tape | GPIO | Write | Read |
|------|------|-------|------|
| `60` | **10** | `gpio_write` (0/1); `gpio_pwm` if value &gt; 1 | `gpio_read` → 0/1 |
| `61` | **16** | same | same |
| `62` | **19** | same | same |
| `63` | **20** | same | same |

### Pixel I/O tape (pure BF — no new symbols)

Plot registers + small framebuffer bank. Classic programs using cells 0–47 are unaffected. GPIO **60–63** unchanged.

| Cell | Role |
|------|------|
| `48` | **Mode**: 0 = normal; 1 = FB address mode (X/Y are FB coords 0..15) |
| `49` | **FB cursor** (linear index 0..255); auto-updated on poke |
| `50` | **X** — canvas 0..63 (plot) or FB 0..15 (mode=1) |
| `51` | **Y** — same |
| `52` | **R** 0..255 |
| `53` | **G** |
| `54` | **B** |
| `55` | **Plot strobe** — any write ≠0 plots brush at (X,Y) with RGB on the draw canvas; auto-clears to 0 |
| `56` | Brush **size** 1..8 (default 1) |
| `57` | **Clear** strobe — write ≠0 → `canvas_clear` draw layer (UI chrome kept); auto-clears |
| `58` | **FB poke** — writes RGB into 16×16 FB at (X,Y) if mode=1, else at cursor; paints a 4×4 block; auto-clears |

**Framebuffer:** 16×16 RGB, displayed as a **64×64** `watch.canvas` (scale 4) mid-screen on **Run**.  
APIs: `watch.canvas`, `watch.canvas_clear`, `watch.canvas_pixel` (preferred). Fallback: `watch.ball` if pixel poke missing. **No per-cell LVGL rects** (Portal OOM lesson).

Tip: `-` on a zero cell wraps to **255** — short way to set bright RGB.

### I/O

- `.` appends printable ASCII (else `\HH`).
- `,` consumes Run-screen input (default 0). **In+A** queues `A` (65).

## Host / firmware

- **Lua-only app** works in sim with `canvas_pixel`.
- Device needs firmware that exposes `watch.canvas_pixel(x,y,r,g,b)` (additive; same canvas as waterfall). Without it, plot/FB fall back to a single `watch.ball`.
- Prefer **no flash** for app-only SD sync; flash only when picking up the new host symbol.

## Sim

```bash
python3 sim/run.py brainfuck --out /tmp/bf_home.png
python3 sim/run.py brainfuck --click Edit --out /tmp/bf_edit.png
python3 sim/run.py brainfuck --click Map --out /tmp/bf_map.png
python3 sim/run.py brainfuck --click Ex --click Plot --click Run --click Go --ticks 120 --out /tmp/bf_plot.png
python3 sim/run.py brainfuck --click Ex --click "FB chk" --click Run --click Go --ticks 200 --out /tmp/bf_fb.png
```

## Icon

`data/apps/brainfuck/icon.wrgb` via `tools/gen_app_icons.py`.
