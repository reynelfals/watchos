# WatchOS rollback

## A) Roll back a Lua app (phone SoftAP — preferred for apps)

```bash
cd <your-watchos-checkout>
./tools/stage_ota_from_git.sh app/tilt_maze-1.1.0 tilt_maze
# or last known good tree for one app:
./tools/stage_ota_from_git.sh lkg tilt_maze
```

Then on the watch: Install → Phone SoftAP → upload from `~/watchos-ota/<app>/`.

## B) Roll back firmware + filesystem (USB on sapphire)

```bash
cd <your-watchos-checkout>
git checkout lkg          # or a fw/... tag
./tools/flash_pair.sh     # upload + uploadfs; aborts if FS fails
git checkout -            # return to previous branch/commit when done
```

If `uploadfs` fails (e.g. TinyUSB CDC): hold BOOT, retry once, or SoftAP the
needed apps from the same tag so firmware and apps match.

## C) What “mixed revision” looks like

- USB MSC firmware flashed OK, `uploadfs` failed → new firmware, old tilt physics.
- Fix: SoftAP apps from the matching tag, or `flash_pair.sh` after BOOT.
