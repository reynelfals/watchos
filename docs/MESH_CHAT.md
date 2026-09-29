# Mesh Chat (Meshtastic BLE)

Version **1.1.0** — Watch OS Lua app + NimBLE **central** host talking to a
Meshtastic radio (e.g. SenseCAP T1000-E) over the PhoneAPI GATT service.

Mesh Chat is **offline**: it uses the watch's BLE connection to the Meshtastic
radio and does not require Wi-Fi.

## What it does

- Scan for advertisers exposing the Meshtastic service UUID
- Connect with **NO_PIN** (open) or a **4–6 digit PIN** (bonding passkey)
- Handshake: write `ToRadio.want_config_id` (69420), drain `FromRadio` until empty
- Subscribe `FromNum` notify → drain `FromRadio` on tick
- Send / receive **TEXT_MESSAGE_APP** (portnum 1) UTF-8 on **channel index 1 by default** (Family / secondary). Toggle Ch0/Ch1 in-app.
- **Background alerts** while Mesh Chat is closed (BLE still connected): short **speaker beep** + optional vibrate + top banner. Beep is the primary alert; vibrate needs a coin motor soldered to the empty pads.

## Fast text input (AMOLED)

Typing full sentences on 410×502 is slow. Mesh Chat therefore ships:

1. **Phrase chips** — OK / Yes / No / Help / On my way / Location later / Ping (tap = send now)
2. **Type** — host `letter_pad` (A–Z) + **Spc** for short custom lines, then Send
3. **Send** — send the current keyboard draft

## Lua API

| API | Notes |
|-----|--------|
| `watch.ble_ready()` | NimBLE stack up |
| `watch.ble_status()` | `idle` / `scanning` / `connecting` / `connected` / error |
| `watch.ble_scan(timeout_ms?)` | list of `{addr,name,rssi}` |
| `watch.ble_connect(addr, pin?)` | omit/`nil` pin = NO_PIN |
| `watch.ble_disconnect()` | |
| `watch.mesh_connected()` | bool |
| `watch.mesh_send(text)` | ok or false, err — uses current channel |
| `watch.mesh_poll()` | list of `{from,text,time}` (drains host queue) |
| `watch.mesh_channel()` / `(n)` | get/set channel index 0..7 (default **1**, NVS) |
| `watch.mesh_alert_beep()` / `(bool)` | get/set inbound beep (default true, NVS) |
| `watch.mesh_alert_vibrate()` / `(bool)` | get/set inbound vibrate (default true; no-op without motor) |

Legacy: `ble_start` starts a short scan; `ble_stop` disconnects.

## Sim

```bash
python3 sim/run.py mesh_chat --click Scan --click Connect --click OK --ticks 3
python3 sim/run.py mesh_chat --click Scan --click Connect --click Type --click Send --ticks 2
python3 sim/test_watch_api.py
```

Loopback: `mesh_send` echoes locally and adds a fake `ack:` peer reply.

## Gaps (v1)

- Channel UI is Ch0/Ch1 only (not a full NodeDB / PSK picker); Family encryption stays on the radio
- Background alerts require an active BLE link to the T1000 (Exit keeps link; Disc tears it down)
- Beep uses ES8311 speaker path; vibrate is wired but silent until a coin motor is soldered
- No PKI DMs, position, or admin
- Hand-rolled protobuf for text + want_config only
- `ble_scan` / connect briefly block the Lua thread on device
- PIN uses NimBLE passkey injection; OS-level bonding UX varies by peer firmware

## Background alerts

When Mesh Chat **Exit**s (or soft-lock returns to clock) while still connected to
the radio, inbound TEXT still drains in `bleMeshTick` on the main loop. The host:

1. Plays an **ICQ-style "uh-oh"** on the ES8311 speaker if `mesh_alert_beep` is on
   (default): 660→990 Hz ascending, then 520 Hz descending, at full volume.
2. Pulses the GPIO vibrate line if `mesh_alert_vibrate` is on (default; no effect
   until a DIANN/coin motor is soldered to the empty pads).
3. Shows a ~5s top banner: `Mesh Chat: <snippet>`, associated with app id
   `mesh_chat`.

**Swipe down** on the banner opens Mesh Chat (generic: any app that raised a
notification via `watch.notify(app_id, text)` opens that app on swipe-down).

Toggle Beep/Vib on the Mesh Chat home screen. Disc still disconnects and stops alerts.

## Flash

Firmware + LittleFS (channel encode, alert path, Lua app):

```bash
cd <your-watchos-checkout>   # or this tree
pio run -t upload -t uploadfs --upload-port /dev/ttyACM0
```
