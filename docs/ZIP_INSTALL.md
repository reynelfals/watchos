# Zip app install (WatchOS)

Unpack a small app zip into `/apps/<id>/` (LittleFS) or `/sd/apps/<id>/`.

## Caps

- Max uncompressed total: **512 KiB**
- Max single file: **256 KiB**
- Methods: **store (0)** and **deflate (8)** via ROM tinfl
- Paths sanitized (no `..`, no absolute names)

## SoftAP (phone / curl)

1. Launcher → **Install** → start SoftAP (or Home Wi‑Fi).
2. Note IP + session token `k=` from the watch / QR page.
3. Upload:

```bash
# LittleFS
curl -F file=@my_app.zip \
  "http://<IP>/upload_zip?app=my_app&dest=lfs&k=<session-token>"

# microSD
curl -F file=@my_app.zip \
  "http://<IP>/upload_zip?app=my_app&dest=sd&k=<session-token>"
```

Zip should contain `app.json` + `main.lua` (and assets) at the archive root
(or under a single folder — currently paths are written as stored in the zip;
prefer files at zip root: `app.json`, `main.lua`, and optional `icon.wrgb` — see [APP_ICONS.md](APP_ICONS.md)).

## Lua host path

If the zip is already on-device:

```lua
local ok, err = watch.install_zip("/sd/pkg/my_app.zip", "my_app", "lfs")
```

## EMF / Wi‑Fi policy

Radio stays **off** at boot. SoftAP / STA only after explicit Install action.
`watch.wifi_connect` is also explicit (never auto).
