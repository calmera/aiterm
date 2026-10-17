#!/usr/bin/env bash
# Build a single flashable image (offset 0x0) from the build directory's flasher_args.json.
# Usage: tools/make_merged_bin.sh [build_dir] [out_file]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$HERE/build}"
OUT="${2:-$HERE/out/ai-terminal-merged.bin}"
mkdir -p "$(dirname "$OUT")"
if ! command -v esptool.py >/dev/null 2>&1 && ! python -m esptool version >/dev/null 2>&1; then
  echo "esptool not found; source env.sh (or the ESP-IDF export.sh) first" >&2; exit 1
fi
ESPTOOL="esptool.py"; command -v esptool.py >/dev/null 2>&1 || ESPTOOL="python -m esptool"
# Derive <offset> <file> pairs and flash settings from flasher_args.json (never hand-write offsets).
ARGS=$(python3 - "$BUILD/flasher_args.json" <<'PY'
import json,sys
j=json.load(open(sys.argv[1]))
fs=j["flash_settings"]
parts=[]
for off,f in sorted(j["flash_files"].items(), key=lambda kv:int(kv[0],16)):
    parts.append(f"{off} {f}")
print(f'--flash_mode {fs["flash_mode"]} --flash_freq {fs["flash_freq"]} --flash_size {fs["flash_size"]} ' + " ".join(parts))
PY
)
cd "$BUILD"
# shellcheck disable=SC2086
$ESPTOOL --chip esp32p4 merge_bin -o "$OUT" $ARGS
echo "wrote $OUT ($(stat -c %s "$OUT") bytes)"
cp "$BUILD/flasher_args.json" "$(dirname "$OUT")/flasher_args.json"
# Also publish the individual parts: flash.sh writes these at their offsets so the NVS partition
# (0x9000, the saved Wi-Fi/inference settings) is left untouched. The merged image pads the gap
# between the partition table and otadata with 0xFF, which erases NVS on every flash.
mkdir -p "$(dirname "$OUT")/parts"
cp "$BUILD/bootloader/bootloader.bin" "$BUILD/partition_table/partition-table.bin" "$BUILD/ota_data_initial.bin" "$BUILD/ai-terminal.bin" "$(dirname "$OUT")/parts/"
