#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Build and run the ArduinoDS4-tinyusb on-board test sketches.

Compiles (and optionally uploads) the three tests/ sketches for the chosen
board using arduino-cli, then runs the host-side E2E latency harness
(tests/test_e2e.py) if a flashed BasicGamepad board is connected.

Usage:
  # Build all tests for ESP32-S3 (no upload)
  python3 scripts/run_tests.py \
      --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc"

  # Build + upload for RP2040/Pico, then run E2E
  python3 scripts/run_tests.py \
      --fqbn "rp2040:rp2040:rpipico:usbstack=tinyusb" \
      --upload --port /dev/ttyACM0 --e2e --uart /dev/ttyACM0

  # Same, but inside the aventools virtual environment (pinned cores, isolated
  # caches — see scripts/build.sh); CORE ∈ esp32|rp2040|samd|nrf52
  python3 scripts/run_tests.py --isolated rp2040 --e2e --uart /dev/ttyACM0

The library root is auto-detected (parent of scripts/).
"""

import argparse
import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LIB_ROOT = os.path.dirname(SCRIPT_DIR)
TESTS_DIR = os.path.join(LIB_ROOT, "tests")
BUILD_SH = os.path.join(SCRIPT_DIR, "build.sh")

ISOLATED_CORES = ["esp32", "rp2040", "samd", "nrf52"]

TEST_SKETCHES = [
    "TestBasicFunctionality",
    "TestOutputCallbacks",
    "LatencyBenchmark",
]


def run(cmd):
    print("+ " + " ".join(cmd))
    return subprocess.call(cmd)


def compile_sketch(fqbn, sketch_dir, library):
    cmd = ["arduino-cli", "compile", "--fqbn", fqbn, "--library", library, sketch_dir]
    return run(cmd)


def upload_sketch(fqbn, sketch_dir, library, port):
    cmd = ["arduino-cli", "compile", "-u", "-p", port, "--fqbn", fqbn,
           "--library", library, sketch_dir]
    return run(cmd)


def compile_isolated(core, sketch_dir, upload, port):
    # build.sh <core> <sketch_dir> [extra arduino-cli args...] appends the extra
    # args to its compile invocation, so -u/-p flow through for uploads.
    cmd = ["bash", BUILD_SH, core, sketch_dir]
    if upload:
        cmd += ["-u", "-p", port]
    return run(cmd)


def run_e2e(uart, evdev):
    cmd = [sys.executable, os.path.join(TESTS_DIR, "test_e2e.py")]
    if uart:
        cmd += ["--uart", uart]
    if evdev:
        cmd += ["--evdev", evdev]
    return run(cmd)


def main():
    ap = argparse.ArgumentParser(description="ArduinoDS4-tinyusb test runner")
    target = ap.add_mutually_exclusive_group(required=True)
    target.add_argument("--fqbn", default=None,
                        help="Fully Qualified Board Name, e.g. "
                             "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc "
                             "or rp2040:rp2040:rpipico:usbstack=tinyusb")
    target.add_argument("--isolated", metavar="CORE", choices=ISOLATED_CORES,
                        help="compile inside the aventools virtual environment "
                             "(scripts/build.sh) for CORE ∈ esp32|rp2040|samd|nrf52; "
                             "cores/libs are pinned and caches isolated")
    ap.add_argument("--upload", action="store_true", help="upload after compile")
    ap.add_argument("--port", help="serial port for upload (with --upload)")
    ap.add_argument("--e2e", action="store_true",
                    help="run tests/test_e2e.py latency harness afterwards")
    ap.add_argument("--uart", help="UART for E2E (auto-detect if omitted)")
    ap.add_argument("--evdev", help="evdev gamepad device for E2E (auto-detect)")
    args = ap.parse_args()

    if args.upload and not args.port:
        print("ERROR: --upload requires --port")
        sys.exit(2)

    failures = []
    for name in TEST_SKETCHES:
        sketch = os.path.join(TESTS_DIR, name)
        if args.isolated:
            rc = compile_isolated(args.isolated, sketch, args.upload, args.port)
        elif args.upload and args.port:
            rc = upload_sketch(args.fqbn, sketch, LIB_ROOT, args.port)
        else:
            rc = compile_sketch(args.fqbn, sketch, LIB_ROOT)
        if rc != 0:
            failures.append(name)

    if failures:
        print("\nFAILED to build: " + ", ".join(failures))
        sys.exit(1)
    print("\nAll test sketches built successfully.")

    if args.e2e:
        print("\n--- Running E2E latency harness ---")
        rc = run_e2e(args.uart, args.evdev)
        sys.exit(rc)


if __name__ == "__main__":
    main()
