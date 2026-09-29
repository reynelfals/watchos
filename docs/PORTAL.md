# Portal (starship window)

The watch screen is a **physical window** into a **fixed** virtual room. The world does not move — only the window’s aim does, driven by IMU orientation (yaw / pitch / roll).

## Concept

| Watch attitude | What you see |
|----------------|--------------|
| Tip top away / face-up | Ceiling (bay lights) — **pitch** up |
| Vertical | Horizon / walls |
| Turn / pan (gyro) | Look around the bay — **yaw** |
| Twist / tip left–right | Horizon banks — **roll** |
| Flip / face-down | Floor (metal grate) — **pitch** down |

One room: a starship bay with hull walls, pillars/crates, grated floor, and lit ceiling panels.

## Controls (v1.1)

- **Look** — short hold-still calibration (gyro bias + roll/yaw zero; **pitch stays absolute**: tip-up/face-up→ceiling), then IMU aims the view
- **Yaw (pan)** — turn the watch; host gyro rates (`gx,gy,gz` dps) integrate as ω·ĝ (rotation about gravity). Accel-only fallback: when mostly flat, heading `atan(ay, ax)` pans carefully (no tip-L/R→yaw fight with roll when upright)
- **Pitch** — tip up/down from accel gravity (`asin(az)`), same tip-up=look-up feel as 1.0.3
- **Roll** — twist/bank from accel (`atan(ay, az)`); wall column strips shear so the horizon tilts
- **Back** — return to menu; back again exits the app
- No walk / no collision move — you stand in the bay center

Status line (capturable): `yaw … pitch … roll … [gyro|accel]`.

Firmware soft-lock does **not** engage while Portal (or any Lua/WASM app) is open on `scrStub` — Look mode can run without touch for >60s.

## Technique

Column **raycaster** (16 wall strips + shared ceil/floor + `gfx_clear` each frame) with horizon **Y-shear** for pitch and **per-column shear** for roll. Complementary-lite filter: accel anchors pitch/roll; gyro integrates yaw when `watch.imu` exposes rates.

IMU table from host / sim (QMI8658 on device):

| Field | Unit | Role |
|-------|------|------|
| `ax,ay,az` | g | Gravity → pitch, roll; flat heading fallback |
| `gx,gy,gz` | dps | Body rates → yaw/pan (and live gyro detect) |

## Sim

```bash
cd /workspace/watchos
python3 sim/run.py portal
python3 sim/run.py portal --click Look --tilt 0,0,1 --ticks 20    # face-up → ceiling
python3 sim/run.py portal --click Look --tilt 0,0,-1 --ticks 20   # face-down → floor
python3 sim/run.py portal --click Look --tilt 0.05,0.55,0.85 --ticks 20  # tip → roll
python3 sim/run.py portal --click Look --tilt 0,0,1 --gyro 0,0,90 --ticks 30  # pan via gz
python3 sim/run.py portal --shot   # Look + pitch + roll + gyro yaw proof
```

Tilt pad (interactive host): drag to change `ax`/`ay`; `az` follows. `--gyro gx,gy,gz` sets rates (dps). Autoplay also nudges `gz` for pan demos.

## v1.1 limits

- Single room, fixed spawn — no walking
- 16 wall strips + shared ceil/floor + gfx_clear (rect=new LVGL obj; was OOM/StoreProhibited)
- Roll shears strips only (shared ceil/floor stay axis-aligned)
- No sprites / pickups / combat
- Calib is hold-still bias (gyro + roll/yaw); pitch remains absolute

## Later (out of scope)

- Voice **“shoot”** / **“pick”** (and other verbs) aimed through the window
- Richer art (textures, props), multi-room, audio stingers
