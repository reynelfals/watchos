# WatchOS versioning

Local git lives on sapphire at `<your-watchos-checkout>`.
Goal: roll back firmware + LittleFS Lua apps when a flash leaves the watch on mixed revisions.

## Tag naming

| Kind | Pattern | Example |
|------|---------|---------|
| Firmware snapshot | `fw/YYYY-MM-DD-short` | `fw/2026-09-17-usb-msc` |
| Lua app release | `app/<id>-<semver>` | `app/tilt_maze-1.1.1` |
| Last known good (movable) | `lkg` | points at last verified pair |

After a verified flash pair (firmware **and** LittleFS both OK on device), run:

```bash
./tools/mark_lkg.sh "why this is good"
```

## Do not leave mixed revisions

`pio run -t upload` alone can leave old Lua on a new firmware (or the reverse).
Always use `./tools/flash_pair.sh` when both sides change. That script refuses to
report success unless `uploadfs` exits 0.
