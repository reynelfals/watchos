#!/usr/bin/env bash
# Stage one Lua app from a git tag/commit into ~/watchos-ota/<app>/ for SoftAP.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
REF="${1:?usage: stage_ota_from_git.sh <git-ref> <app_id>}"
APP="${2:?usage: stage_ota_from_git.sh <git-ref> <app_id>}"
OUT="${HOME}/watchos-ota/${APP}"
mkdir -p "$OUT"

if ! git cat-file -e "${REF}:data/apps/${APP}/main.lua" 2>/dev/null; then
  echo "Missing ${REF}:data/apps/${APP}/main.lua" >&2
  exit 1
fi

git show "${REF}:data/apps/${APP}/main.lua" > "$OUT/main.lua"
if git cat-file -e "${REF}:data/apps/${APP}/app.json" 2>/dev/null; then
  git show "${REF}:data/apps/${APP}/app.json" > "$OUT/app.json"
fi

cat > "$OUT/README_PHONE_OTA.txt" << README
WatchOS SoftAP rollback package
ref: ${REF}
app: ${APP}
staged: $(date -Iseconds)

Install → Phone SoftAP → join WatchOS-OTA → upload main.lua (+ app.json).
README

echo "Staged $OUT from $REF"
ls -la "$OUT"
