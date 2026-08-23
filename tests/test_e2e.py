#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
End-to-end latency test for the ArduinoDS4-tinyusb library.

Connects to a board (ESP32-S3 or RP2040/Pico) over its debug UART running
BasicGamepad.ino, sends button/stick commands, and measures the time until
corresponding USB HID events arrive via evdev on the host PC. Targets the Sony
DualShock 4 HID device (VID=054C PID=05C4).

Usage:
  sudo python3 test_e2e.py                        # auto-detect port & evdev
  sudo python3 test_e2e.py --uart /dev/ttyACM0    # explicit UART port
  sudo python3 test_e2e.py --list                 # list available devices
"""

import argparse
import glob
import select
import statistics
import sys
import time

import serial
from serial.tools import list_ports
from evdev import InputDevice, ecodes, list_devices

# DualShock 4 USB VID/PID (emitted by DS4Gamepad)
DS4_VID = 0x054C
DS4_PID = 0x05C4

# Raspberry Pi Debug Probe CDC-UART bridge (gates data transfer on DTR)
DEBUG_PROBE_VID = 0x2E8A
DEBUG_PROBE_PID = 0x000C

# Command timeout in seconds
CMD_TIMEOUT_S = 1.0


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def find_evdev():
    """Return (InputDevice, path) for the DS4 main gamepad, or (None, None).

    The DS4 exposes 3 sub-devices with the same VID/PID:
      eventN: main gamepad (sticks, buttons, hat, triggers)
      eventN: motion sensors (IMU)
      eventN: touchpad
    We prefer the main gamepad by selecting the device with ABS_HAT0X
    (d-pad) — only the main gamepad has it.
    """
    candidates = []
    for path in list_devices():
        dev = InputDevice(path)
        if dev.info.vendor == DS4_VID and dev.info.product == DS4_PID:
            caps = dev.capabilities()
            abs_codes = [c[0] if isinstance(c, tuple) else c
                         for c in caps.get(ecodes.EV_ABS, [])]
            candidates.append((dev, path, abs_codes))
    if not candidates:
        return None, None
    for dev, path, abs_codes in candidates:
        if ecodes.ABS_HAT0X in abs_codes:
            return dev, path
    candidates.sort(key=lambda c: len(c[2]), reverse=True)
    return candidates[0][0], candidates[0][1]


def find_uart(pattern="/dev/ttyUSB*,/dev/ttyACM*"):
    """Find first serial port matching any comma-separated glob pattern."""
    for pat in pattern.split(","):
        ports = sorted(glob.glob(pat.strip()))
        if ports:
            return ports[0]
    return None


def is_debug_probe(port_name):
    """True if port_name is the Raspberry Pi Debug Probe's CDC-UART bridge."""
    for p in list_ports.comports():
        if p.device == port_name:
            return (p.vid, p.pid) == (DEBUG_PROBE_VID, DEBUG_PROBE_PID)
    return False


def send_cmd(uart, cmd: str):
    """Send a KEY=VALUE line to the BasicGamepad UART parser."""
    uart.write(cmd.encode() + b"\n")
    uart.flush()


def wait_for_event(dev, event_type, event_codes, timeout=CMD_TIMEOUT_S, value=None):
    """Block until an evdev event matches, or timeout. Returns (elapsed_s, value)."""
    if isinstance(event_codes, int):
        event_codes = [event_codes]
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        r, _, _ = select.select([dev.fd], [], [], 0.010)
        if r:
            try:
                events = dev.read()
            except OSError:
                return timeout, None
            for event in events:
                if event.type == event_type and event.code in event_codes:
                    if value is None or event.value == value:
                        return time.monotonic() - start, event.value
    return timeout, None


# ---------------------------------------------------------------------------
# DS4 button → evdev mapping
# ---------------------------------------------------------------------------
DS4_CMD_MAP = {
    "BTN_CROSS":    (0x130, 1, 0),
    "BTN_CIRCLE":   (0x131, 1, 0),
    "BTN_TRIANGLE": (0x133, 1, 0),
    "BTN_SQUARE":   (0x134, 1, 0),
    "BTN_L1":       (0x136, 1, 0),
    "BTN_R1":       (0x137, 1, 0),
    "BTN_L2":       (0x138, 1, 0),
    "BTN_R2":       (0x139, 1, 0),
    "BTN_SHARE":    (0x13A, 1, 0),
    "BTN_OPTIONS":  (0x13B, 1, 0),
    "BTN_L3":       (0x13C, 1, 0),
    "BTN_R3":       (0x13D, 1, 0),
    "BTN_PS":       (0x13E, 1, 0),
    "BTN_TOUCHPAD": (0x14A, 1, 0),
}


# ---------------------------------------------------------------------------
# Test scenarios
# ---------------------------------------------------------------------------

