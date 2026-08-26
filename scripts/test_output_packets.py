#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inject DS4 output report 0x05 (rumble + LED/RGB) packets via libusb and verify
DS4Gamepad callback telemetry over the debug UART.

Usage:
    sudo python3 scripts/test_output_packets.py --uart /dev/ttyACM0

Requires pyusb and pyserial. The board must be running
tests/TestOutputCallbacks (which prints CB_RUMBLE:<L>,<R>, CB_LED:<red> and
CB_LEDRGB:<r>,<g>,<b> telemetry on the DS4_DEBUG_SERIAL UART — Serial0 on
ESP32-S3, Serial1 on RP2040/Pico).

The UART differs by board: an ESP32-S3 dev module exposes its USB-UART bridge
as /dev/ttyUSB0; a Raspberry Pi Pico (incl. the Debug Probe) exposes its UART
as /dev/ttyACM0. The script auto-detects among those if --uart is omitted.

Report layout sent on EP1 interrupt OUT (32 bytes, Report ID included):
    [0]=0x05 Report ID
    [1]=valid_flags [2]=valid_flags1 [3]=rsvd
    [4]=motor_right [5]=motor_left [6]=lightbar_red
    [7]=lightbar_green [8]=lightbar_blue
This matches DS4Gamepad::_setFeature(), which normalizes the interrupt-OUT path
(report_id=0, ID byte inside buffer) and fires onRumble(left, right),
onLed(red), and onLedColor({r,g,b}).
"""

import argparse
import glob
import select
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    print("ERROR: pyusb required. Install with: sudo apt install python3-usb")
    sys.exit(1)

try:
    import serial
except ImportError:
    print("ERROR: pyserial required. Install with: pip3 install pyserial")
    sys.exit(1)


DS4_VID = 0x054C
DS4_PID = 0x05C4

# Interrupt OUT endpoint address on Adafruit-TinyUSB backends. Set to None
# automatically in find_device() when the active configuration has no OUT
# endpoint (Renesas RA4M1 build = IN-only, control SET_REPORT transport).
OUT_ENDPOINT = 0x01

# Constant LED red used on rumble-only packets so CB_LED lines never collide
# with the LED test's distinct red values (0, 64, 128, 255).
RUMBLE_LED_RED = 17


def find_device():
    dev = usb.core.find(idVendor=DS4_VID, idProduct=DS4_PID)
    if dev is None:
        print("ERROR: No DS4Gamepad device found (VID=0x%04X PID=0x%04X)" % (DS4_VID, DS4_PID))
        sys.exit(1)
    return dev


def find_uart(explicit):
    if explicit:
        return explicit
    for cand in ("/dev/ttyACM0", "/dev/ttyUSB0"):
        if glob.glob(cand):
            return cand
    return None


def detach_kernel_driver(dev):
    try:
        if dev.is_kernel_driver_active(0):
            print("  Detaching kernel driver from interface 0")
            dev.detach_kernel_driver(0)
    except (usb.core.USBError, NotImplementedError):
        pass


def detect_out_endpoint(dev):
    """None when the active config has no interrupt OUT endpoint."""
    global OUT_ENDPOINT
    try:
        cfg = dev.get_active_configuration()
    except usb.core.USBError:
        OUT_ENDPOINT = None
        return
    for intf in cfg:
        for ep in intf:
            if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_OUT:
                OUT_ENDPOINT = ep.bEndpointAddress
                return
    OUT_ENDPOINT = None
    print("  No interrupt OUT endpoint - using control SET_REPORT (DS4-v1 transport)")


def make_pkt(motor_right: int, motor_left: int, red: int,
             green: int = 0, blue: int = 0) -> bytes:
    """Build a 32-byte DS4 output report 0x05.

    The Report ID occupies buffer[0] (TinyUSB strips it before the payload
    callback), so the payload offsets are shifted by one: motor_right@4,
    motor_left@5, lightbar_red@6.
    """
    pkt = bytearray(32)
    pkt[0] = 0x05          # Report ID
    pkt[1] = 0x03          # valid_flags (matches Linux hid-playstation)
    pkt[2] = 0x00          # valid_flags1
    pkt[3] = 0x00          # reserved
    pkt[4] = motor_right   # payload[3] -> motor_right
    pkt[5] = motor_left    # payload[4] -> motor_left
    pkt[6] = red           # payload[5] -> lightbar_red
    pkt[7] = green         # payload[6] -> lightbar_green
    pkt[8] = blue          # payload[7] -> lightbar_blue
    return bytes(pkt)


def send_pkt(dev, pkt) -> bool:
    # Platforms whose HID interface has an interrupt OUT endpoint (all
    # Adafruit-TinyUSB backends) take the dev.write() path. The Renesas RA4M1
    # build is interface-0 IN-only (authentic DS4-v1 shape), so output reports
    # go over control SET_REPORT (bmRequestType 0x21, bRequest 0x09,
    # wValue = 0x0300 | report_id) — the same transport the Linux
    # hid-playstation driver uses for such hardware.
    try:
        if OUT_ENDPOINT is None:
            # wValue = (report_type << 8) | report_id; OUTPUT = 0x02
            dev.ctrl_transfer(0x21, 0x09, 0x0200 | pkt[0], 0, pkt)
            return True
        n = dev.write(OUT_ENDPOINT, pkt)
        return n == len(pkt)
    except usb.core.USBError as e:
        print("  USB write error: %s" % e)
        return False


# Read windows are deliberately generous (3 s): the Pico <-> Debug Probe UART
# link occasionally garbles or delays a short burst, and a missed CB_* tag
# otherwise reads as a false FAIL. Cost is ~15 s across the whole suite.
def read_uart_lines(ser, duration=1.0):
    """Read all available UART lines within a time window."""
    lines = []
    deadline = time.time() + duration
    while time.time() < deadline:
        remaining = max(0.05, min(0.2, deadline - time.time()))
        try:
            if ser.in_waiting > 0 or select.select([ser], [], [], remaining)[0]:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                lines.append(line)
        except Exception:
            break
    return lines


def wait_for_keyword(ser, keyword, timeout=15.0):
    """Wait until a UART line contains the given keyword."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = max(0.1, min(0.5, deadline - time.time()))
        try:
            if ser.in_waiting > 0 or select.select([ser], [], [], remaining)[0]:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                if keyword in line:
                    return True
        except Exception:
            break
    return False


