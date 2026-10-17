#!/usr/bin/env bash
# Build the firmware and the merged image inside the official ESP-IDF container (no local IDF needed).
#   tools/docker_build.sh            # configure (if needed) + build + out/ai-terminal-merged.bin
#   tools/docker_build.sh fullclean  # wipe build/ and sdkconfig first (re-reads sdkconfig.defaults)
# Image: espressif/idf:v6.0.2 (multi-arch). All components are vendored, the component manager is off.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IDF_IMAGE:-espressif/idf:v6.0.2}"
if [ "${1:-}" = "fullclean" ]; then rm -rf "$HERE/build" "$HERE/sdkconfig" "$HERE/sdkconfig.old"; fi
docker run --rm -t -v "$HERE":/project -w /project \
  -e IDF_COMPONENT_MANAGER=0 -e IDF_PYTHON_CHECK_CONSTRAINTS=no -e IDF_TARGET=esp32p4 \
  "$IMAGE" bash -lc '
    set -e
    if [ ! -f build/CMakeCache.txt ]; then idf.py set-target esp32p4; fi
    idf.py build
    tools/make_merged_bin.sh
  '
