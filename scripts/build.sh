#!/usr/bin/env bash
# Build the merged official-base Marauder firmware for a given board target.
#
# Usage:  scripts/build.sh <BOARD> [BUILD_DIR]
#   BOARD = PANCAKE | V8 | MINI_V3 | WS_C5_35
#
# Requires arduino-cli with the ESP32 Arduino core 3.3.4 installed.
# Set ARDUINO_CLI_BIN to the arduino-cli path if it is not on PATH.
set -euo pipefail

BOARD="${1:-PANCAKE}"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
SKETCH_DIR="${PROJECT_DIR}/firmware/Marauder"
LIBRARY_DIR="${PROJECT_DIR}/libraries"
BUILD_DIR="${2:-${PROJECT_DIR}/build/${BOARD}}"
CACHE_DIR="${BUILD_DIR}/cache"
EXPECTED_CORE_VERSION="3.3.4"

# --- board -> FQBN / define / TFT User_Setup -------------------------------
case "${BOARD}" in
  PANCAKE)  FLAG="MARAUDER_PANCAKE"; TFT="User_Setup_marauder_pancake.h";
            FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=enabled" ;;
  V8)       FLAG="MARAUDER_V8";      TFT="User_Setup_marauder_v8.h";
            FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=enabled" ;;
  WS_C5_35) FLAG="MARAUDER_WS_C5_35"; TFT="User_Setup_ws_c5_35.h";
            FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=enabled" ;;
  MINI_V3)  FLAG="MARAUDER_MINI_V3"; TFT="User_Setup_marauder_mini_v3.h";
            FQBN="esp32:esp32:esp32c5:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=enabled" ;;
  *) echo "Unknown board '${BOARD}'. Use PANCAKE|V8|MINI_V3|WS_C5_35." >&2; exit 2 ;;
esac

# --- locate arduino-cli -----------------------------------------------------
if [[ -n "${ARDUINO_CLI_BIN:-}" ]]; then ARDUINO_CLI="${ARDUINO_CLI_BIN}";
elif command -v arduino-cli >/dev/null 2>&1; then ARDUINO_CLI="$(command -v arduino-cli)";
else echo "arduino-cli not found. Set ARDUINO_CLI_BIN." >&2; exit 1; fi

CORE_VER="$("${ARDUINO_CLI}" core list 2>/dev/null | awk '$1=="esp32:esp32"{print $2}')"
if [[ "${CORE_VER}" != "${EXPECTED_CORE_VERSION}" ]]; then
  echo "ESP32 core ${EXPECTED_CORE_VERSION} required; found ${CORE_VER:-none}." >&2; exit 1
fi

# --- stage libraries with exactly one active TFT User_Setup -----------------
mkdir -p "${BUILD_DIR}" "${CACHE_DIR}"
STAGE_LIBS="${BUILD_DIR}/libs"
rm -rf "${STAGE_LIBS}"; mkdir -p "${STAGE_LIBS}"
cp -r "${LIBRARY_DIR}/." "${STAGE_LIBS}/"
SEL="${STAGE_LIBS}/TFT_eSPI/User_Setup_Select.h"
# comment out every marauder User_Setup include, then enable the one for BOARD
sed -i -E 's@^[[:space:]]*#include <(User_Setup_[A-Za-z0-9_]+\.h)>@//#include <\1>@' "${SEL}"
sed -i "s@^//#include <${TFT}>@#include <${TFT}>@" "${SEL}"
echo "Active TFT setup(s):"; grep -nE "^#include <User_Setup_" "${SEL}" || true

# --- compile ----------------------------------------------------------------
TZ=UTC "${ARDUINO_CLI}" compile \
  --warnings none \
  --fqbn "${FQBN}" \
  --libraries "${STAGE_LIBS}" \
  --build-path "${CACHE_DIR}" \
  --build-property "compiler.cpp.extra_flags=-D${FLAG}" \
  --build-property "compiler.c.extra_flags=-D${FLAG}" \
  --build-property "compiler.c.elf.extra_flags=-Wl,-zmuldefs" \
  --output-dir "${BUILD_DIR}" \
  "${SKETCH_DIR}"

echo "Build complete for ${BOARD}: ${BUILD_DIR}/$(basename "${SKETCH_DIR}").ino.bin"