def find_line_tag(lines, tag):
    """Check if any of the read lines contains the given tag."""
    for l in lines:
        if tag in l:
            return l
    return None


def test_rumble(dev, ser) -> bool:
    print("\n--- RUMBLE TEST ---")
    passed = True

    # (left_motor, right_motor) — note packet stores right first.
    tests = [
        (128, 0),
        (0, 255),
        (64, 192),
        (255, 255),
        (0, 0),
    ]

    for left, right in tests:
        ok = send_pkt(dev, make_pkt(motor_right=right, motor_left=left,
                                    red=RUMBLE_LED_RED))
        if not ok:
            print("  FAIL: could not write rumble packet L=%d R=%d" % (left, right))
            passed = False
            continue

        lines = read_uart_lines(ser, duration=3.0)
        expected_tag = "CB_RUMBLE:%d,%d" % (left, right)
        matched_line = find_line_tag(lines, expected_tag)
        if matched_line:
            print("  PASS: L=%3d R=%3d -> got '%s'" % (left, right, matched_line))
        else:
            summary = "; ".join(lines[:5]) if lines else "(no UART output)"
            print("  FAIL: L=%3d R=%3d -> expected '%s', got: %s" % (left, right, expected_tag, summary))
            passed = False

    return passed


def test_led(dev, ser) -> bool:
    print("\n--- LED TEST ---")
    passed = True

    for red in (0, 64, 128, 255):
        ok = send_pkt(dev, make_pkt(motor_right=0, motor_left=0, red=red))
        if not ok:
            print("  FAIL: could not write LED packet red=%d" % red)
            passed = False
            continue

        lines = read_uart_lines(ser, duration=3.0)
        expected_tag = "CB_LED:%d" % red
        matched_line = find_line_tag(lines, expected_tag)
        if matched_line:
            print("  PASS: red %3d -> got '%s'" % (red, matched_line))
        else:
            summary = "; ".join(lines[:5]) if lines else "(no UART output)"
            print("  FAIL: red %3d -> expected '%s', got: %s" % (red, expected_tag, summary))
            passed = False

    return passed


def test_mixed(dev, ser) -> bool:
    """Interleave rumble and LED packets to verify correct routing."""
    print("\n--- MIXED PACKET TEST ---")
    passed = True

    sequence = [
        ("rumble", 100, 200, RUMBLE_LED_RED),
        ("led", 3, None, 0),
        ("rumble", 50, 75, RUMBLE_LED_RED),
        ("led", 1, None, 128),
    ]

    for typ, a, b, red in sequence:
        if typ == "rumble":
            ok = send_pkt(dev, make_pkt(motor_right=b, motor_left=a, red=red))
            expected_tag = "CB_RUMBLE:%d,%d" % (a, b)
        else:
            ok = send_pkt(dev, make_pkt(motor_right=0, motor_left=0, red=red))
            expected_tag = "CB_LED:%d" % red

        if not ok:
            print("  FAIL: could not write packet")
            passed = False
            continue

        lines = read_uart_lines(ser, duration=3.0)
        matched_line = find_line_tag(lines, expected_tag)
        if matched_line:
            print("  PASS: -> got '%s'" % matched_line)
        else:
            summary = "; ".join(lines[:5]) if lines else "(no UART output)"
            print("  FAIL: expected '%s', got: %s" % (expected_tag, summary))
            passed = False

    return passed


