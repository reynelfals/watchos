#!/usr/bin/env python3
"""Quick sim check for waterfall mic + canvas APIs."""
from __future__ import annotations
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from watch_host import WatchHost  # noqa: E402


def main() -> int:
    host = WatchHost(os.path.join(os.path.dirname(HERE), "data", "apps"))
    assert host.audio_ready() is True
    assert host.mic_start({"rate": 16000}) is True
    assert host.mic_info()["running"] is True
    pcm, n = host.mic_read(256)
    assert isinstance(pcm, (bytes, bytearray)) and n == 256 and len(pcm) == 512
    assert host.canvas(0, 0, 64, 40) is True
    for _ in range(12):
        spec = host.mic_spectrum(64)
        assert isinstance(spec, list) and len(spec) == 64
        host.canvas_scroll(-1)
        host.canvas_row(39, spec)
    host.mic_stop()
    assert host.mic_read(16)[1] == "mic_not_running"
    img = host.render_lcd()
    assert img.size == (410, 502)
    print("PASS sim/test_waterfall.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
