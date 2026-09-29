#!/usr/bin/env python3
"""Unit-test appsHostSanitizeId rules + fake FS recursive remove (no hardware)."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
APP_ID_MAX = 32


def sanitize_id(folder_or_id: str) -> str | None:
    """Mirror appsHostSanitizeId (strict reject on bad chars / ..)."""
    if folder_or_id is None:
        return None
    if ".." in folder_or_id:
        return None
    p = folder_or_id
    if "/" in p:
        p = p.rsplit("/", 1)[-1]
    if not p or p in (".", ".."):
        return None
    out = []
    for c in p:
        if c.isalnum() or c in "_-":
            out.append(c)
        else:
            return None
    if not out or len(out) >= APP_ID_MAX:
        return None
    return "".join(out)


def is_protected(app_json_path: str) -> bool:
    try:
        with open(app_json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return bool(data.get("protected") is True)
    except Exception:
        return False


def remove_app(apps_root: str, folder_or_id: str) -> bool:
    sid = sanitize_id(folder_or_id)
    if not sid:
        return False
    folder = os.path.join(apps_root, sid)
    # Must stay under apps_root
    real_apps = os.path.realpath(apps_root)
    real_folder = os.path.realpath(folder)
    if real_folder != os.path.join(real_apps, sid):
        return False
    if not os.path.isdir(folder):
        return False
    meta = os.path.join(folder, "app.json")
    if os.path.isfile(meta) and is_protected(meta):
        return False
    shutil.rmtree(folder)
    return True


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def test_sanitize() -> None:
    expect(sanitize_id("My_app") == "My_app", "bare id")
    expect(sanitize_id("/apps/My_app") == "My_app", "path id")
    expect(sanitize_id("counter") == "counter", "counter")
    expect(sanitize_id("../etc") is None, "dotdot")
    expect(sanitize_id("/apps/../etc") is None, "path dotdot")
    expect(sanitize_id("bad name") is None, "space")
    expect(sanitize_id("a/b") == "b", "basename only if chars ok")
    expect(sanitize_id("") is None, "empty")
    expect(sanitize_id(".") is None, "dot")
    expect(sanitize_id("..") is None, "dots")
    expect(sanitize_id("x" * APP_ID_MAX) is None, "too long")
    expect(sanitize_id("ok-id_1") == "ok-id_1", "dash underscore")
    print("PASS sanitize")


def test_remove_tree() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        apps = os.path.join(tmp, "apps")
        target = os.path.join(apps, "demo_app")
        os.makedirs(os.path.join(target, "img"))
        with open(os.path.join(target, "app.json"), "w", encoding="utf-8") as f:
            f.write('{"id":"demo_app","name":"Demo","version":"0.1.0","entry":"main.lua"}')
        with open(os.path.join(target, "main.lua"), "w", encoding="utf-8") as f:
            f.write("-- demo\n")
        with open(os.path.join(target, "img", "a.bin"), "wb") as f:
            f.write(b"\x00\x01")

        prot = os.path.join(apps, "locked")
        os.makedirs(prot)
        with open(os.path.join(prot, "app.json"), "w", encoding="utf-8") as f:
            f.write(
                '{"id":"locked","name":"Locked","version":"1","entry":"main.lua",'
                '"protected":true}'
            )

        expect(remove_app(apps, "demo_app") is True, "remove demo")
        expect(not os.path.exists(target), "demo gone")
        expect(remove_app(apps, "locked") is False, "protected refused")
        expect(os.path.isdir(prot), "locked remains")
        expect(remove_app(apps, "../demo_app") is False, "escape refused")
        expect(remove_app(apps, "missing") is False, "missing")
    print("PASS remove_tree")


def test_manage_mock_shot() -> None:
    """Minimal Manage Apps mock screenshot (PIL) for docs / visual check."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("SKIP manage shot (no PIL)")
        return

    W, H = 410, 502
    img = Image.new("RGB", (W, H), (5, 5, 8))
    d = ImageDraw.Draw(img)
    # title
    d.text((W // 2 - 70, 12), "Manage Apps", fill=(62, 224, 240))
    d.rectangle([8, 8, 70, 36], fill=(18, 20, 28))
    d.text((18, 14), "Back", fill=(139, 147, 167))
    d.text((80, 48), "Tap an app to uninstall (3)", fill=(139, 147, 167))
    # fake list
    rows = [("Counter  ·  v0.1.0", False), ("My App  ·  v0.1.0", False),
            ("Locked  [protected]", True)]
    y = 80
    for label, dim in rows:
        d.rounded_rectangle([10, y, W - 10, y + 48], radius=10, fill=(18, 20, 28))
        color = (139, 147, 167) if dim else (244, 247, 251)
        d.text((24, y + 14), label, fill=color)
        y += 56
    # confirm overlay mock (lower third sample)
    d.rectangle([0, 0, W, H], fill=(0, 0, 0, 128)) if False else None
    panel = [35, 140, W - 35, 380]
    d.rounded_rectangle(panel, radius=16, fill=(18, 20, 28))
    d.text((W // 2 - 50, 160), "Uninstall", fill=(244, 247, 251))
    d.text((W // 2 - 40, 190), "My App", fill=(62, 224, 240))
    d.text((W // 2 - 55, 220), "(My_app)?", fill=(244, 247, 251))
    d.text((W // 2 - 90, 250), "Deletes /apps/My_app/", fill=(139, 147, 167))
    d.rounded_rectangle([55, 310, 185, 355], radius=12, fill=(255, 107, 107))
    d.text((75, 322), "Uninstall", fill=(244, 247, 251))
    d.rounded_rectangle([225, 310, 355, 355], radius=12, fill=(28, 39, 64))
    d.text((260, 322), "Cancel", fill=(244, 247, 251))

    out_dir = os.environ.get("WATCHOS_SIM_SHOTS", "/workspace/watchos-sim-shots")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "manage_uninstall_confirm.png")
    img.save(out)
    print(f"PASS manage_shot wrote {out}")


def main() -> int:
    test_sanitize()
    test_remove_tree()
    test_manage_mock_shot()
    print("ALL OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
