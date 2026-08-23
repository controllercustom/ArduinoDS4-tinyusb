#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Validate an AutoCycle pcap capture for the ArduinoDS4-tinyusb library.

The AutoCycle.ino sketch emits a deterministic P0..P11 stimulus cycle. Between
every phase it sends an unmistakable "marker" frame (all 14 buttons pressed,
d-pad centered, sticks centered, triggers zero). This tool:

  1. Extracts the 64-byte DS4 input reports (Report ID 0x01 + 63-byte payload)
     from a usbmon pcap (Linux `tcpdump -i usbmonN -w file.pcap`).
  2. Splits the stream into phases on the marker frames.
  3. Verifies each phase's expected content (P0..P11), the always-incrementing
     6-bit report counter, and the constant 0x1B battery byte.

Extraction uses `tshark` when available, otherwise a scapy/raw fallback that
scans packet bytes for the DS4 report signature (byte0 == 0x01 and byte30 ==
0x1B). No special USB analyzer hardware is required — only a host-side usbmon
capture.

Usage:
  python3 validate_autocycle_pcap.py capture.pcap
  python3 validate_autocycle_pcap.py capture.pcap --quiet
  python3 validate_autocycle_pcap.py capture.pcap --require-cycles 2
"""

import argparse
import re
import struct
import subprocess
import sys

# ---- DS4 report layout (matches src/DS4Gamepad.cpp) -----------------------
# On the wire: Report ID (0x01) byte followed by 63 payload bytes.
OFF_LX, OFF_LY, OFF_RX, OFF_RY = 0, 1, 2, 3
OFF_BTN0, OFF_BTN1, OFF_BTN2 = 4, 5, 6
OFF_L2, OFF_R2 = 7, 8
OFF_BATT = 29
OFF_TPADACT = 32
OFF_F1, OFF_F2 = 34, 38
PAYLOAD_LEN = 63

# Button bit masks
FACE = {"SQUARE": 0x10, "CROSS": 0x20, "CIRCLE": 0x40, "TRIANGLE": 0x80}
SHOULDER = {"L1": 0x01, "R1": 0x02, "L2": 0x04, "R2": 0x08,
            "SHARE": 0x10, "OPTIONS": 0x20, "L3": 0x40, "R3": 0x80}
SPECIAL = {"PS": 0x01, "TOUCHPAD": 0x02}

# Marker frame: all face buttons + all shoulders/menu/sticks + PS + TOUCHPAD,
# d-pad centered (0x08), sticks centered (0x80 each), triggers zero.
MARKER_BTN0 = 0xF8
MARKER_BTN1 = 0xFF
MARKER_BTN2 = 0x03


def is_marker(p):
    return (p[OFF_BTN0] == MARKER_BTN0 and
            p[OFF_BTN1] == MARKER_BTN1 and
            (p[OFF_BTN2] & 0x03) == MARKER_BTN2)


def counter_of(p):
    return (p[OFF_BTN2] >> 2) & 0x3F


# ---------------------------------------------------------------------------
# Extraction
# ---------------------------------------------------------------------------

def extract_with_tshark(path):
    """Return list of 63-byte payloads via tshark usbmon capture."""
    try:
        out = subprocess.check_output(
            ["tshark", "-r", path,
             "-Y", "usb.endpoint_address==0x81 && usbhid.data",
             "-T", "fields", "-e", "usbhid.data"],
            stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return None
    payloads = []
    for line in out.decode("utf-8", "ignore").splitlines():
        line = line.strip()
        if not line:
            continue
        # tshark may separate bytes by ':' or emit raw hex.
        hexstr = line.replace(":", "")
        try:
            data = bytes.fromhex(hexstr)
        except ValueError:
            continue
        if len(data) >= PAYLOAD_LEN + 1 and data[0] == 0x01:
            payloads.append(data[1:1 + PAYLOAD_LEN])
    return payloads or None


def extract_with_scapy(path):
    """Fallback: scan raw packet bytes for the DS4 report signature."""
    try:
        from scapy.all import rdpcap
    except ImportError:
        return None
    try:
        pkts = rdpcap(path)
    except Exception:
        return None
    payloads = []
    for pkt in pkts:
        raw = bytes(pkt)
        # Slide a 64-byte window looking for (0x01, ..., battery 0x1B at 30).
        for i in range(0, max(0, len(raw) - 63)):
            if raw[i] == 0x01 and raw[i + 30] == 0x1B:
                payloads.append(raw[i + 1:i + 1 + PAYLOAD_LEN])
                break
    return payloads or None


def extract_reports(path):
    payloads = extract_with_tshark(path)
    if payloads:
        return payloads, "tshark"
    payloads = extract_with_scapy(path)
    if payloads:
        return payloads, "scapy"
    return None, "none"


# ---------------------------------------------------------------------------
# Phase classification + validation
# ---------------------------------------------------------------------------

def split_phases(payloads):
    """Group consecutive non-marker frames into phase buckets."""
    phases = []
    current = []
    for p in payloads:
        if is_marker(p):
            if current:
                phases.append(current)
                current = []
            # markers are separators; ignore the marker frame itself
        else:
            current.append(p)
    if current:
        phases.append(current)
    return phases


def check_idle(phase, label):
    """P0/P11: no inputs, sticks/triggers centered, d-pad centered."""
    errors = []
    for idx, p in enumerate(phase):
        if p[OFF_BTN0] != 0x08 or p[OFF_BTN1] != 0x00 or (p[OFF_BTN2] & 0x03) != 0x00:
            errors.append(f"{label} frame {idx}: nonzero buttons")
            break
        if not all(p[o] == 0x80 for o in (OFF_LX, OFF_LY, OFF_RX, OFF_RY)):
            errors.append(f"{label} frame {idx}: sticks not centered")
            break
        if p[OFF_L2] != 0x00 or p[OFF_R2] != 0x00:
            errors.append(f"{label} frame {idx}: triggers not zero")
            break
    return errors


def check_buttons(phase):
    """P1: every one of the 14 buttons observed pressed at least once."""
    seen = set()
    for p in phase:
        for name, bit in FACE.items():
            if p[OFF_BTN0] & bit:
                seen.add("FACE:" + name)
        for name, bit in SHOULDER.items():
            if p[OFF_BTN1] & bit:
                seen.add("SH:" + name)
        if p[OFF_BTN2] & SPECIAL["PS"]:
            seen.add("SPEC:PS")
        if p[OFF_BTN2] & SPECIAL["TOUCHPAD"]:
            seen.add("SPEC:TOUCHPAD")
    expected = (set("FACE:" + n for n in FACE) |
                set("SH:" + n for n in SHOULDER) |
                {"SPEC:PS", "SPEC:TOUCHPAD"})
    return sorted(expected - seen)


def check_dpad(phase):
    """P2: d-pad low nibble should cover 0..8."""
    seen = set()
    for p in phase:
        seen.add(p[OFF_BTN0] & 0x0F)
    missing = [d for d in range(0, 9) if d not in seen]
    return missing


def check_axis(phase, off):
    vals = [p[off] for p in phase]
    return min(vals), max(vals)


def check_triggers(phase, off):
    vals = [p[off] for p in phase]
    return min(vals), max(vals)


def check_touch_varies(phase, off):
    seen = set(bytes(p[off:off + 4]) for p in phase)
    return len(seen) > 1


def check_ps_tp_click(phase):
    """P9: PS and TOUCHPAD bits must both toggle (set and clear)."""
    ps = [bool(p[OFF_BTN2] & SPECIAL["PS"]) for p in phase]
    tp = [bool(p[OFF_BTN2] & SPECIAL["TOUCHPAD"]) for p in phase]
    errors = []
    if not (any(ps) and not all(ps)):
        errors.append("PS bit did not toggle")
    if not (any(tp) and not all(tp)):
        errors.append("TOUCHPAD bit did not toggle")
    return errors


def check_stress(phase):
    """P10: must contain an all-shoulder/menu/stick frame with d-pad UP and at
    least one SQUARE rapid-fire frame."""
    errors = []
    saw_all_shoulders = False
    saw_square_rapid = False
    prev_square = None
    for p in phase:
        if p[OFF_BTN1] == 0xFF and (p[OFF_BTN0] & 0x0F) == 0x00:
            saw_all_shoulders = True
        sq = bool(p[OFF_BTN0] & FACE["SQUARE"])
        if prev_square is not None and sq != prev_square:
            saw_square_rapid = True
        prev_square = sq
    if not saw_all_shoulders:
        errors.append("no all-shoulder/dpad-UP frame found")
    if not saw_square_rapid:
        errors.append("no SQUARE rapid-fire toggle found")
    return errors


def validate_phase(idx, phase):
    """Return (passed, messages)."""
    msgs = []
    ok = True
    if idx == 0 or idx == 11:
        errs = check_idle(phase, f"P{idx}")
        if errs:
            ok = False
            msgs.extend(errs)
    elif idx == 1:
        missing = check_buttons(phase)
        if missing:
            ok = False
            msgs.append("buttons never pressed: " + ", ".join(missing))
    elif idx == 2:
        missing = check_dpad(phase)
        if missing:
            ok = False
            msgs.append("dpad values missing: " + ", ".join(str(m) for m in missing))
    elif idx == 3:
        mn, mx = check_axis(phase, OFF_LX)
        if not (mn <= 0x10 and mx >= 0xF0):
            ok = False
            msgs.append(f"LX range insufficient min=0x{mn:02X} max=0x{mx:02X}")
        mn, mx = check_axis(phase, OFF_LY)
        if not (mn <= 0x10 and mx >= 0xF0):
            ok = False
            msgs.append(f"LY range insufficient min=0x{mn:02X} max=0x{mx:02X}")
    elif idx == 4:
        mn, mx = check_axis(phase, OFF_RX)
        if not (mn <= 0x10 and mx >= 0xF0):
            ok = False
            msgs.append(f"RX range insufficient min=0x{mn:02X} max=0x{mx:02X}")
        mn, mx = check_axis(phase, OFF_RY)
        if not (mn <= 0x10 and mx >= 0xF0):
            ok = False
            msgs.append(f"RY range insufficient min=0x{mn:02X} max=0x{mx:02X}")
    elif idx == 5:
        mn, mx = check_triggers(phase, OFF_L2)
        if not (mn <= 0x02 and mx >= 0xFD):
            ok = False
            msgs.append(f"L2 range insufficient min=0x{mn:02X} max=0x{mx:02X}")
    elif idx == 6:
        mn, mx = check_triggers(phase, OFF_R2)
        if not (mn <= 0x02 and mx >= 0xFD):
            ok = False
            msgs.append(f"R2 range insufficient min=0x{mn:02X} max=0x{mx:02X}")
    elif idx == 7:
        if not check_touch_varies(phase, OFF_F1):
            ok = False
            msgs.append("finger0 touch state did not vary")
    elif idx == 8:
        if not check_touch_varies(phase, OFF_F1) or not check_touch_varies(phase, OFF_F2):
            ok = False
            msgs.append("two-finger touch state did not vary")
    elif idx == 9:
        errs = check_ps_tp_click(phase)
        if errs:
            ok = False
            msgs.extend(errs)
        if not check_touch_varies(phase, OFF_F1):
            ok = False
            msgs.append("finger0 not present in P9")
    elif idx == 10:
        errs = check_stress(phase)
        if errs:
            ok = False
            msgs.extend(errs)
    return ok, msgs


def check_counter(payloads):
    """6-bit counter must increment by 1 each frame (mod 64)."""
    errors = 0
    prev = None
    for p in payloads:
        c = counter_of(p)
        if prev is not None:
            if ((prev + 1) & 0x3F) != c:
                errors += 1
        prev = c
    return errors


def check_battery(payloads):
    bad = sum(1 for p in payloads if p[OFF_BATT] != 0x1B)
    return bad


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pcap", help="usbmon pcap file")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--require-cycles", type=int, default=1,
                    help="minimum number of full P0..P11 cycles required")
    args = ap.parse_args()

    payloads, method = extract_reports(args.pcap)
    if not payloads:
        print("ERROR: no DS4 reports extracted (tshark and scapy both failed).")
        sys.exit(2)
    if not args.quiet:
        print(f"Extracted {len(payloads)} reports via {method}.")

    # Global invariants
    cnt_err = check_counter(payloads)
    batt_err = check_battery(payloads)
    if not args.quiet:
        print(f"Counter errors (non-incrementing): {cnt_err}")
        print(f"Battery errors (byte[30] != 0x1B): {batt_err}")

    phases = split_phases(payloads)

    # A leading fragment before the first marker means the capture began
    # mid-phase; split_phases() would otherwise turn that prefix into an
    # incomplete first group. Drop it.
    first_marker = next((i for i, p in enumerate(payloads) if is_marker(p)), None)
    if first_marker is None:
        print("ERROR: no marker frame found in capture.")
        sys.exit(1)
    if first_marker > 0:
        if not args.quiet:
            print(f"Dropped {first_marker} leading frames "
                  f"(capture began mid-phase).")
        payloads = payloads[first_marker:]
        phases = split_phases(payloads)

    # A trailing group closed by EOF instead of a marker frame is a truncated
    # capture-boundary artifact (the window ended mid-phase), not firmware
    # behavior — exclude it from validation.
    if phases and payloads and not is_marker(payloads[-1]):
        dropped = phases.pop()
        if not args.quiet:
            print(f"Dropped unclosed trailing group ({len(dropped)} frames, "
                  f"capture ended mid-phase).")

    if not args.quiet:
        print(f"Detected {len(phases)} phase groups (markers={sum(1 for p in payloads if is_marker(p))}).")

    n = len(phases)
    if n < 12 * args.require_cycles:
        print(f"ERROR: only {n} phase groups; need >= {12 * args.require_cycles}.")
        sys.exit(1)

    # The capture may start mid-cycle, so a group's position is not its phase
    # index. Find the alignment offset k (0..11) maximizing the number of groups
    # whose content matches validate_phase((i + k) % 12, group[i]).
    best_k, best_score = 0, -1
    for k in range(12):
        score = 0
        for i, ph in enumerate(phases):
            if ph and validate_phase((i + k) % 12, ph)[0]:
                score += 1
        if score > best_score:
            best_score, best_k = score, k
    if not args.quiet:
        print(f"Aligned capture to cycle offset k={best_k} "
              f"(matching groups: {best_score}/{n}).")

    by_phase = {p: [] for p in range(12)}
    for i, ph in enumerate(phases):
        if ph:
            by_phase[(i + best_k) % 12].append(ph)

    all_ok = True
    for p in range(12):
        groups = by_phase[p]
        if not groups:
            print(f"P{p}: FAIL (no group found)")
            all_ok = False
            continue
        phase_ok = True
        pmsgs = []
        for gi, ph in enumerate(groups):
            ok, msgs = validate_phase(p, ph)
            if not ok:
                phase_ok = False
                pmsgs.append(f"group {gi}: " + "; ".join(msgs))
        if not phase_ok:
            all_ok = False
        if not args.quiet or not phase_ok:
            status = "PASS" if phase_ok else "FAIL"
            extra = "" if phase_ok else " -> " + " | ".join(pmsgs)
            print(f"P{p}: {status} ({len(groups)} group(s)){extra}")

    if cnt_err:
        all_ok = False
        print(f"FAIL: {cnt_err} counter discontinuities.")
    if batt_err:
        all_ok = False
        print(f"FAIL: {batt_err} battery-byte mismatches.")

    print("\nRESULT: " + ("PASS" if all_ok else "FAIL"))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
