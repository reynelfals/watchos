# USB SD (MSC) — expose microSD as a USB drive

WatchOS can present the microSD card to a PC as a **USB Mass Storage** drive
so you can copy Squish WRGB images (and other files) without SoftAP upload.

## Which firmware?

USB SD requires the optional **TinyUSB** build:

```bash
pio run -e waveshare-amoled-206-msc
```

The **default** env (`waveshare-amoled-206`) uses HW CDC/JTAG (`USB_MODE=1`) so
esptool works on `/dev/ttyACM0`. That build **cannot** expose MSC — Install →
**USB SD** will say you need the `-msc` firmware.

See [USB_FLASH.md](USB_FLASH.md) for the MODE=0 vs MODE=1 tradeoff and how to
recover ACM0 flashing.

## Requirements (`-msc` build only)

- Firmware from **`waveshare-amoled-206-msc`** (`ARDUINO_USB_MODE=0`, CDC-on-boot).
- microSD inserted and formatted (FAT is typical for PC mounts).
- USB-C cable to the PC.

## Reynel taps (device)

1. Unlock → **Launcher** → **Install**.
2. Tap **USB SD**.
3. Status should read: *USB SD active — PC should see drive. Tap Exit before unplug.*
4. On the PC, wait for the removable drive to appear (FAT volume).
5. Create folders if needed: `squish/img/` at the card root  
   (on-watch path is `/sd/squish/img/<slug>.wrgb`).
6. Copy `*.wrgb` files into `squish/img/`.
7. **Safely eject** the drive on the PC.
8. On the watch tap **Exit USB SD** (same button), then unplug.
9. Optional: open **SD Lab** or Squish ID to confirm files under `/sd/...`.

Alternate: Launcher → **USB SD** lab app (Enter MSC / Exit MSC) — same stack.

## Serial / flash notes

- On `-msc` firmware, CDC is TinyUSB — **exit MSC** before any serial work.
- Re-flashing `-msc` (or switching back to default) may need **BOOT cold-plug**
  or UART; do not rely on ACM0 auto-reset. See [USB_FLASH.md](USB_FLASH.md).
- SoftAP Install still updates Lua apps without USB flash.

## Design

- `USBMSC` + SD_MMC `readRAW` / `writeRAW` (512-byte sectors).
- FatFS file APIs are **locked** while MSC owns the card; Exit remounts `/sd`
  so Lua sees PC writes.
- SoftAP/STA is stopped when entering MSC.
- Composite CDC+MSC is TinyUSB only (`USB_MODE=0`).

## Do not

- Unplug without PC eject + watch Exit (risk of FS corruption).
- Wipe LittleFS / run `rsync --delete` / overwrite `app_secrets.h`.
