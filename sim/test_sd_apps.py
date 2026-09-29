#!/usr/bin/env python3
"""Unit-test SD + LittleFS app catalog merge (no hardware).

Mirrors appsHostRescan rules:
- Scan lfs_root/*/app.json then sd_root/*/app.json
- Dedupe by id: LittleFS wins
- Record storage + full script path
- Remove deletes under the correct root
"""

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


def load_app(folder: str, storage: str) -> dict | None:
    meta = os.path.join(folder, "app.json")
    if not os.path.isfile(meta):
        return None
    try:
        with open(meta, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return None
    app_id = str(data.get("id") or os.path.basename(folder))
    name = str(data.get("name") or app_id)
    version = str(data.get("version") or "")
    entry = str(data.get("entry") or "main.lua")
    protected = data.get("protected") is True
    script = os.path.join(folder, entry)
    return {
        "id": app_id,
        "name": name,
        "version": version,
        "entry": entry,
        "folder": folder,
        "script": script,
        "storage": storage,
        "is_protected": protected,
    }


def scan_merge(lfs_apps: str, sd_apps: str | None) -> list[dict]:
    """Merge catalogs; LittleFS wins on id clash."""
    out: list[dict] = []
    seen: set[str] = set()

    if os.path.isdir(lfs_apps):
        for name in sorted(os.listdir(lfs_apps)):
            folder = os.path.join(lfs_apps, name)
            if not os.path.isdir(folder):
                continue
            info = load_app(folder, "lfs")
            if not info:
                continue
            out.append(info)
            seen.add(info["id"])

    if sd_apps and os.path.isdir(sd_apps):
        for name in sorted(os.listdir(sd_apps)):
            folder = os.path.join(sd_apps, name)
            if not os.path.isdir(folder):
                continue
            info = load_app(folder, "sd")
            if not info:
                continue
            if info["id"] in seen:
                continue  # LittleFS wins
            out.append(info)
            seen.add(info["id"])
    return out


def remove_app(catalog: list[dict], folder_or_id: str) -> bool:
    sid = sanitize_id(folder_or_id)
    if not sid:
        return False
    match = next((a for a in catalog if a["id"] == sid), None)
    if not match:
        return False
    if match["is_protected"]:
        return False
    folder = match["folder"]
    if not os.path.isdir(folder):
        return False
    # Path must stay under its apps root parent
    parent = os.path.dirname(folder)
    if os.path.basename(folder) != sid:
        return False
    if not os.path.isdir(parent):
        return False
    shutil.rmtree(folder)
    return True


def expect(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def write_app(root: str, app_id: str, name: str, *, protected: bool = False) -> str:
    folder = os.path.join(root, app_id)
    os.makedirs(folder, exist_ok=True)
    meta = {
        "id": app_id,
        "name": name,
        "version": "0.1.0",
        "entry": "main.lua",
    }
    if protected:
        meta["protected"] = True
    with open(os.path.join(folder, "app.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f)
    with open(os.path.join(folder, "main.lua"), "w", encoding="utf-8") as f:
        f.write(f"-- {app_id}\nwatch.set_title({name!r})\n")
    return folder


def test_merge_and_clash() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        lfs = os.path.join(tmp, "lfs_apps")
        sd = os.path.join(tmp, "sd_apps")
        os.makedirs(lfs)
        os.makedirs(sd)

        write_app(lfs, "counter", "Counter")
        write_app(lfs, "shared", "Flash Shared")
        write_app(sd, "sd_hello", "SD Hello")
        write_app(sd, "shared", "SD Shared")  # clash — ignored

        cat = scan_merge(lfs, sd)
        ids = [a["id"] for a in cat]
        expect(ids == ["counter", "shared", "sd_hello"], f"order/ids {ids}")
        by_id = {a["id"]: a for a in cat}
        expect(by_id["counter"]["storage"] == "lfs", "counter lfs")
        expect(by_id["sd_hello"]["storage"] == "sd", "sd_hello sd")
        expect(by_id["shared"]["storage"] == "lfs", "shared prefers lfs")
        expect(by_id["shared"]["name"] == "Flash Shared", "shared name from lfs")
        expect(
            by_id["sd_hello"]["script"].endswith("sd_hello/main.lua"),
            "script path",
        )
        expect(os.path.isfile(by_id["sd_hello"]["script"]), "script exists")
    print("PASS merge_and_clash")


def test_sd_only_and_remove() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        lfs = os.path.join(tmp, "lfs_apps")
        sd = os.path.join(tmp, "sd_apps")
        os.makedirs(lfs)
        os.makedirs(sd)
        write_app(sd, "sd_hello", "SD Hello")
        write_app(sd, "locked", "Locked", protected=True)

        cat = scan_merge(lfs, sd)
        expect(len(cat) == 2, "two sd apps")
        expect(remove_app(cat, "sd_hello") is True, "remove sd_hello")
        expect(not os.path.isdir(os.path.join(sd, "sd_hello")), "folder gone")
        cat2 = scan_merge(lfs, sd)
        expect([a["id"] for a in cat2] == ["locked"], "only locked left")
        expect(remove_app(cat2, "locked") is False, "protected refused")
        expect(os.path.isdir(os.path.join(sd, "locked")), "locked remains")
    print("PASS sd_only_and_remove")


def test_stage_demo_present() -> None:
    stage = os.path.join(ROOT, "tools", "sd_apps_stage", "sd_hello")
    expect(os.path.isfile(os.path.join(stage, "app.json")), "stage app.json")
    expect(os.path.isfile(os.path.join(stage, "main.lua")), "stage main.lua")
    with open(os.path.join(stage, "app.json"), encoding="utf-8") as f:
        data = json.load(f)
    expect(data.get("id") == "sd_hello", "stage id")
    print("PASS stage_demo_present")


def main() -> int:
    test_merge_and_clash()
    test_sd_only_and_remove()
    test_stage_demo_present()
    print("ALL OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