def test_led_rgb(dev, ser) -> bool:
    """Inject distinct RGB values and verify CB_LEDRGB telemetry."""
    print("\n--- LED RGB TEST ---")
    passed = True

    # Distinct (r,g,b) tuples — none overlap with rumble's constant red=17/g=0/b=0.
    colors = [
        (255, 255, 255),   # white
        (255, 0, 0),       # pure red
        (0, 255, 0),       # pure green
        (0, 0, 255),       # pure blue
        (255, 0, 255),     # magenta
        (17, 34, 51),      # mixed low values
    ]

    for r, g, b in colors:
        ok = send_pkt(dev, make_pkt(motor_right=0, motor_left=0, red=r, green=g, blue=b))
        if not ok:
            print("  FAIL: could not write RGB packet (%d,%d,%d)" % (r, g, b))
            passed = False
            continue

        lines = read_uart_lines(ser, duration=3.0)
        expected_tag = "CB_LEDRGB:%d,%d,%d" % (r, g, b)
        matched_line = find_line_tag(lines, expected_tag)
        if matched_line:
            print("  PASS: RGB(%3d,%3d,%3d) -> got '%s'" % (r, g, b, matched_line))
        else:
            summary = "; ".join(lines[:5]) if lines else "(no UART output)"
            print("  FAIL: RGB(%3d,%3d,%3d) -> expected '%s', got: %s" % (r, g, b, expected_tag, summary))
            passed = False

    return passed


def main():
    parser = argparse.ArgumentParser(description="DS4Gamepad OUT packet test")
    parser.add_argument("--uart", default=None, help="UART port for telemetry "
                        "(auto-detected among /dev/ttyACM0, /dev/ttyUSB0)")
    args = parser.parse_args()

    print("Finding DS4Gamepad device...")
    dev = find_device()
    print("  Found: %s" % dev)

    # CRITICAL: do NOT call dev.set_configuration(). It triggers a USB
    # re-enumeration/reset on the device which unmounts TinyUSB and stops output
    # report delivery; once the OUT FIFO fills, writes hang with Errno 110.
    try:
        detach_kernel_driver(dev)
        usb.util.claim_interface(dev, 0)
        detect_out_endpoint(dev)
        print("  Interface claimed")
    except Exception as e:
        print("ERROR claiming interface: %s" % e)
        sys.exit(1)

    uart_path = find_uart(args.uart)
    if not uart_path:
        print("ERROR: no UART found; pass --uart explicitly.")
        usb.util.dispose_resources(dev)
        sys.exit(1)

    try:
        ser = serial.Serial(uart_path, 115200, timeout=3, rtscts=False, dsrdtr=False)
        time.sleep(1.5)

        # Drain any stale output from before we opened the port.
        while ser.in_waiting > 0:
            ser.readline()

        print("UART opened on %s" % uart_path)

        # Wait for sketch to signal it's ready for packets.
        print("Waiting for READY_FOR_PACKETS...")
        found_ready = wait_for_keyword(ser, "READY_FOR_PACKETS", timeout=15)

        if not found_ready:
            print("WARNING: Did not receive READY_FOR_PACKETS — proceeding anyway (USB may not be mounted)")
    except Exception as e:
        print("ERROR opening UART: %s" % e)
        usb.util.dispose_resources(dev)
        sys.exit(1)

    # Allow device to settle after USB claim.
    time.sleep(0.5)

    results = []
    try:
        results.append(("Rumble", test_rumble(dev, ser)))
        time.sleep(0.2)
        results.append(("LED", test_led(dev, ser)))
        time.sleep(0.2)
        results.append(("RGB", test_led_rgb(dev, ser)))
        time.sleep(0.2)
        results.append(("Mixed", test_mixed(dev, ser)))

        # Signal sketch to print final results and halt, then surface its
        # on-board summary (PASS/FAIL counts) for the record.
        time.sleep(0.5)
        ser.write(b"DONE\n")
        final_lines = read_uart_lines(ser, duration=2.0)
        for l in final_lines:
            if "FINAL RESULTS" in l or "ALL TESTS" in l or "INCOMPLETE" in l:
                print("  [sketch] %s" % l)
        time.sleep(0.5)
    finally:
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
        ser.close()

    print("\n=== RESULTS ===")
    all_pass = True
    for name, ok in results:
        status = "PASS" if ok else "FAIL"
        print("  %s: %s" % (name, status))
        if not ok:
            all_pass = False

    if all_pass:
        print("\nALL TESTS PASSED")
    else:
        print("\nSOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
