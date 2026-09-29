#!/usr/bin/env python3
"""Run a WatchOS Lua demo in the desktop host and save a PNG screenshot."""

from __future__ import annotations

import argparse
import os
import sys

from lupa import LuaRuntime

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from watch_host import WatchHost, SAFE_INSET, CORNER_INSET  # noqa: E402


def _click(host: WatchHost, label: str) -> bool:
    for b in host.state.buttons:
        if b.label == label and b.cb is not None:
            b.cb()
            return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("app", help="app id under data/apps/")
    ap.add_argument("--out", default=None)
    ap.add_argument("--click", action="append", default=[])
    ap.add_argument(
        "--shot",
        action="store_true",
        help="app proof: tilt_maze ball move, or portal Look+tilt sweep",
    )
    ap.add_argument(
        "--tilt",
        default=None,
        help="synthetic IMU ax,ay[,az] e.g. 0.4,0.3 or 0,0,1 (face-up)",
    )
    ap.add_argument(
        "--gyro",
        default=None,
        help="synthetic gyro gx,gy,gz in dps e.g. 0,0,45 (yaw rate about Z when flat)",
    )
    ap.add_argument("--ticks", type=int, default=0, help="extra on_tick fires after clicks")
    args = ap.parse_args()

    apps = os.path.join(ROOT, "data", "apps")
    entry = os.path.join(apps, args.app, "main.lua")
    if not os.path.isfile(entry):
        print(f"missing {entry}", file=sys.stderr)
        return 2

    host = WatchHost(apps)
    lua = LuaRuntime(unpack_returned_tuples=True)
    g = lua.globals()
    watch = lua.table()

    for name in (
        "set_title", "set_text", "append_text", "clear", "clear_ui",
        "width", "height", "millis", "delay", "fill", "rect", "circle",
        "text_at", "gfx_clear", "button_layout", "button", "back", "battery",
        "safe_inset", "corner_inset", "sd_ready", "sd_mount", "sd_unmount",
        "sd_read", "sd_write", "sd_mkdir", "sd_put_lfs",
        "sd_remove", "sd_rename",
        "usb_msc_active", "usb_msc_enter", "usb_msc_exit", "usb_msc_status",
        "lfs_list", "image_exists", "image", "image_clear", "squish_image_path",
        "squish_count", "squish_search", "letter_pad", "on_tick", "vibrate",
        "ball",
        "label", "panel", "progress", "progress_set", "slider", "z_raise",
        "on_back", "on_long_press",
        "prefs_get", "prefs_set",
        "wifi_disconnect", "audio_ready", "ble_ready", "ble_stop", "ble_status",
        "ble_disconnect", "mesh_connected", "mesh_channel", "mesh_alert_beep", "mesh_alert_vibrate", "launch_action", "launch_text", "speech_to_text",
        "brightness", "idle_timeout", "sprite", "image_frame",
        "gpio_write", "gpio_read",
        "battery_pct", "battery_mv", "charging", "usb_power", "battery_connected",
        "mic_stop", "mic_info", "speaker_ready", "speaker_stop", "speaker_volume", "speaker_volume_raw", "canvas", "canvas_clear", "canvas_scroll", "canvas_pixel",
    ):
        watch[name] = getattr(host, name)

    def imu_lua():
        d = host.imu()
        return lua.table(
            ax=d["ax"], ay=d["ay"], az=d["az"], gx=d["gx"], gy=d["gy"], gz=d["gz"]
        )

    def now_lua():
        d = host.now()
        return lua.table(**d)

    def time_lua():
        d = host.time()
        return lua.table(**d)

    def gesture_lua():
        d = host.gesture()
        if d is None:
            return None
        return lua.table(**d)

    def touch_delta_lua():
        return lua.table(**host.touch_delta())

    def touch_lua():
        d = host.touch()
        if d is None:
            return None
        return lua.table(**d)

    def wifi_state_lua():
        return host.wifi_state()

    def wifi_connect_lua(ssid, password="", timeout_ms=12000):
        ok, info = host.wifi_connect(ssid, password, timeout_ms)
        return ok, info

    def http_get_lua(url, max_bytes=32768):
        body, code = host.http_get(url, max_bytes)
        return body, code

    def set_alarm_lua(*a):
        return host.set_alarm(*a)

    def mic_record_lua(*a):
        return host.mic_record(*a)

    def mic_start_lua(opts=None):
        if opts is None:
            return host.mic_start()
        # lupa table -> dict
        if hasattr(opts, "keys"):
            d = {}
            for k in opts.keys():
                d[str(k)] = opts[k]
            return host.mic_start(d)
        return host.mic_start(opts)

    def mic_spectrum_lua(bins=64):
        r = host.mic_spectrum(bins)
        if isinstance(r, tuple):
            return r
        return lua.table_from(r)

    def mic_fft_lua(bins=64):
        return mic_spectrum_lua(bins)

    def canvas_row_lua(y, mags):
        if hasattr(mags, "values") or hasattr(mags, "keys"):
            n = len(mags)
            vals = [float(mags[i + 1]) for i in range(n)]
        else:
            vals = list(mags)
        return host.canvas_row(y, vals)

    def speaker_play_lua(*a):
        return host.speaker_play(*a)

    def mic_record_file_lua(path, seconds=3, rate=16000):
        return host.mic_record_file(path, seconds, rate)

    def mic_read_lua(max_samples=256):
        return host.mic_read(max_samples)

    def mic_read_table_lua(max_samples=256):
        r = host.mic_read_table(max_samples)
        if isinstance(r, tuple) and r[0] is None:
            return r
        samples, n = r
        return lua.table_from(samples), n

    def mic_info_lua():
        d = host.mic_info()
        return lua.table(**d)

    def speaker_start_lua(opts=None):
        if opts is None:
            return host.speaker_start()
        if hasattr(opts, "keys"):
            d = {str(k): opts[k] for k in opts.keys()}
            return host.speaker_start(d)
        return host.speaker_start(opts)

    def speaker_write_lua(pcm):
        return host.speaker_write(pcm)

    def ble_start_lua(*a):
        return host.ble_start(*a)

    def ble_scan_lua(timeout_ms=4000):
        hits = host.ble_scan(timeout_ms)
        out = lua.table()
        for i, h in enumerate(hits, 1):
            out[i] = lua.table(addr=h.get("addr"), name=h.get("name"), rssi=h.get("rssi", 0))
        return out

    def ble_connect_lua(addr, pin=None):
        r = host.ble_connect(addr, pin)
        if r is True:
            return True
        if isinstance(r, tuple):
            return r[0], r[1]
        return False, "fail"

    def mesh_send_lua(text):
        r = host.mesh_send(text)
        if r is True:
            return True
        if isinstance(r, tuple):
            return r[0], r[1]
        return False, "fail"

    def mesh_poll_lua():
        msgs = host.mesh_poll()
        out = lua.table()
        for i, m in enumerate(msgs, 1):
            out[i] = lua.table(from_=m.get("from"), text=m.get("text"), time=m.get("time", 0))
            # lupa uses from as reserved; also set string key "from"
            out[i]["from"] = m.get("from")
        return out

    def install_zip_lua(path, app_id, dest="lfs"):
        return host.install_zip(path, app_id, dest)

    def sd_list_lua(path):
        ents = host.sd_list(path)
        if ents is None:
            return None
        out = lua.table()
        for i, e in enumerate(ents, 1):
            out[i] = lua.table(
                name=e.get("name"),
                size=e.get("size", 0),
                is_dir=bool(e.get("is_dir")),
            )
        return out

    def sd_info_lua():
        info = host.sd_info()
        if info is None:
            return None
        return lua.table(**info)

    def gpio_pwm_lua(pin, duty, freq=5000):
        return host.gpio_pwm(pin, duty, freq)

    def gpio_adc_lua(pin):
        return host.gpio_adc(pin)

    def job_start_lua(name, opts=None):
        d = None
        if opts is not None and hasattr(opts, "keys"):
            d = {str(k): opts[k] for k in opts.keys()}
        elif isinstance(opts, dict):
            d = opts
        id_, err = host.job_start(name, d)
        if id_ is None:
            return None, err
        return id_

    def job_status_lua(jid):
        return host.job_status(jid)

    def job_result_lua(jid):
        res, err = host.job_result(jid)
        if res is None:
            return None, err
        if isinstance(res, list):
            return lua.table_from(res)
        return res

    def job_cancel_lua(jid):
        return host.job_cancel(jid)


    watch["imu"] = imu_lua
    watch["now"] = now_lua
    watch["time"] = time_lua
    watch["gesture"] = gesture_lua
    watch["touch_delta"] = touch_delta_lua
    watch["touch"] = touch_lua
    watch["wifi_state"] = wifi_state_lua
    watch["wifi_connect"] = wifi_connect_lua
    watch["http_get"] = http_get_lua
    watch["set_alarm"] = set_alarm_lua
    watch["mic_record"] = mic_record_lua
    watch["mic_record_file"] = mic_record_file_lua
    watch["mic_start"] = mic_start_lua
    watch["mic_info"] = mic_info_lua
    watch["mic_read"] = mic_read_lua
    watch["mic_read_table"] = mic_read_table_lua
    watch["mic_spectrum"] = mic_spectrum_lua
    watch["mic_fft"] = mic_fft_lua
    watch["canvas_row"] = canvas_row_lua
    watch["canvas_pixel"] = host.canvas_pixel
    watch["speaker_play"] = speaker_play_lua
    watch["speaker_start"] = speaker_start_lua
    watch["speaker_write"] = speaker_write_lua
    watch["ble_start"] = ble_start_lua
    watch["ble_scan"] = ble_scan_lua
    watch["ble_connect"] = ble_connect_lua
    watch["ble_disconnect"] = host.ble_disconnect
    watch["mesh_connected"] = host.mesh_connected
    watch["mesh_send"] = mesh_send_lua
    watch["mesh_poll"] = mesh_poll_lua
    watch["mesh_channel"] = host.mesh_channel
    watch["mesh_alert_beep"] = host.mesh_alert_beep
    watch["mesh_alert_vibrate"] = host.mesh_alert_vibrate
    watch["install_zip"] = install_zip_lua
    watch["job_start"] = job_start_lua
    watch["job_status"] = job_status_lua
    watch["job_result"] = job_result_lua
    watch["job_cancel"] = job_cancel_lua
    watch["sd_list"] = sd_list_lua
    watch["sd_info"] = sd_info_lua
    watch["gpio_pwm"] = gpio_pwm_lua
    watch["gpio_adc"] = gpio_adc_lua
    g.watch = watch
    g.print = lambda *a: print(*[str(x) for x in a])

    with open(entry, "r", encoding="utf-8") as f:
        src = f.read()
    try:
        lua.execute(src)
    except Exception as e:
        print(f"LUA ERROR: {e}", file=sys.stderr)
        print("continuing to screenshot partial UI")

    def _parse_tilt(s: str):
        parts = [float(x.strip()) for x in s.split(",")]
        if len(parts) == 2:
            host.set_tilt(parts[0], parts[1])
        elif len(parts) >= 3:
            host.set_tilt(parts[0], parts[1], parts[2])
        else:
            raise SystemExit(f"bad --tilt {s!r}; want ax,ay[,az]")

    if args.tilt:
        _parse_tilt(args.tilt)
        print(f"tilt set ax={host.imu_ax:+.3f} ay={host.imu_ay:+.3f} az={host.imu_az:+.3f}")

    if args.gyro:
        parts = [float(x.strip()) for x in args.gyro.split(",")]
        if len(parts) != 3:
            raise SystemExit(f"bad --gyro {args.gyro!r}; want gx,gy,gz dps")
        host.set_gyro(parts[0], parts[1], parts[2])
        print(f"gyro set gx={host.imu_gx:+.2f} gy={host.imu_gy:+.2f} gz={host.imu_gz:+.2f} dps")

    for label in args.click:
        if not _click(host, label):
            print(f"warn: button {label!r} not found", file=sys.stderr)

    ball_before = None
    if args.shot and args.app == "portal":
        import re

        def _portal_field(name: str):
            m = re.search(rf"{name}\s+([+-]?\d+(?:\.\d+)?)", host.state.text or "")
            return float(m.group(1)) if m else None

        host.use_sim_clock(0)
        if not _click(host, "Look"):
            # maybe already clicked via --click Look
            if "Look" not in (args.click or []):
                print("shot: Look button missing", file=sys.stderr)
                return 3
        # Hold still through calib (face-up flat, gyro quiet)
        host.set_gyro(0.0, 0.0, 0.0)
        for i in range(40):
            host.set_tilt(0.0, 0.0, 1.0)
            host.advance_ms(50)
            host.fire_tick()
        # Face-up (az=+1): tip top away/up → look toward ceiling → +pitch
        host.set_tilt(0.0, 0.0, 1.0)
        host.set_gyro(0.0, 0.0, 0.0)
        for i in range(12):
            host.advance_ms(50)
            host.fire_tick()
        pitch_up = _portal_field("pitch")
        # Face-down (az=-1): flip → floor → −pitch
        host.set_tilt(0.0, 0.0, -1.0)
        for i in range(12):
            host.advance_ms(50)
            host.fire_tick()
        pitch_down = _portal_field("pitch")
        # Tip right while near-flat → roll (bank), not only yaw
        host.set_tilt(ax=0.05, ay=0.55, az=0.85)
        host.set_gyro(0.0, 0.0, 0.0)
        for i in range(16):
            host.advance_ms(50)
            host.fire_tick()
        roll_tip = _portal_field("roll")
        # Gyro pan: face-up + gz → yaw integrates (capturable pan)
        host.set_tilt(0.0, 0.0, 1.0)
        host.set_gyro(0.0, 0.0, 0.0)
        for i in range(8):
            host.advance_ms(50)
            host.fire_tick()
        yaw_before = _portal_field("yaw")
        host.set_gyro(0.0, 0.0, 90.0)  # 90 dps about Z ≈ gravity when flat
        for i in range(max(args.ticks, 20)):
            host.advance_ms(50)
            host.fire_tick()
        yaw_after = _portal_field("yaw")
        roll_after = _portal_field("roll")
        print(
            f"portal shot tilt=ax={host.imu_ax:+.2f},ay={host.imu_ay:+.2f},"
            f"az={host.imu_az:+.2f} gyro_gz={host.imu_gz:+.1f} "
            f"pitch_up={pitch_up} pitch_down={pitch_down} "
            f"roll_tip={roll_tip} yaw_before={yaw_before} yaw_after={yaw_after}"
        )
        if pitch_up is None or pitch_down is None:
            print("FAIL: portal pitch not in status text", file=sys.stderr)
            return 8
        if not (pitch_up > 0.5 and pitch_down < -0.5):
            print(
                f"FAIL: pitch mapping — tip-up/face-up should look ceiling (+pitch), "
                f"face-down floor (−pitch); got up={pitch_up} down={pitch_down}",
                file=sys.stderr,
            )
            return 9
        if roll_tip is None or abs(roll_tip) < 0.25:
            print(
                f"FAIL: roll not wired — tip ay should bank horizon; got roll={roll_tip}",
                file=sys.stderr,
            )
            return 10
        if yaw_before is None or yaw_after is None:
            print("FAIL: portal yaw not in status text (pan not capturable)", file=sys.stderr)
            return 11
        if abs(yaw_after - yaw_before) < 0.35:
            print(
                f"FAIL: gyro yaw/pan — expected |Δyaw|>=0.35 from gz=90dps; "
                f"before={yaw_before} after={yaw_after}",
                file=sys.stderr,
            )
            return 12
        print(
            "PASS: portal Look + pitch ceiling/floor + roll bank + gyro yaw/pan"
        )
    elif args.shot:
        host.use_sim_clock(0)
        if not _click(host, "Play"):
            print("shot: Play button missing", file=sys.stderr)
            return 3
        if host.state.ball:
            ball_before = (host.state.ball.x, host.state.ball.y)
        # Hold still through calib (zero tilt), then tip.
        # Host ticks ~50ms (or 200ms on device); Lua uses fixed 20ms substeps.
        calib_ticks = 40
        for i in range(calib_ticks):
            host.set_tilt(0.0, 0.0)
            host.advance_ms(50)
            host.fire_tick()
        after_calib = (host.state.ball.x, host.state.ball.y) if host.state.ball else None
        # After v1.1.4 X negate: +ay → ball LEFT; +ax → ball DOWN (Y unchanged).
        # Sim tilt pad drag-right still drives +ay → now LEFT (mirrored vs physical tip-right).
        host.set_tilt(ax=0.40, ay=0.45)
        play_ticks = max(args.ticks, 50)
        prev = after_calib
        max_tick_d = 0.0
        for i in range(play_ticks):
            # Simulate sluggish host (~200ms) to prove substeps prevent teleports
            host.advance_ms(200 if i < 8 else 50)
            host.fire_tick()
            if host.state.ball and prev is not None and i < 12:
                cur = (host.state.ball.x, host.state.ball.y)
                ddx = cur[0] - prev[0]
                ddy = cur[1] - prev[1]
                d = (ddx * ddx + ddy * ddy) ** 0.5
                max_tick_d = max(max_tick_d, d)
                print(f"tick[{i}] pos=({cur[0]:.1f},{cur[1]:.1f}) d=({ddx:+.2f},{ddy:+.2f}) |d|={d:.2f}")
                prev = cur
            elif host.state.ball:
                prev = (host.state.ball.x, host.state.ball.y)
        after = (host.state.ball.x, host.state.ball.y) if host.state.ball else None
        print(
            f"shot calib_ball={after_calib} before={ball_before} "
            f"after={after} "
            f"tilt=ax={host.imu_ax:+.2f},ay={host.imu_ay:+.2f} max_tick_d={max_tick_d:.2f}"
        )
        if after and after_calib:
            # +ay → -X (left) after sapphire X negate; require net leftward under +ay
            if after[0] >= after_calib[0]:
                print(
                    f"FAIL: L/R axis — expected -X under +ay "
                    f"(calib_x={after_calib[0]}, after_x={after[0]})",
                    file=sys.stderr,
                )
                return 6
            if max_tick_d > 40:
                print(
                    f"FAIL: teleport — max per-tick |d|={max_tick_d:.1f} > 40px",
                    file=sys.stderr,
                )
                return 7
    elif args.ticks > 0:
        for i in range(args.ticks):
            host.advance_ms(50)
            host.fire_tick()

    out = args.out
    if not out:
        shot_dir = os.environ.get("WATCHOS_SIM_SHOTS", "/workspace/watchos-sim-shots")
        os.makedirs(shot_dir, exist_ok=True)
        suffix = "_shot" if args.shot else ""
        out = os.path.join(shot_dir, f"{args.app}{suffix}.png")

    img = host.render()
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    img.save(out)
    print(f"sim ok app={args.app} SAFE_INSET={SAFE_INSET} CORNER_INSET={CORNER_INSET}")
    print(f"buttons={[b.label for b in host.state.buttons]} layout={host.state.layout}")
    print(f"wrote {out}")

    if args.shot and args.app == "portal":
        pass  # portal proof already printed above
    elif args.shot:
        if not host.state.ball or ball_before is None:
            print("FAIL: no ball sprite", file=sys.stderr)
            return 4
        ax0, ay0 = ball_before
        ax1, ay1 = host.state.ball.x, host.state.ball.y
        dist2 = (ax1 - ax0) ** 2 + (ay1 - ay0) ** 2
        print(f"ball move delta=({ax1 - ax0},{ay1 - ay0}) dist2={dist2}")
        if dist2 < 25:  # at least ~5px
            print("FAIL: ball did not move under tilt", file=sys.stderr)
            return 5
        print("PASS: ball moved under simulated tilt")
        # Firmware used to reject non-integral coords via luaL_checkinteger.
        # Probe float args through the same Lua binding path the app uses.
        try:
            lua.execute("watch.ball(56.7, 84.3, 10, 220, 200, 60)")
            if not host.state.ball:
                print("FAIL: float watch.ball cleared sprite", file=sys.stderr)
                return 8
            print(
                f"PASS: float watch.ball -> "
                f"({host.state.ball.x},{host.state.ball.y})"
            )
        except Exception as e:
            print(f"FAIL: float watch.ball raised: {e}", file=sys.stderr)
            return 8
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
