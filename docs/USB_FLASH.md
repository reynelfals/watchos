# USB flash vs USB SD (MSC)

## The tradeoff

ESP32-S3 has one USB peripheral that can run in two modes:

| `ARDUINO_USB_MODE` | Stack | `/dev/ttyACM0` for esptool | USB SD (MSC) |
|---|---|---|---|
| **1** (default) | HW CDC/JTAG | Reliable ROM download | **Not available** |
| **0** (`-msc` env) | TinyUSB OTG | App CDC — often **cannot** enter ROM download | **Works** |

Default env `waveshare-amoled-206` uses **MODE=1** so `pio run -t upload` /
`uploadfs` work over ACM0 again. USB mass-storage of the microSD card is only
in optional env `waveshare-amoled-206-msc` (MODE=0).

Keep `ARDUINO_USB_CDC_ON_BOOT=1` in both builds.

## How we got stuck

Enabling TinyUSB for USB SD (`-DARDUINO_USB_MODE=0`) made Serial an **app** CDC
interface. esptool expects the ROM USB-Serial/JTAG path to reset into download
mode. After the MSC firmware was flashed, ACM0 no longer entered download mode
reliably, so normal PlatformIO uploads failed.

## Recovery (once)

Flash the **default** (`USB_MODE=1`) firmware once so the watch is back on HW
CDC/JTAG:

1. **Preferred:** hold **BOOT**, cold-plug USB-C, then:
   ```bash
   pio run -e waveshare-amoled-206 -t upload
   ```
   (add `-t uploadfs` if you also need LittleFS)
2. **Or** use a USB–UART bridge on the board UART pins if ACM0 still will not
   enter download mode.
3. After that, day-to-day `pio run -t upload` / `uploadfs` on ACM0 should work.
4. **Lua apps** do not need this — SoftAP / Install HTTP still updates apps on
   LittleFS without a USB flash.

Do **not** flash the `-msc` build unless you intentionally need USB SD and
accept awkward re-flashing (UART or BOOT cold-plug).

## USB SD only on `-msc`

```bash
pio run -e waveshare-amoled-206-msc
# upload that firmware via BOOT cold-plug or UART when needed
```

On the default build, **Install → USB SD** shows that USB SD needs the `-msc`
firmware. Details: [USB_SD.md](USB_SD.md).
