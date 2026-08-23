#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Capture DS4 HID traffic from a Linux usbmon device for pcap validation.
#
# Usage:
#   sudo ./capture_autocycle.sh <output.pcap> [usbmonN]
#   sudo DURATION=30 ./capture_autocycle.sh /tmp/ds4.pcap
#
# Finding the right bus:
#   lsusb | grep 054c
#   -> the board enumerates as "Wireless Controller" (VID 054C, PID 05C4).
#   Use the usbmon device for that bus number (e.g. usbmon3).
#
# NOTE on debug UARTs (these are NOT the HID capture target):
#   * An ESP32-S3 dev module exposes a USB-UART bridge (usually /dev/ttyUSB0)
#     for DS4_DEBUG_SERIAL telemetry — separate from its main USB-C that
#     enumerates the DS4 HID device.
#   * A Raspberry Pi Pico (or its Debug Probe) exposes a UART bridge
#     (/dev/ttyACM0) for telemetry — separate from the Pico's USB that
#     enumerates the DS4 HID device.
# Capture the bus where the board appears as 054C:05C4, not the serial bridge.
#
# After capture, validate with:
#   python3 tests/validate_autocycle_pcap.py <output.pcap>

set -e

OUT="${1:-/tmp/ds4_capture.pcap}"
BUS="$2"
DURATION="${DURATION:-20}"

if [ -z "$OUT" ]; then
  echo "usage: $0 <output.pcap> [usbmonN]" >&2
  exit 2
fi

if [ -z "$BUS" ]; then
  LINE=$(lsusb | grep -i "054c:05c4" | head -1 || true)
  if [ -z "$LINE" ]; then
    echo "Could not auto-detect DS4 bus. Pass usbmonN explicitly." >&2
    echo "Available usbmon devices:" >&2
    ls -1 /dev/usbmon* 2>/dev/null >&2 || true
    exit 3
  fi
  NUM=$(echo "$LINE" | awk '{print $2}' | sed 's/^0*//')
  BUS="usbmon${NUM}"
fi

echo "Capturing from ${BUS} -> ${OUT} for ${DURATION}s (Ctrl-C to stop early)"
sudo timeout "${DURATION}" tcpdump -i "${BUS}" -w "${OUT}"

echo
echo "Capture saved to ${OUT}"
echo "Validate with:"
echo "  python3 tests/validate_autocycle_pcap.py ${OUT}"
