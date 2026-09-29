-- WatchOS SD Hello — proves launcher load from /sd/apps/sd_hello/
-- Copy this folder to the microSD as apps/sd_hello/ (see docs/SD_APPS.md).

watch.set_title("SD Hello")
watch.set_text("Loaded from microSD\n/sd/apps/sd_hello/\n\nTap OK.")

watch.circle(360, 48, 10, 80, 200, 120)

watch.button("OK", function()
  watch.set_text("SD apps work.\nCopy more under\napps/<id>/ on the card.")
end)

print("sd_hello ready")
