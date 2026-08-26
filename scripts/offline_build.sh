#!/usr/bin/env bash
#
# offline_build.sh - Offline fallback for `build.sh esp32`.
#
# Use when the host has no working DNS/network and `lib install` fails
# (symptom: "Download failed: ... dial tcp: lookup ... write udp ...:
# operation not permitted"). Seeds Adafruit TinyUSB 3.7.7 into the per-build
# aventools sketchbook straight from the local arduino-cli staging cache and
# skips `lib install` entirely. TinyUSB's other declared deps (NeoPixel,
# SdFat - Adafruit Fork, SPIFlash, MIDI) are tag-dependencies that nothing in
# this library #includes — they are NOT needed to compile.
#
# Only esp32 needs this: rp2040/samd/nrf52/renesas bundle TinyUSB in their cores, so
# plain `build.sh <core>` works offline once the core is in AVENV_GOLDEN.
#
# Usage:
#   AVENV_GOLDEN="$HOME/.arduino15" scripts/offline_build.sh <sketch_dir> [extra arduino-cli args...]
#
# Examples:
#   AVENV_GOLDEN="$HOME/.arduino15" scripts/offline_build.sh examples/AutoCycle -u -p /dev/ttyUSB0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/avenv.sh"

# The esp32 core must already be in AVENV_GOLDEN (seeded by a prior build.sh
# core install). This offline fallback deliberately skips `core install` and
# `lib install`, so a missing core yields a cryptic "platform not found" later
# — check up front instead.
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc"

[[ $# -ge 1 ]] || { echo "usage: $0 <sketch_dir> [arduino-cli args...]" >&2; exit 2; }
TARGET="$1"; shift
[[ -d "$TARGET" ]] || { echo "offline_build.sh: no such sketch dir: $TARGET" >&2; exit 1; }

# Local sources for the TinyUSB zip, best first.
ZIP_CANDIDATES=(
  "$HOME/.arduino15/staging/libraries/Adafruit_TinyUSB_Library-3.7.7.zip"
)
ZIP=""
for z in "${ZIP_CANDIDATES[@]}"; do
  [[ -f "$z" ]] && { ZIP="$z"; break; }
done
[[ -n "$ZIP" ]] || {
  echo "offline_build.sh: no cached Adafruit_TinyUSB_Library-3.7.7.zip found in:" >&2
  printf '  %s\n' "${ZIP_CANDIDATES[@]}" >&2
  exit 1
}

export AVENV_GOLDEN="${AVENV_GOLDEN:-$HOME/.arduino15}"
[[ -d "$AVENV_GOLDEN/packages/esp32" ]] || {
  echo "offline_build.sh: esp32 core not found in AVENV_GOLDEN ($AVENV_GOLDEN)." >&2
  echo "  Run 'build.sh esp32 <sketch>' once online to seed it, or set AVENV_GOLDEN correctly." >&2
  exit 1
}
aventools_init "ds4tinyusb-esp32-offline"

# Seed the pinned TinyUSB into the isolated sketchbook.
unzip -q "$ZIP" -d "$AVENV_USER/libraries/"

# Stage aside any per-sketch sketch.yaml whose default_profile would override
# our explicit --fqbn/--library (same dance as build.sh).
SKETCH_YAML="$TARGET/sketch.yaml"
HAD_YAML=0
if [[ -e "$SKETCH_YAML" ]]; then
  mv "$SKETCH_YAML" "$SKETCH_YAML.buildbak"
  HAD_YAML=1
fi
restore_yaml() {
  if [[ $HAD_YAML -eq 1 && -e "$SKETCH_YAML.buildbak" ]]; then
    mv "$SKETCH_YAML.buildbak" "$SKETCH_YAML"
  fi
  aventools_cleanup
}
trap restore_yaml EXIT INT TERM HUP

# Core comes from AVENV_GOLDEN (no network access needed for the compile).
arduino-cli compile --fqbn "$FQBN" --library "$PROJ_ROOT" "$TARGET" "$@"
