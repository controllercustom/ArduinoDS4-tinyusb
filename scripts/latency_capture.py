#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Capture an AutoCycle pcap from a Linux usbmon device and validate it.

This pairs with examples/AutoCycle/AutoCycle.ino: flash AutoCycle to the board,
then run this script to capture the DS4 HID stream and verify every P0..P11
phase, the sequential 6-bit counter, and the constant 0x1B battery byte.

Usage:
  sudo python3 scripts/latency_capture.py --out /tmp/ds4.pcap --duration 60
  sudo python3 scripts/latency_capture.py --out /tmp/ds4.pcap --bus usbmon3

The ESP32-S3 dev module's USB-UART bridge (/dev/ttyUSB0) and the Raspberry Pi
Pico Debug Probe's UART bridge (/dev/ttyACM0) carry telemetry only — capture
the bus where the board enumerates as 054C:05C4 (not the serial bridge).
"""

import argparse
import os
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LIB_ROOT = os.path.dirname(SCRIPT_DIR)
VALIDATOR = os.path.join(LIB_ROOT, "tests", "validate_autocycle_pcap.py")

DS4_VIDPID = "054c:05c4"


def find_bus():
    try:
        out = subprocess.check_output(["lsusb"]).decode("utf-8", "ignore")
    except (OSError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines():
        if DS4_VIDPID in line.lower():
            num = line.split()[1]
            return "usbmon" + num.lstrip("0") or "usbmon0"
    return None


def main():
    ap = argparse.ArgumentParser(description="Capture + validate AutoCycle pcap")
    ap.add_argument("--out", default="/tmp/ds4_capture.pcap", help="output pcap path")
    ap.add_argument("--bus", help="usbmon device (auto-detected if omitted)")
    ap.add_argument("--duration", type=int, default=60, help="capture seconds")
    ap.add_argument("--no-validate", action="store_true", help="skip validation step")
    args = ap.parse_args()

    bus = args.bus or find_bus()
    if not bus:
        print("ERROR: could not auto-detect DS4 bus. Pass --bus usbmonN.")
        print("Hint: lsusb | grep 054c")
        sys.exit(2)

    print("Capturing %s -> %s for %ds (Ctrl-C to stop early)" % (bus, args.out, args.duration))
    try:
        subprocess.call(["sudo", "timeout", str(args.duration),
                         "tcpdump", "-i", bus, "-w", args.out])
    except KeyboardInterrupt:
        pass

    if args.no_validate:
        print("Saved %s (validation skipped)." % args.out)
        return

    if not os.path.exists(VALIDATOR):
        print("ERROR: validator not found at %s" % VALIDATOR)
        sys.exit(2)

    print("\nValidating capture...")
    rc = subprocess.call([sys.executable, VALIDATOR, args.out])
    sys.exit(rc)


if __name__ == "__main__":
    main()
