# App icons (WatchOS launcher)

## Format

- **WRGB only** (little-endian RGB565 pixel payload).
- Magic: ASCII `WRGB`, then `uint16_t` width, `uint16_t` height (LE), then `w*h` RGB565 pixels.
- Preferred size: **72×72**. Loader accepts up to **96×96**.
- Missing/invalid icons → colored letter tile (first letter of the app name).

## Packaging

In each app folder (LittleFS `/apps/<id>/` or SD `/sd/apps/<id>/`):

| File | Role |
|------|------|
| `app.json` | Manifest; optional `"icon":"icon.wrgb"` |
| `icon.wrgb` | Default icon filename when present |
| `main.lua` | Entry (or whatever `entry` says) |

If `app.json` omits `icon`, the host still uses `icon.wrgb` when that file exists.

### Zip install (SoftAP / install zip)

Put assets at the **archive root** next to `app.json`:

```
my_app.zip
├── app.json
├── main.lua
└── icon.wrgb
```

SoftAP zip install already preserves non-script assets; include `icon.wrgb` in the zip so the launcher shows the icon after install.

## Launcher UI

- **Grid** (default): 3 columns, icon + truncated name; scrollable; respects `LCD_SAFE_INSET_PX` (24).
- **List**: icon + name + version (`[SD]` when on SD).
- Toggle: launcher header **Set** → Settings → Grid / List.
- Persisted in NVS: namespace `watchos`, key `launcher_view`, values `grid` | `list`.

## Generating icons

```bash
# Letter-tile WRGB for every data/apps/*/app.json
python3 tools/gen_app_icons.py

# Or from a PNG (optional size; use 72 for launcher):
python3 tools/png_to_wrgb.py input.png data/apps/my_app/icon.wrgb 72
```

## Simulator

Native LVGL launcher/icons are firmware-only. `sim/test_watch_api.py` does not render the launcher grid; keep host stubs green. No new Lua APIs for this feature.
