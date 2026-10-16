#!/usr/bin/env bash
# Flash the merged image to a WHY2025 badge.
#   PORT=/dev/ttyACM0 ./flash.sh            (default port /dev/ttyACM0, 921600 baud)
# If esptool cannot connect, put the badge in download mode first: with the badge off,
# hold the designated download/boot key while pressing reset (see README.md), then retry.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BIN="${BIN:-$HERE/ai-terminal-merged.bin}"
[ -f "$BIN" ] || BIN="$HERE/../out/ai-terminal-merged.bin"
PORT="${PORT:-/dev/ttyACM0}"
BAUD="${BAUD:-921600}"
ESPTOOL="esptool.py"; command -v esptool.py >/dev/null 2>&1 || ESPTOOL="python -m esptool"
echo "Flashing $BIN to $PORT at $BAUD baud (chip esp32p4, offset 0x0)"
$ESPTOOL --chip esp32p4 -p "$PORT" -b "$BAUD" --before default_reset --after hard_reset \
  write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$BIN"
