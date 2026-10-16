#!/usr/bin/env bash
# Source this file to get the ESP-IDF toolchain for this project.
# Usage: source env.sh
export IDF_PATH="${IDF_PATH:-/root/esp/esp-idf-v6.0.2}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/root/.espressif}"
export IDF_PYTHON_CHECK_CONSTRAINTS=no   # the constraints file host (dl.espressif.com) is not reachable here
export IDF_COMPONENT_MANAGER=0           # all components are vendored under components/ (registry not reachable)
export IDF_EXPORT_QUIET=1
. "$IDF_PATH/export.sh" >/dev/null 2>&1 || { echo "export.sh failed; run it manually to see why" >&2; return 1; }
