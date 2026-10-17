#!/usr/bin/env bash
# Flash ai-terminal to a WHY2025 badge.
#   ./flash.sh                               (auto-detects the port on macOS)
#   PORT=/dev/ttyUSB0 ./flash.sh             (Linux)
#   ERASE_SETTINGS=1 ./flash.sh              (also wipe the saved Wi-Fi / inference settings)
# By default the bootloader, partition table, otadata and app are written at their own offsets
# (from out/parts/, offsets from flasher_args.json) so the NVS partition keeps your settings.
# The merged image (out/ai-terminal-merged.bin, offset 0x0) is only used with ERASE_SETTINGS=1
# or when out/parts/ is missing; it pads the NVS area with 0xFF and therefore erases it.
# Use the SIDE USB-C socket of the badge: it is a CH340 USB-UART wired to the P4 UART
# (macOS needs the WCH CH34x driver -> /dev/cu.wchusbserialNN, Linux -> /dev/ttyUSB0).
# The bottom USB-C is charging / P4 native USB and is not used for flashing.
# Default 460800 baud with --no-stub, reported reliable through the CH340 (BAUD= / STUB=1 to change).
# If esptool cannot connect: hold BOOT, press and release RESET, release BOOT, then retry.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/ai-terminal-merged.bin}"
[ -f "$BIN" ] || BIN="$HERE/../out/ai-terminal-merged.bin"
BAUD="${BAUD:-460800}"
STUBFLAG="--no-stub"; [ "${STUB:-0}" = "1" ] && STUBFLAG=""

if [ -z "${PORT:-}" ]; then
  if [ "$(uname)" = "Darwin" ]; then
    # shellcheck disable=SC2012
    CANDIDATES=$(ls /dev/cu.wchusbserial* /dev/cu.usbserial* /dev/cu.usbmodem* 2>/dev/null || true)
    COUNT=$(printf '%s\n' "$CANDIDATES" | grep -c . || true)
    if [ "$COUNT" -eq 1 ]; then PORT="$CANDIDATES"
    elif [ "$COUNT" -eq 0 ]; then echo "No /dev/cu.wchusbserial*/usbserial*/usbmodem* device found. Plug the badge SIDE USB-C in, install the WCH CH34x driver (see README), or set PORT=." >&2; exit 1
    else echo "Several serial devices found, set PORT= to one of:" >&2; printf '  %s\n' $CANDIDATES >&2; exit 1
    fi
  else
    PORT=/dev/ttyUSB0
  fi
fi

if [ -z "${ESPTOOL:-}" ]; then
  if command -v esptool >/dev/null 2>&1; then ESPTOOL="esptool"
  elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL="esptool.py"
  else ESPTOOL="python3 -m esptool"
  fi
fi

PARTS="$HERE/parts"; ARGS_JSON="$HERE/flasher_args.json"
if [ "${ERASE_SETTINGS:-0}" != "1" ] && [ -d "$PARTS" ] && [ -f "$ARGS_JSON" ]; then
  # <offset> <file> pairs from flasher_args.json, files taken from out/parts/ (NVS untouched)
  PAIRS=$(python3 - "$ARGS_JSON" "$PARTS" <<'PY'
import json, os, sys
j = json.load(open(sys.argv[1])); parts = sys.argv[2]
out = []
for off, f in sorted(j["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    out.append(off); out.append(os.path.join(parts, os.path.basename(f)))
print(" ".join(out))
PY
)
  echo "Flashing parts (settings preserved) to $PORT at $BAUD baud using: $ESPTOOL"
  # shellcheck disable=SC2086
  $ESPTOOL --chip esp32p4 -p "$PORT" -b "$BAUD" $STUBFLAG --before default_reset --after hard_reset \
    write_flash --flash_mode keep --flash_freq keep --flash_size keep $PAIRS
else
  echo "Flashing merged image $BIN (ERASES saved settings) to $PORT at $BAUD baud using: $ESPTOOL"
  $ESPTOOL --chip esp32p4 -p "$PORT" -b "$BAUD" $STUBFLAG --before default_reset --after hard_reset \
    write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$BIN"
fi
