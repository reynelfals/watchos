# Uninstall apps (WatchOS)

Remove a LittleFS package under `/apps/<id>/` from the watch without a full
`uploadfs`. Firmware must include `appsHostRemove` + SoftAP `/uninstall`.

ASCII-only UI strings. Confirm dialogs always shown on-watch / SoftAP page.

## What gets deleted

- Entire folder `/apps/<id>/` (all files, then `rmdir`), recursively.
- Catalog is rescanned (`appsHostRescan`) so the launcher updates.
- Path safety: id must be alnum / `_` / `-` only; must resolve under `/apps/`;
  `..` and path escape are refused.
- Optional: `app.json` with `"protected": true` blocks uninstall (SoftAP + watch).
  Stock demos are **not** protected in v1.

## On-watch UX

1. Unlock to **Apps** launcher.
2. Tap **Manage** (top bar; also **Install → Manage Apps**).
3. Tap the app row (e.g. `My App`).
4. Confirm **Uninstall** (or **Cancel**).
5. List refreshes; launcher list updates when you go **Back**.

Protected rows show `[protected]` and cannot be removed.

## SoftAP / home Wi-Fi HTTP

Start **Install → Phone SoftAP** (or Home Wi-Fi). Open the page QR / URL with
session token `k=` (same as upload).

### List apps

```bash
curl "http://<IP>/apps?k=<session-token>"
```

### Uninstall

```bash
curl -X POST "http://<IP>/uninstall?app=My_app&k=<session-token>"
```

Example for Reynel's custom app id:

```bash
curl -X POST "http://192.168.4.1/uninstall?app=My_app&k=<session-token>"
```

The SoftAP HTML **Manage apps** section lists installed apps with **Uninstall**
buttons and a browser confirm. Upload (`POST /upload`) is unchanged.

## Sim / desk checks (no flash)

Path sanitizer unit test (no hardware):

```bash
python3 sim/test_uninstall.py
```

Optional Lua demo smoke (standing rule before sapphire flash):

```bash
python3 sim/run.py counter --out /tmp/counter.png
```

This agent does **not** USB-flash. Parent flashes firmware + FS on sapphire when ready.

## Notes

- SD packages uninstall via the same Manage / SoftAP flow (deletes `/sd/apps/<id>/`).
  See `docs/SD_APPS.md`.
- After SoftAP uninstall, leave Install open or return to launcher to see the
  refreshed app list.
- Full `pio run -t uploadfs` still replaces the whole LittleFS image (nuclear option).
