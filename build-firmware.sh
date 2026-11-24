#!/usr/bin/env bash
set -euo pipefail

# Build both ANCHOR C5 variants from a single source file.
# Default: production (no USB bench commands or verbose traces).
# Debug: ANCHOR_DEBUG=1, includes USB diagnostics and test controls.
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
variant="${1:-production}"
case "$variant" in
  production) debug=0 ;;
  debug) debug=1 ;;
  *) echo 'Usage: ANCHOR/build-firmware.sh [production|debug]' >&2; exit 2 ;;
esac

cli="${ARDUINO_CLI:-arduino-cli}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/anchor_c5"
cp "$project_dir/firmware/anchor_c5.ino" "$tmp/anchor_c5/anchor_c5.ino"
cp "$project_dir/firmware/trainer_protocol.h" "$tmp/anchor_c5/trainer_protocol.h"
out="$project_dir/dist/$variant"
mkdir -p "$out"
"$cli" compile \
  --fqbn esp32:esp32:XIAO_ESP32C5 \
  --build-property "compiler.cpp.extra_flags=-DANCHOR_DEBUG=$debug" \
  --output-dir "$out" \
  "$tmp/anchor_c5"

cp "$out/anchor_c5.ino.bin" "$out/ANCHOR-C5-$variant-app.bin"
cp "$out/anchor_c5.ino.merged.bin" "$out/ANCHOR-C5-$variant-full.bin"
test "$(stat -c %s "$out/ANCHOR-C5-$variant-full.bin")" -eq 8388608
(cd "$out" && sha256sum "ANCHOR-C5-$variant-app.bin" "ANCHOR-C5-$variant-full.bin" > SHA256SUMS)
cat "$out/SHA256SUMS"
