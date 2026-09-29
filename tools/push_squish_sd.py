#!/usr/bin/env python3
"""Push staged Squish WRGB images to the watch microSD via SoftAP/STA OTA.

Requires firmware with POST /upload_sd (writes /sd/<path> via sdHost).

Typical flow:
  1. On the watch: Launcher → Install → start SoftAP (or Home Wi-Fi).
  2. Join WatchOS-OTA (or use watch LAN IP) and note the session ?k= token.
  3. Run:
       python3 tools/push_squish_sd.py \\
         --stage tools/squish_sd_stage/img \\
         --host 192.168.4.1 \\
         --k <session-token>

Does not touch LittleFS squish samples or app_secrets.
"""

from __future__ import annotations

import argparse
import sys
import time
import uuid
from pathlib import Path

try:
    import urllib.error
    import urllib.parse
    import urllib.request
except ImportError:  # pragma: no cover
    print("Python 3 with urllib required", file=sys.stderr)
    sys.exit(2)


def list_wrgb(stage: Path) -> list[Path]:
    return sorted(
        p for p in stage.iterdir() if p.is_file() and p.suffix.lower() == ".wrgb"
    )


def multipart_body(field_name: str, filename: str, data: bytes) -> tuple[bytes, str]:
    boundary = f"----WatchOsSd{uuid.uuid4().hex}"
    disposition = (
        f'Content-Disposition: form-data; name="{field_name}"; '
        f'filename="{filename}"'
    )
    # Binary WRGB — no charset
    parts = [
        f"--{boundary}\r\n".encode(),
        f"{disposition}\r\n".encode(),
        b"Content-Type: application/octet-stream\r\n\r\n",
        data,
        f"\r\n--{boundary}--\r\n".encode(),
    ]
    body = b"".join(parts)
    content_type = f"multipart/form-data; boundary={boundary}"
    return body, content_type


def upload_one(
    host: str,
    token: str,
    rel_path: str,
    file_path: Path,
    timeout: float,
) -> tuple[bool, str]:
    qs = urllib.parse.urlencode({"path": rel_path, "k": token})
    url = f"http://{host}/upload_sd?{qs}"
    data = file_path.read_bytes()
    body, content_type = multipart_body("file", file_path.name, data)
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": content_type,
            "Content-Length": str(len(body)),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8", errors="replace").strip()
            if 200 <= resp.status < 300:
                return True, text
            return False, f"HTTP {resp.status}: {text}"
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace").strip()
        return False, f"HTTP {e.code}: {detail or e.reason}"
    except Exception as e:  # noqa: BLE001 — report any transport error
        return False, f"{type(e).__name__}: {e}"


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Bulk-push staged .wrgb files to watch /sd/squish/img/ via SoftAP OTA"
    )
    ap.add_argument(
        "--stage",
        type=Path,
        default=Path("tools/squish_sd_stage/img"),
        help="Directory of .wrgb files (default: tools/squish_sd_stage/img)",
    )
    ap.add_argument(
        "--host",
        default="192.168.4.1",
        help="Watch SoftAP/STA IP (default: 192.168.4.1)",
    )
    ap.add_argument(
        "--k",
        required=True,
        help="Session token from Install SoftAP page (?k=)",
    )
    ap.add_argument(
        "--sd-prefix",
        default="squish/img",
        help="Path under /sd/ (default: squish/img)",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="Per-file HTTP timeout seconds (default: 60)",
    )
    ap.add_argument(
        "--retries",
        type=int,
        default=2,
        help="Retries per file after failure (default: 2)",
    )
    ap.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Upload at most N files (0 = all)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="List files only; do not POST",
    )
    ap.add_argument(
        "--sleep",
        type=float,
        default=0.05,
        help="Pause between files (seconds); keeps SoftAP deadline warm",
    )
    args = ap.parse_args()

    stage = args.stage
    if not stage.is_dir():
        print(f"ERROR: stage dir not found: {stage}", file=sys.stderr)
        return 2

    files = list_wrgb(stage)
    if args.limit and args.limit > 0:
        files = files[: args.limit]
    if not files:
        print(f"No .wrgb files in {stage}", file=sys.stderr)
        return 1

    prefix = args.sd_prefix.strip("/")
    print(f"Host={args.host}  stage={stage}  files={len(files)}  dest=/sd/{prefix}/")
    if args.dry_run:
        for p in files[:10]:
            print(f"  would upload {p.name} -> /sd/{prefix}/{p.name}")
        if len(files) > 10:
            print(f"  ... and {len(files) - 10} more")
        return 0

    ok_n = 0
    fail_n = 0
    t0 = time.time()
    for i, fp in enumerate(files, 1):
        rel = f"{prefix}/{fp.name}"
        attempt = 0
        last_err = ""
        success = False
        while attempt <= args.retries:
            attempt += 1
            success, msg = upload_one(args.host, args.k, rel, fp, args.timeout)
            if success:
                last_err = msg
                break
            last_err = msg
            if attempt <= args.retries:
                time.sleep(0.4 * attempt)
        if success:
            ok_n += 1
            if i == 1 or i % 25 == 0 or i == len(files):
                elapsed = time.time() - t0
                rate = ok_n / elapsed if elapsed > 0 else 0
                print(f"[{i}/{len(files)}] OK {fp.name}  ({ok_n} ok, {fail_n} fail, {rate:.1f}/s)")
        else:
            fail_n += 1
            print(f"[{i}/{len(files)}] FAIL {fp.name}: {last_err}", file=sys.stderr)
        if args.sleep > 0:
            time.sleep(args.sleep)

    elapsed = time.time() - t0
    print(f"Done: {ok_n} ok, {fail_n} fail, {elapsed:.1f}s")
    return 0 if fail_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
