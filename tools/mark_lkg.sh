#!/usr/bin/env bash
# Move the last-known-good tag to HEAD after a verified flash pair.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
MSG="${1:-verified flash pair}"
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Not a git repo: $ROOT" >&2
  exit 1
fi
git tag -f -a lkg -m "LKG: $MSG"
echo "Tagged lkg -> $(git rev-parse --short HEAD): $MSG"
git tag -l 'fw/*' 'app/*' lkg | tail -20
