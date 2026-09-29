# Squish ID images (WRGB)

## Format

Custom **WRGB** (no LVGL PNG decoder required):

| Offset | Type | Meaning |
|--------|------|---------|
| 0..3 | bytes | Magic `WRGB` |
| 4..5 | uint16 LE | width |
| 6..7 | uint16 LE | height |
| 8.. | uint16 LE[] | `width * height` RGB565 pixels (little-endian) |

- Max decode size: **160 x 160** (larger files rejected).
- Typical thumbs: **128 x 128** (~32 KiB each).

## Lookup order

Slug from catalog `name`: lowercase; any char not `[a-z0-9]` becomes `_`.

1. `/sd/squish/img/<slug>.wrgb` - full library on microSD (lazy mount)
2. `/apps/squish_id/img/<slug>.wrgb` - LittleFS samples shipped with the app

`watch.squish_image_path(name)` returns the first existing path, or nil.

## Lua API

- `watch.image(path, x, y, max_w?, max_h?)` -> bool
- `watch.image_clear()`
- `watch.image_exists(path)` -> bool
- `watch.squish_image_path(name)` -> path or nil

Missing image: app shows a colored circle + initial placeholder.

## Full library on SD (Reynel decision)

To dump all catalog thumbs later:

```
/sd/squish/img/aaliyah.wrgb
/sd/squish/img/ally.wrgb
...
```

Keep LittleFS samples small; prefer SD for the full ~3756 set.
Catalog CSV stays LittleFS-first (`/apps/squish_id/catalog.csv`).

## SoftAP bulk push to microSD

Firmware exposes `POST /upload_sd?path=<rel>&k=<session>` which streams a
multipart `file` onto `/sd/<rel>` (mounts the card first).

From a PC joined to the Install SoftAP (or on the same LAN in STA mode):

```bash
# Keep Install SoftAP open on the watch; use the on-screen ?k= token.
python3 tools/push_squish_sd.py \
  --stage tools/squish_sd_stage/img \
  --host 192.168.4.1 \
  --k <session-token>
```

Writes `/sd/squish/img/<slug>.wrgb`. Does **not** remove LittleFS samples under
`/apps/squish_id/img/`. Prep SD (in-app copy of LFS → SD) remains for the small
bundled set only — the full catalog needs this OTA path or a PC-mounted card.
