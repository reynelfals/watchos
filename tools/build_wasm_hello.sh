#!/usr/bin/env bash
# Build data/apps/wasm_hello/main.wasm from main.c
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/data/apps/wasm_hello/main.c"
OUT="$ROOT/data/apps/wasm_hello/main.wasm"

CLANG=""
if [[ -n "${WASI_SDK_PATH:-}" && -x "${WASI_SDK_PATH}/bin/clang" ]]; then
  CLANG="${WASI_SDK_PATH}/bin/clang"
elif [[ -x "/tmp/wasi-sdk-24.0-x86_64-linux/bin/clang" ]]; then
  CLANG="/tmp/wasi-sdk-24.0-x86_64-linux/bin/clang"
elif command -v clang >/dev/null 2>&1; then
  CLANG="$(command -v clang)"
fi

if [[ -z "$CLANG" ]]; then
  echo "No wasm32 clang found. Install wasi-sdk and set WASI_SDK_PATH,"
  echo "or keep the vendored prebuilt main.wasm."
  exit 1
fi

echo "Using: $CLANG"
"$CLANG" --target=wasm32 -nostdlib -Os \
  -Wl,--no-entry \
  -Wl,--export=init \
  -Wl,--export=tick \
  -Wl,--export=on_back \
  -Wl,--allow-undefined \
  -Wl,--export-memory \
  -o "$OUT" "$SRC"

ls -la "$OUT"
echo "Built $OUT"
