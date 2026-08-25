#!/usr/bin/env bash
#
# build.sh - Isolated, reproducible, multi-core build for the ArduinoDS4-tinyusb
# unified library. Supports ESP32-S3, RP2040/Pico, SAMD and nRF52840 from one
# code base.
#
# Wraps the aventools virtual environment (scripts/avenv.sh) so every compile
# gets its own cores/libraries/downloads/build-cache.
#
# Why not `--profile`? arduino-cli refuses `--library` together with
# `--profile`, and a `--profile` build does not scan user/libraries for the
# local library. So for this library project we pin explicitly via `core install`
# (and, for ESP32-S3 only, `lib install` of Adafruit TinyUSB) — versions kept in
# sync with sketch.yaml.
#
# Usage:
#   scripts/build.sh <core> <sketch_dir> [extra arduino-cli args...]
#     core ∈ esp32 | rp2040 | samd | nrf52
#
# Examples:
#   scripts/build.sh esp32 examples/BasicGamepad
#   scripts/build.sh rp2040 tests/TestBasicFunctionality
#   scripts/build.sh nrf52  examples/AutoCycle --verbose
#
# Build every example on every core:
#   for c in esp32 rp2040 samd nrf52; do
#     for ex in examples/*/; do scripts/build.sh "$c" "${ex%/}"; done
#   done
#
# Speed up CI with a shared, read-only toolchain cache:
#   AVENV_GOLDEN="$HOME/.arduino15" scripts/build.sh esp32 examples/BasicGamepad
# (this repo is a library, so `aventools prime` — which compiles the target as a
#  sketch — does not apply; the first build.sh run populates the golden cache
#  via core/lib install.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/avenv.sh"

usage() {
  echo "usage: build.sh <core> <sketch_dir> [arduino-cli args...]" >&2
  echo "  core ∈ esp32 | rp2040 | samd | nrf52" >&2
  exit 2
}
[[ $# -ge 2 ]] || usage
CORE_NAME="$1"; shift
if [[ ! -d "$1" ]]; then echo "build.sh: no such sketch dir: $1" >&2; exit 1; fi
TARGET="$1"; shift

# Pinned versions — keep in sync with sketch.yaml.
case "$CORE_NAME" in
  esp32)
    CORE="esp32:esp32@3.3.11"
    FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc"
    NEED_TINYUSB=1
    ;;
  rp2040)
    CORE="rp2040:rp2040@6.0.0"
    FQBN="rp2040:rp2040:rpipico:usbstack=tinyusb"
    NEED_TINYUSB=0
    ;;
  samd)
    CORE="adafruit:samd@1.7.17"
    FQBN="adafruit:samd:adafruit_feather_m0:usbstack=tinyusb"
    NEED_TINYUSB=0
    ;;
  nrf52)
    CORE="adafruit:nrf52@1.7.0"
    FQBN="adafruit:nrf52:feather52840:softdevice=s140v6,debug=l0,debug_output=serial"
    NEED_TINYUSB=0
    ;;
  *)
    echo "build.sh: unknown core '$CORE_NAME'" >&2
    usage
    ;;
esac

aventools_init "ds4tinyusb-$CORE_NAME"

# Serialize installs when they mutate the shared golden cache: parallel builds
# populating AVENV_GOLDEN on first use must not race (per aventools guidance).
# lib installs land in the per-build user dir, but locking them too is cheap and
# keeps all arduino-cli mutations serialized under concurrency.
av_install() {
  if [[ -n "$AVENV_GOLDEN" && -d "$AVENV_GOLDEN" ]]; then
    flock "$AVENV_GOLDEN/.install.lock" arduino-cli "$@"
  else
    arduino-cli "$@"
  fi
}

# Pin the core into the isolated env (reused from AVENV_GOLDEN if set; otherwise
# downloaded once per build root).
av_install core install "$CORE"

# ESP32-S3 does not bundle TinyUSB — install the separate library (rp2040/samd/
# nrf52 bundle it in their cores, so no separate install there).
if [[ "$NEED_TINYUSB" -eq 1 ]]; then
  av_install lib install "Adafruit TinyUSB Library@3.7.7"
fi

# Some sketch dirs (e.g. tests/) ship a sketch.yaml with a default_profile.
# arduino-cli auto-applies that profile, overriding our --fqbn / --library, so
# stage it aside for the duration of the compile and restore it on exit.
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

# --library adds the local ArduinoDS4-tinyusb library.
arduino-cli compile --fqbn "$FQBN" --library "$PROJ_ROOT" "$TARGET" "$@"
