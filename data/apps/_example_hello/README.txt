Example packaged app folder for Watch OS LittleFS.

Layout:
  /apps/<folder>/app.json   — required metadata (id, name, version, entry)
  /apps/<folder>/main.lua   — entry script (live Lua 5.4 session while open)

Tapping this app opens a live session until Back. Script can call:
  watch.set_title / set_text / append_text / clear / print
  watch.width / height / millis / time / delay (capped 500ms)
  watch.button(label, fn)   — max 10; extra buttons soft-fail with a message
  watch.button_layout("column"|"grid2"[, cols])  — keep mode across clear_ui
  watch.touch()            — nil or {x,y,pressed=true} while finger down
  watch.on_tick(fn)         — ~200ms poll from firmware loop
  watch.fill / rect / circle / text_at
  watch.battery() / back() / vibrate()

See project README.md for the full API. Also try data/apps/counter/.
