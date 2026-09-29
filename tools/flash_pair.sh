#!/usr/bin/env bash
# Flash firmware THEN LittleFS. Refuse success if uploadfs fails.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
PORT="${UPLOAD_PORT:-/dev/ttyACM0}"
export PLATFORMIO_UPLOAD_PORT="$PORT"

echo "==> Building"
pio run

echo "==> Uploading firmware to $PORT"
pio run -t upload

echo "==> Uploading LittleFS (required companion)"
set +e
pio run -t uploadfs
fs_rc=$?
set -e

if [[ $fs_rc -ne 0 ]]; then
  echo ""
  echo "REFUSED: firmware may be new but LittleFS upload FAILED (exit $fs_rc)."
  echo "Device is on a MIXED revision — do not call this done."
  echo "Retry with BOOT held, or SoftAP apps from the matching git tag."
  echo "See docs/ROLLBACK.md"
  exit $fs_rc
fi

echo ""
echo "OK: firmware + LittleFS both succeeded on $PORT"
echo "When device smoke-test passes: ./tools/mark_lkg.sh \"description\""