def test_button_latency(uart, dev, btn_name, btn_code, num_samples=200):
    latencies = []
    for _ in range(num_samples):
        t0 = time.monotonic()
        send_cmd(uart, f"{btn_name}=1")
        dt, val = wait_for_event(dev, ecodes.EV_KEY, btn_code, value=1)
        latencies.append(dt * 1000)
        if dt >= CMD_TIMEOUT_S:
            break
        send_cmd(uart, f"{btn_name}=0")
        wait_for_event(dev, ecodes.EV_KEY, btn_code, timeout=0.5, value=0)
    return latencies


def test_stick_latency(uart, dev, cmd_name, axis_code, num_samples=200):
    latencies = []
    for i in range(num_samples):
        val = -127 if i % 2 == 0 else 127
        send_cmd(uart, f"{cmd_name}={val}")
        dt, _ = wait_for_event(dev, ecodes.EV_ABS, axis_code)
        latencies.append(dt * 1000)
        if dt >= CMD_TIMEOUT_S:
            break
    return latencies


def test_dpad_latency(uart, dev, num_samples=200):
    hat_codes = [ecodes.ABS_HAT0X, ecodes.ABS_HAT0Y]
    latencies = []
    for i in range(num_samples):
        dpad = i % 9
        send_cmd(uart, f"HAT={dpad}")
        dt, val = wait_for_event(dev, ecodes.EV_ABS, hat_codes)
        latencies.append(dt * 1000)
        if dt >= CMD_TIMEOUT_S:
            break
    return latencies


def test_trigger_latency(uart, dev, trig_name, axis_code, num_samples=200):
    latencies = []
    for i in range(num_samples):
        val = 255 if i % 2 == 0 else 0
        send_cmd(uart, f"{trig_name}={val}")
        dt, _ = wait_for_event(dev, ecodes.EV_ABS, axis_code)
        latencies.append(dt * 1000)
        if dt >= CMD_TIMEOUT_S:
            break
    return latencies


