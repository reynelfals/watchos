# SD-card Lua apps (WatchOS)

Copy a Lua app folder onto the microSD and the launcher lists it alongside
LittleFS packages under `/apps/`. Same `app.json` + entry script layout.

## Folder layout on the card

At the **card root** (FAT):

```
apps/
  sd_hello/
    app.json
    main.lua
  my_game/
    app.json
    main.lua
    ...
```

On-watch paths:

| Card path | Watch path |
|-----------|------------|
| `apps/<id>/app.json` | `/sd/apps/<id>/app.json` |
| `apps/<id>/main.lua` | `/sd/apps/<id>/main.lua` |

`app.json` fields (same as LittleFS):

```json
{"id":"sd_hello","name":"SD Hello","version":"0.1.0","entry":"main.lua"}
```

Optional `"protected": true` blocks Manage / SoftAP uninstall.

## How Reynel copies an app onto SD

Any of:

1. **USB card reader** — eject the microSD, copy `apps/<id>/` on a PC, reinsert.
2. **USB SD (MSC)** — flash optional `-msc` firmware, Install → **USB SD**, copy
   `apps/<id>/` onto the drive, eject + Exit. See `docs/USB_SD.md`.
3. **SoftAP `upload_sd`** — Phone SoftAP / Home Wi‑Fi, then upload files to
   `apps/<id>/...` (relative under `/sd/`):

```bash
# Create files one at a time (same session token k= as the SoftAP page)
curl -F file=@app.json \
  "http://<IP>/upload_sd?path=apps/sd_hello/app.json&k=<session-token>"
curl -F file=@main.lua \
  "http://<IP>/upload_sd?path=apps/sd_hello/main.lua&k=<session-token>"
```

Demo package in-repo: `tools/sd_apps_stage/sd_hello/`.

## SoftAP phone upload vs SD

| Path | Destination |
|------|-------------|
| SoftAP **Upload** (`POST /upload?app=`) | **LittleFS** `/apps/<id>/` |
| SoftAP **upload_sd** (`POST /upload_sd?path=`) | **microSD** `/sd/<path>` |
| `pio run -t uploadfs` | **LittleFS** image from `data/` |

Phone SoftAP Install UI still targets LittleFS for app packages. Use
`upload_sd` (or USB / card reader) for SD apps.

## LittleFS vs SD / id clash

- Catalog merges LittleFS `/apps/*/app.json` then SD `/sd/apps/*/app.json`.
- **LittleFS wins on id clash** — if both have the same `id`, only the flash
  copy appears in the launcher.
- Launcher rows from SD show a `[SD]` tag.
- Firmware lazy-mounts the card when scanning (no need to open SD Lab first).

## Launch

Tapping an app opens `folder/entry` via `luaHostOpen`. Paths under `/sd/...`
are read from the card; LittleFS paths from flash.

## Uninstall

**Manage Apps** (or SoftAP `POST /uninstall?app=<id>`) deletes:

- LittleFS apps → `/apps/<id>/`
- SD apps → `/sd/apps/<id>/` (recursive)

Confirm dialog shows the path that will be deleted. MSC lock blocks SD deletes
until you Exit USB SD.

## Sim check (no flash)

```bash
python3 sim/test_sd_apps.py
python3 sim/test_uninstall.py
```

Standing rule: run sim checks before sapphire flash.

## Build

```bash
pio run -e waveshare-amoled-206
```

Default env keeps `USB_MODE=1` (ACM0 flashable). Do not switch TinyUSB default.
