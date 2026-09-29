#!/usr/bin/env python3
"""Sim unit tests for expanded watch.* APIs (no hardware)."""

from __future__ import annotations

import os
import sys
import zipfile
import io
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from watch_host import WatchHost  # noqa: E402


def ok(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main() -> int:
    host = WatchHost(os.path.join(os.path.dirname(HERE), "data", "apps"))

    # Prefs
    ok(host.prefs_set("lab_msg", "hello") is True, "prefs_set")
    ok(host.prefs_get("lab_msg") == "hello", "prefs_get")
    ok(host.prefs_get("missing", "d") == "d", "prefs default")

    # Layout
    ok(host.panel(10, 10, 100, 40) is True, "panel")
    ok(host.label(12, 12, "Hi") is True, "label")
    pid = host.progress(10, 60, 200, 12, 40)
    ok(pid == 0, "progress id")
    ok(host.progress_set(pid, 80) is True, "progress_set")
    sid = host.slider(10, 80, 200, 20, 0, 100, 50)
    ok(sid == 0, "slider")

    # Time / power
    n = host.now()
    ok("epoch" in n and "hour" in n, "now")
    ok(host.set_alarm() == (False, "planned"), "set_alarm stub")
    ok(host.brightness() == 75, "bri get")
    ok(host.brightness(40) == 40, "bri set")
    ok(host.idle_timeout(120) == 120, "idle set")
    ok(host.idle_timeout() == 120, "idle get")

    # Input
    host.sim_swipe("swipe_left", -120, 0)
    g = host.gesture()
    ok(g and g["type"] == "swipe_left", "gesture")
    fired = {"n": 0}
    host.on_long_press(lambda x, y: fired.__setitem__("n", fired["n"] + 1))
    host.sim_long_press(1, 2)
    ok(fired["n"] == 1, "long press")

    # Net (wifi off by default)
    ok(host.wifi_state() == "off", "wifi off")
    body, err = host.http_get("http://example.com")
    ok(body is None and err == "wifi_off", "http requires wifi")
    ok(host.wifi_connect("Test")[0] is True, "wifi connect")
    ok(host.wifi_state() == "on", "wifi on")
    body, code = host.http_get("http://example.com", 100)
    ok(code == 200 and body.startswith("sim-http-ok"), "http_get")
    ok(host.wifi_disconnect() is True, "wifi disconnect")

    # Audio / mic spectrum + fine PCM (sim always ready)
    ok(host.audio_ready() is True, "audio")
    info0 = host.mic_info()
    ok(isinstance(info0, dict) and info0.get("running") is False, "mic_info stopped")
    ok(info0.get("bits") == 16 and info0.get("channels") == 1, "mic_info format")
    pcm_err = host.mic_read(64)
    ok(pcm_err[0] is None and pcm_err[1] == "mic_not_running", "mic_read not started")
    tab_err = host.mic_read_table(64)
    ok(tab_err[0] is None and tab_err[1] == "mic_not_running", "mic_read_table not started")
    ok(host.mic_start({"rate": 16000}) is True, "mic_start")
    info1 = host.mic_info()
    ok(info1.get("running") is True and info1.get("sample_rate") == 16000, "mic_info running")
    pcm, n = host.mic_read(128)
    ok(isinstance(pcm, (bytes, bytearray)) and n == 128 and len(pcm) == 256, "mic_read bytes")
    tab, n2 = host.mic_read_table(64)
    ok(isinstance(tab, list) and n2 == 64 and len(tab) == 64, "mic_read_table")
    ok(all(isinstance(v, int) and -32768 <= v <= 32767 for v in tab), "mic_read_table range")
    # Cap at 1024
    pcm2, n3 = host.mic_read(9999)
    ok(n3 == 1024 and len(pcm2) == 2048, "mic_read cap 1024")
    # RMS sanity (noise + tone should be non-trivial)
    import struct as _st
    samples = _st.unpack("<" + "h" * n, pcm)
    mean_sq = sum(s * s for s in samples) / float(n)
    ok(mean_sq > 100.0, "mic_read rms energy")
    spec = host.mic_spectrum(64)
    ok(isinstance(spec, list) and len(spec) == 64, "mic_spectrum len")
    ok(all(0.0 <= v <= 1.0 for v in spec), "mic_spectrum range")
    ok(host.mic_stop() is True, "mic_stop")
    ok(host.mic_record()[1] == "use_mic_spectrum_or_mic_read", "mic_record redirect")
    ok(host.speaker_ready() is True, "speaker_ready")
    ok(host.sd_mount() is True, "sd for audio")
    ok(host.mic_record_file("/sd/recordings/t.wav", 1, 16000) is True, "mic_record_file")
    ok("/sd/recordings/t.wav" in host._sd, "wav stored")
    wav = host._sd["/sd/recordings/t.wav"]
    ok(isinstance(wav, (bytes, bytearray)) and wav[:4] == b"RIFF", "wav riff")
    ok(host.speaker_play("/sd/recordings/t.wav") is True, "speaker_play")
    ok(host.speaker_start({"rate": 16000}) is True, "speaker_start")
    ok(host.speaker_write(b"\x00\x00\x00\x00" * 8) is True, "speaker_write")
    ok(host.speaker_stop() is True, "speaker_stop")
    # Speaker volume 0..100% <-> raw 0x00..0xBF (session)
    ok(host.speaker_volume() == 100, "speaker_volume default 100")
    ok(host.speaker_volume_raw() == 0xBF, "speaker_volume_raw default 0xBF")
    ok(host.speaker_volume(50) is True, "speaker_volume set 50")
    ok(host.speaker_volume() == 50, "speaker_volume get 50")
    ok(host.speaker_volume_raw() == host._vol_pct_to_raw(50), "speaker_volume raw maps")
    ok(host.speaker_volume(0) is True, "speaker_volume mute")
    ok(host.speaker_volume() == 0 and host.speaker_volume_raw() == 0, "speaker_volume 0")
    ok(host.speaker_volume(100) is True, "speaker_volume restore")
    ok(host.speaker_volume_raw(0x60) is True, "speaker_volume_raw set")
    ok(0 <= host.speaker_volume() <= 100, "speaker_volume from raw in range")
    ok(host.speaker_volume_raw(0xFF) is True, "speaker_volume_raw clamp")
    ok(host.speaker_volume_raw() == 0xBF, "speaker_volume_raw max 0xBF")
    ok(host.speaker_volume(100) is True, "speaker_volume back to 100")
    ok(host.canvas(10, 10, 32, 16) is True, "canvas")
    ok(host.canvas_clear(0, 0, 0) is True, "canvas_clear")
    ok(host.canvas_row(15, [0.0, 0.5, 1.0]) is True, "canvas_row")
    ok(host.canvas_scroll(-1) is True, "canvas_scroll")
    ok(host.ble_ready() is True, "ble_ready")
    ok(host.ble_status() == "idle", "ble idle")
    hits = host.ble_scan(100)
    ok(isinstance(hits, list) and len(hits) >= 1, "ble_scan")
    ok(host.ble_connect(hits[0]["addr"]) is True, "ble_connect NO_PIN")
    ok(host.mesh_connected() is True, "mesh_connected")
    ok(host.mesh_channel() == 1, "mesh_channel default 1")
    ok(host.mesh_channel(0) == 0, "mesh_channel set 0")
    ok(host.mesh_channel(1) == 1, "mesh_channel set 1")
    ok(host.mesh_alert_beep() is True, "mesh_alert_beep default")
    ok(host.mesh_alert_vibrate() is True, "mesh_alert_vibrate default")
    ok(host.mesh_alert_beep(False) is False, "mesh_alert_beep off")
    ok(host.mesh_alert_beep(True) is True, "mesh_alert_beep on")
    ok(host.mesh_send("hi") is True, "mesh_send")
    polled = host.mesh_poll()
    ok(any(m.get("text") == "hi" for m in polled), "mesh_poll loopback")
    ok(host.mesh_inject_peer("ping-bg") is True, "mesh_inject_peer")
    ok(host.notif_app_id() == "mesh_chat", "notif app_id from mesh")
    ok(host._alert_uh_oh_count >= 1, "mesh inject plays uh-oh")
    ok(host.sim_notif_swipe_down() == "mesh_chat", "notif swipe-down opens mesh_chat")
    ok(host.notif_app_id() is None, "notif cleared after swipe")
    # re-notify to test launch intent
    host.notify("mesh_chat", "Yo reply me")
    host.sim_notif_swipe_down()
    ok(host.launch_action() == "reply", "launch_action reply")
    ok(host.launch_text() == "Yo reply me", "launch_text body")
    ok(host.launch_action() is None, "launch_action one-shot")
    ok(host.notify("sound_deck", "rec done") is True, "notify generic")
    ok(host.notif_app_id() == "sound_deck", "notif generic app_id")
    ok(host.sim_notif_swipe_down() == "sound_deck", "notif swipe opens sound_deck")
    ok(host.beep(80, 880) is True, "beep")
    ok(host.alert_uh_oh() is True, "alert_uh_oh")
    ok(host.ble_disconnect() is True, "ble_disconnect")
    ok(host.speech_to_text(2) == "on my way", "speech_to_text sim")

    # SD remove/rename
    ok(host.sd_mount() is True, "sd mount")
    ok(host.sd_write("/sd/a.txt", "x") is True, "sd write")
    ok(host.sd_rename("/sd/a.txt", "/sd/b.txt") is True, "sd rename")
    ok(host.sd_remove("/sd/b.txt") is True, "sd remove")

    # GPIO whitelist
    ok(host.gpio_write(10, 1) is True, "gpio ok")
    ok(host.gpio_write(18, 1) is False, "gpio blocked motor pin")
    ok(host.gpio_pwm(99, 10)[0] is False, "gpio bad pin")

    # Zip install marker
    ok(host.install_zip("/sd/x.zip", "demo", "lfs")[0] is True, "install_zip")

    # Sprite
    ok(host.sprite("/apps/x.wrgb", 0, 0, 32, 32, 0) is True, "sprite")

    print("PASS sim/test_watch_api.py")

    # Background jobs (sim completes over polls)
    jid, err = host.job_start("fft", {"bins": 16})
    ok(jid is not None and err is None, f"job_start fft {err}")
    st = host.job_status(jid)
    ok(st in ("queued", "running", "done"), f"job_status {st}")
    # advance
    for _ in range(5):
        host._job_advance()
    res, err = host.job_result(jid)
    ok(err is None and isinstance(res, list) and len(res) == 16, f"job_result fft {err} {res}")
    jid2, _ = host.job_start("rms")
    for _ in range(5):
        host._job_advance()
    rms, err = host.job_result(jid2)
    ok(err is None and isinstance(rms, float), f"job_result rms {err}")
    jid3, _ = host.job_start("fft", {"bins": 8})
    ok(host.job_cancel(jid3) is True, "job_cancel")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