def test_touch_latency(uart, dev, num_samples=200):
    latencies = []
    for _ in range(num_samples):
        send_cmd(uart, "TP1=960,471")
        dt, _ = wait_for_event(dev, ecodes.EV_ABS, [ecodes.ABS_MT_POSITION_X,
                                                     ecodes.ABS_MT_POSITION_Y])
        latencies.append(dt * 1000)
        if dt >= CMD_TIMEOUT_S:
            break
        send_cmd(uart, "TP1=")
        time.sleep(0.01)
    return latencies


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def report(name, latencies_ms):
    if not latencies_ms:
        print(f"  {name}: FAIL (no data)")
        return None
    latencies_ms.sort()
    n = len(latencies_ms)
    avg = statistics.mean(latencies_ms)
    med = statistics.median(latencies_ms)
    p99 = latencies_ms[int(n * 0.99)]
    mx = max(latencies_ms)
    mn = min(latencies_ms)
    jitter = statistics.stdev(latencies_ms) if n > 1 else 0
    print(f"  {name}:")
    print(f"    samples={n}  min={mn:.3f}ms  max={mx:.3f}ms  avg={avg:.3f}ms")
    print(f"    median={med:.3f}ms  p99={p99:.3f}ms  jitter(σ)={jitter:.3f}ms")
    return {"name": name, "min": mn, "max": mx, "avg": avg,
            "median": med, "p99": p99, "jitter": jitter, "n": n}


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="ArduinoDS4-tinyusb E2E Latency Test")
    parser.add_argument("--uart", help="UART port (e.g. /dev/ttyACM0 or /dev/ttyUSB0)")
    parser.add_argument("--baud", type=int, default=115200, help="UART baud rate")
    parser.add_argument("--samples", type=int, default=200, help="samples per test")
    parser.add_argument("--list", action="store_true", help="list available devices")
    parser.add_argument("--dtr", choices=["auto", "keep", "drop"], default="auto",
                        help="DTR/RTS handling for the telemetry UART: 'auto' "
                             "keeps DTR asserted on a Raspberry Pi Debug Probe "
                             "(its CDC bridge gates data on DTR and wedges "
                             "write() otherwise) and drops DTR/RTS elsewhere "
                             "(ESP32-S3 dev boards wire them to auto-reset)")
    args = parser.parse_args()

    if args.list:
        print("Input devices:")
        for path in list_devices():
            dev = InputDevice(path)
            tags = []
            if dev.info.vendor == DS4_VID and dev.info.product == DS4_PID:
                tags.append(" *** DS4 gamepad ***")
            print(f"  {path}  vendor={dev.info.vendor:04x} product={dev.info.product:04x}{''.join(tags)}")
        return

    evdev_dev, evdev_path = find_evdev()
    if not evdev_dev:
        print("ERROR: DS4 evdev device not found. Is the board plugged in?")
        print("Use --list to see all input devices.")
        sys.exit(1)
    print(f"evdev device: {evdev_path}  ({evdev_dev.name})")

    uart_port = args.uart or find_uart()
    if not uart_port:
        print("ERROR: UART port not found. Specify with --uart.")
        sys.exit(1)
    print(f"UART port:    {uart_port} @ {args.baud} baud")

    # write_timeout keeps a wedged adapter from hanging write() forever.
    uart = serial.Serial(uart_port, args.baud, timeout=1, write_timeout=5)

    dtr_policy = args.dtr
    if dtr_policy == "auto":
        dtr_policy = "keep" if is_debug_probe(uart_port) else "drop"
    if dtr_policy == "drop":
        uart.dtr = False
        uart.rts = False
    print(f"DTR/RTS policy: {dtr_policy}")

    time.sleep(2)

    evdev_dev, evdev_path = find_evdev()
    if not evdev_dev:
        print("ERROR: DS4 evdev device lost after UART open")
        sys.exit(1)
    dev = evdev_dev
    print(f"evdev device: {evdev_path} ({dev.name})")

    uart.reset_input_buffer()

    # Warm-up: the port open resets the board, and send() refuses reports
    # during the post-mount settle window (~100ms after USB mount). Poll
    # LX=0 until the board confirms SENT=1 so no first-command is lost.
    # NOTE: must be a KEY=VALUE command — the sketch parser ignores plain
    # words like RELEASE (no '=' -> no exec() -> no reply).
    deadline = time.monotonic() + 15
    warmed_up = False
    while time.monotonic() < deadline:
        uart.write(b"LX=0\n")
        uart.flush()
        line = uart.readline().decode("utf-8", errors="replace").strip()
        if line == "SENT=1":
            warmed_up = True
            break
        time.sleep(0.2)
    if not warmed_up:
        print("ERROR: board never confirmed SENT=1 during warm-up")
        sys.exit(1)
    print("Warm-up OK")

    caps = dev.capabilities()
    btn_codes = caps.get(ecodes.EV_KEY, [])
    abs_raw = caps.get(ecodes.EV_ABS, [])
    abs_codes = [c[0] if isinstance(c, tuple) else c for c in abs_raw]
    print(f"EV_KEY codes: {[hex(c) for c in btn_codes]}")
    print(f"EV_ABS codes: {[hex(c) for c in abs_codes]}")

    results = []

    btn_name = None
    btn_key = None
    for name, code in DS4_CMD_MAP.items():
        if code[0] in btn_codes:
            btn_name = name
            btn_key = code[0]
            break

    lx_code = ecodes.ABS_X if ecodes.ABS_X in abs_codes else None
    ly_code = ecodes.ABS_Y if ecodes.ABS_Y in abs_codes else None
    l2_code = ecodes.ABS_Z if ecodes.ABS_Z in abs_codes else None
    r2_code = ecodes.ABS_RZ if ecodes.ABS_RZ in abs_codes else None
    hat_code = ecodes.ABS_HAT0X if ecodes.ABS_HAT0X in abs_codes else None

    print("\n=== Button Latency ===")
    if btn_key is not None:
        lat = test_button_latency(uart, dev, btn_name, btn_key, num_samples=args.samples)
        r = report(btn_name, lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no button codes found)")

    print("\n=== Left Stick Latency ===")
    if lx_code is not None:
        lat = test_stick_latency(uart, dev, "LX", lx_code, num_samples=args.samples)
        r = report("LX", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_X found)")
    if ly_code is not None:
        lat = test_stick_latency(uart, dev, "LY", ly_code, num_samples=args.samples)
        r = report("LY", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_Y found)")

    print("\n=== Trigger Latency ===")
    if l2_code is not None:
        lat = test_trigger_latency(uart, dev, "TRIG_L", l2_code, num_samples=args.samples)
        r = report("TRIG_L", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_Z found)")
    if r2_code is not None:
        lat = test_trigger_latency(uart, dev, "TRIG_R", r2_code, num_samples=args.samples)
        r = report("TRIG_R", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_RZ found)")

    print("\n=== D-Pad Latency ===")
    if hat_code is not None:
        lat = test_dpad_latency(uart, dev, num_samples=args.samples)
        r = report("DPAD", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_HAT0X found)")

    print("\n=== Touch Latency ===")
    if ecodes.ABS_MT_POSITION_X in abs_codes:
        lat = test_touch_latency(uart, dev, num_samples=min(args.samples, 50))
        r = report("TOUCH", lat)
        if r:
            results.append(r)
    else:
        print("  SKIP (no ABS_MT_POSITION_X found)")

    print("\n" + "=" * 50)
    print("SUMMARY")
    print("=" * 50)
    all_p99 = [r["p99"] for r in results]
    all_max = [r["max"] for r in results]
    worst_p99 = max(all_p99) if all_p99 else float("inf")
    worst_max = max(all_max) if all_max else float("inf")
    passed = worst_p99 < 10.0
    print(f"  Worst 99th percentile: {worst_p99:.3f}ms")
    print(f"  Worst max latency:     {worst_max:.3f}ms")
    print(f"  Threshold:            <10ms p99")
    print(f"  RESULT: {'PASS' if passed else 'FAIL'}")

    dev.close()
    uart.close()
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
