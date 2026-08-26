# ArduinoDS4-tinyusb Test Baseline

Verified results for the ArduinoDS4-tinyusb library test battery on SAMD,
captured on Arduino Zero hardware using the cross-flash Metro M0 build (see
`AGENTS.md` "SAMD on-board verification"). This library is TinyUSB-only on
SAMD (`ARDUINO_ARCH_SAMD && USE_TINYUSB`), so there is a single tier — unlike
the SAMDds4 reference, which also has a logic tier.

## Environment

- **Board**: Arduino Zero (ATSAMD21G18A). EDBG programming port (`/dev/ttyACM0`,
  `03eb:2157`) used for OpenOCD flashing only. Native USB port enumerates as
  `054C:05C4`. FTDI bridge on D0/D1 header (`/dev/ttyUSB0`) carries
  `DS4_DEBUG_SERIAL` telemetry at 115200.
- **Board 2**: Adafruit Grand Central M4 (ATSAMD51P20A), native USB port,
  same FTDI on Serial1 (pins 0/1). Flashed via SEGGER J-Link PLUS SWD
  (`JLinkExe -device ATSAMD51P20A`).
- **Board 3**: Adafruit Feather nRF52840 Express (NRF52840_XXAA), native USB
  port, same FTDI on Serial1 (D0/D1). Flashed via SEGGER J-Link PLUS SWD
  (`JLinkExe -device nRF52840_xxAA`, app `.hex` — SoftDevice/bootloader left
  intact). Core `adafruit:nrf52` 1.7.0 (SoftDevice S140 6.1.1).
- **Core**: `adafruit:samd` 1.7.17, `usbstack=tinyusb`. Build FQBN
  `adafruit:samd:adafruit_feather_m0:usbstack=tinyusb` (metro_m0 equivalent).
  `Adafruit_TinyUSB_Arduino` bundled with the core.
- **Toolchain**: arduino-cli; OpenOCD
  `~/.arduino15/packages/arduino/tools/openocd/0.11.0-arduino2/bin/openocd`;
  objcopy from `~/.arduino15/packages/adafruit/tools/arm-none-eabi-gcc/9-2019q4`.
- **Host**: Linux, `hid-playstation` driver (auto-claims the DS4), `pyusb` +
  `pyserial` + `evdev` for harnesses.
- **Date**: 2026-08-23.

## Flash method

All Zero flashing is via OpenOCD (`.bin` @ `0x2000`, bootloader intact).
`arduino-cli upload` is broken on both samd cores (OpenOCD arg
incompatibility), so the manual flow is used:

```bash
FQBN="adafruit:samd:adafruit_feather_m0:usbstack=tinyusb"
arduino-cli compile --fqbn "$FQBN" --build-path /tmp/opencode/ds4z \
  --library /home/pi/ArduinoDS4-tinyusb <sketch>
arm-none-eabi-objcopy -O binary /tmp/opencode/ds4z/*.ino.elf /tmp/opencode/ds4z/app.bin
openocd -f interface/cmsis-dap.cfg -c 'transport select swd' -f target/at91samdXX.cfg \
  -c "init" -c "reset halt" \
  -c "program /tmp/opencode/ds4z/app.bin verify 0x00002000" \
  -c "reset run" -c "shutdown"
```

Start the FTDI logger BEFORE triggering reset to catch early `setup()` output.

### Grand Central M4 (J-Link) — ⚠️ app base is 0x4000, NOT 0x2000

The Grand Central bootloader is **16 KB** (`bootloaders/grand_central_m4/`,
`FLASH ORIGIN = 0x00000000+0x4000 /* First 16KB used by bootloader */`).
Flashing an app `.bin` at `0x2000` corrupts the bootloader's upper half:
symptoms are no USB enumeration at all and the core looping in early boot code
(`PC ≈ 0x570`, below the app region) after every reset. Recovery is to restore
the shipped bootloader image, then flash the app at `0x4000`:

```bash
BL=~/.arduino15/packages/adafruit/hardware/samd/1.7.17/bootloaders/grand_central_m4/bootloader-grandcentral_m4.bin
printf 'h\nloadfile %s,0x0\nloadfile /path/app.bin,0x4000\nr\ng\nq\n' "$BL" > /tmp/f.jlink
JLinkExe -device ATSAMD51P20A -if swd -speed 4000 -autoconnect 1 -NoGui 1 \
         -CommandFile /tmp/f.jlink
```

Routine re-flashing of a working board needs only the app line
(`loadfile app.bin,0x4000`). The `.ino.elf` from arduino-cli for this FQBN is
already linked at `0x4000`, so its objcopy `.bin` maps directly.

### Feather nRF52840 (J-Link, `.hex`)

```bash
arduino-cli compile --profile feather52840 tests/<Test>      # sketch.yaml profile
# or: arduino-cli compile --fqbn "adafruit:nrf52:feather52840:softdevice=s140v6,debug=l0,debug_output=serial" \
#      --library /home/pi/ArduinoDS4-tinyusb <sketch>
printf 'h\nloadfile /path/Sketch.ino.hex\nr\ng\nq\n' > /tmp/f.jlink
JLinkExe -device nRF52840_xxAA -if swd -speed 4000 -autoconnect 1 -NoGui 1 \
         -CommandFile /tmp/f.jlink
```

The `.hex` carries addresses and covers the app region only — SoftDevice
S140 + UF2 bootloader stay intact. Use one `--build-path` per sketch: mixing
sketches in one build dir leaves a stale `.hex` of whatever built last (this
bit once — flashed LatencyBenchmark while intending TestBasicFunctionality).
If odd library-detection errors appear after cache-sensitive operations:
`rm -rf ~/.cache/arduino/`.

## Compile battery (Feather M0 TinyUSB)

| Sketch | Flash |
|---|---|
| examples: BasicGamepad, AutoCycle, FullController, JoystickTest, SimpleButtons | OK, 17–19% |
| tests: TestBasicFunctionality, TestOutputCallbacks, LatencyBenchmark | OK, 19–24% |
| Spot-check `adafruit:samd:adafruit_metro_m4:usbstack=tinyusb` (SAMD51 path) | OK |
| Regression `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc` | OK |
| Regression `rp2040:rp2040:rpipico:usbstack=tinyusb` | OK |

Negative check: same sketch with adafruit:samd default "Arduino" USB stack
fails with `'DS4Gamepad' does not name a type` (empty header — documented
gotcha, same failure shape as RP2040 without `USE_TINYUSB`).

---

## TestBasicFunctionality — PASS 68 / FAIL 0

```
=== DS4Gamepad TestBasicFunctionality ===
--- Buttons ---  --- Sticks ---  --- Triggers ---  --- Hat ---  --- Touch ---
--- releaseAll ---  --- LED callback ---  --- _setFeature output report ---
=== RESULTS ===
PASS: 68
FAIL: 0
ALL TESTS PASSED
```

Reproduced twice on 2026-08-23 with identical results.

## TestOutputCallbacks + scripts/test_output_packets.py — ALL TESTS PASSED

Host harness sends rumble/LED/RGB/mixed output reports 0x05 via pyusb
(`detach_kernel_driver(0)` + `claim_interface(0)`, never `set_configuration()`)
and correlates with `CB_*` telemetry on the FTDI:

```
=== RESULTS ===
  Rumble: PASS   LED: PASS   RGB: PASS   Mixed: PASS
ALL TESTS PASSED
  [sketch] === FINAL RESULTS: PASS=50 FAIL=0 RUMBLE_PKTS=22 LED_PKTS=22 ===
  [sketch] ALL TESTS PASSED
```

Reproduced twice with identical counts.

## BasicGamepad + tests/test_e2e.py — PASS

Two runs against the evdev gamepad (auto-detected via ABS_HAT0X):

| Run | Worst p99 | Worst max | Result |
|---|---|---|---|
| 1 | 3.152 ms | 3.624 ms | PASS |
| 2 | 4.629 ms | 7.368 ms | PASS |

Threshold `<10 ms p99`. Touch section SKIPs (`ABS_MT_POSITION_X` not mapped by
`hid-playstation` for this report layout) — expected on all platforms.

## LatencyBenchmark — protocol smoke run CLEAN

Logger-first reset, then `START` over the UART:

```
banner: RPT:63:14   BTN_lines=14   READY=True
TS lines=2652 end_marker=True
first: TS:432074:1:99:0
last : TS:5134004:2652:99:1     (~4.7 s run)
seq discontinuities=0
type counts: {'0': 700, '1': 700, '2': 400, '3': 200, '4': 200, '5': 450, '99': 2}
```

2652 = 50 iterations x (14 buttons x 2 + 8 stick + 4 trigger + 4 touch + 9 hat)
+ 2 BEGIN markers; every count exact, sequence perfectly continuous.

## AutoCycle + tests/validate_autocycle_pcap.py — RESULT: PASS

~70 s `tcpdump -i usbmon1` capture while AutoCycle ran (marker-frame splitting
makes start/end position irrelevant):

```
Extracted 672 reports via tshark.
Counter errors (non-incrementing): 0
Battery errors (byte[30] != 0x1B): 0
Detected 26 phase groups (markers=27). Aligned k=2 (matching groups: 26/26).
P0..P11: PASS
RESULT: PASS
```

Second capture same day: 670 reports, RESULT: PASS. Capture kept at
`/tmp/ds4_zero_autocycle.pcap`.

---

## SAMD51 — Adafruit Grand Central M4 (J-Link flash @ 0x4000)

Build `adafruit:samd:adafruit_grandcentral_m4:usbstack=tinyusb` (SAMD51P20A,
`__SAMD51__` MAC path exercised). All sketches 5% of 1 MB flash. Date
2026-08-23.

| Test | Result |
|---|---|
| TestBasicFunctionality (on-board) | **PASS 68 / FAIL 0** |
| TestOutputCallbacks + test_output_packets.py | **ALL TESTS PASSED**; board `PASS=50 FAIL=0 RUMBLE_PKTS=22 LED_PKTS=22` |
| BasicGamepad + test_e2e.py latency | **PASS** — worst p99 3.073 ms, max 3.103 ms (<10 ms) |
| LatencyBenchmark protocol run | **CLEAN** — 2652/2652 TS events, 0 seq gaps, type counts exact, 4.70 s emit window |
| AutoCycle pcap validation (usbmon1, 674 reports) | **RESULT: PASS** — P0..P11 all pass, 0 counter/battery errors |

Capture kept at `/tmp/ds4_gcm4_autocycle.pcap`. The M4 numbers match the Zero
(SAMD21) baseline within run-to-run noise.

---

## nRF52 — Adafruit Feather nRF52840 Express (J-Link flash, `.hex`)

Build `adafruit:nrf52:feather52840:softdevice=s140v6,debug=l0,debug_output=serial`
(`NRF52840_XXAA`, FICR DEVICEID MAC path exercised). All sketches 7–10% of the
815 KB app region. Date 2026-08-23. Telemetry FTDI `/dev/ttyUSB0` @115200
(Serial1, D0/D1).

| Test | Result |
|---|---|
| Compile battery: 5 examples + 3 tests (profile builds) | all OK |
| Regression compile esp32s3 / rpipico / feather_m0 / grandcentral_m4 | all OK |
| TestBasicFunctionality (on-board) | **PASS 68 / FAIL 0** |
| TestOutputCallbacks + test_output_packets.py | **ALL TESTS PASSED**; board `PASS=50 FAIL=0 RUMBLE_PKTS=22 LED_PKTS=22` |
| BasicGamepad + test_e2e.py latency | **PASS** — worst p99 4.406 ms, max 4.421 ms (<10 ms) |
| LatencyBenchmark protocol run | **CLEAN** — 2652/2652 TS events, 0 seq gaps, type counts exact, 5.50 s emit window |
| AutoCycle pcap validation (usbmon1, 665 reports) | **RESULT: PASS** — P0..P11 all pass, 0 counter/battery errors |

Capture kept at `/tmp/ds4_nrf52_autocycle.pcap`.

---

## Summary

| Test | Zero (SAMD21, cross-flash) | Grand Central M4 (SAMD51, J-Link) | Feather nRF52840 (J-Link) |
|---|---|---|---|
| TestBasicFunctionality (on-board) | PASS 68 / FAIL 0 | PASS 68 / FAIL 0 | PASS 68 / FAIL 0 |
| TestOutputCallbacks + host harness | ALL PASSED (board PASS=50, 22+22 pkts) | ALL PASSED (board PASS=50, 22+22 pkts) | ALL PASSED (board PASS=50, 22+22 pkts) |
| BasicGamepad + test_e2e.py latency | PASS, worst p99 3.15–4.63 ms (<10 ms) | PASS, worst p99 3.07 ms (<10 ms) | PASS, worst p99 4.41 ms (<10 ms) |
| LatencyBenchmark protocol run | 2652/2652 events, 0 gaps | 2652/2652 events, 0 gaps | 2652/2652 events, 0 gaps |
| AutoCycle pcap validation | RESULT: PASS (P0–P11, 0 errors) | RESULT: PASS (P0–P11, 0 errors) | RESULT: PASS (P0–P11, 0 errors) |

These numbers are the regression baseline for SAMD and nRF52 (all four
hardware paths: SAMD21, SAMD51, nRF52840). Re-run the battery after any
library change and compare. ESP32-S3 and RP2040 baselines are unchanged by the
ports (guards only add platforms); re-verify there if shared code paths are
touched.


## Renesas RA4M1 — UNO R4 Minima (2026-08-26)

Board: UNO R4 Minima (`R7FA4M1AB`, patched `arduino:renesas_uno` 1.6.0 via
`scripts/patch_renesas_core.sh`; builds with `-DDISABLE_USB_SERIAL`). Flashed
via SEGGER J-Link SWD through the 0x0 flash alias (device `R7FA4M1AB`,
`loadfile <sketch>.ino.hex` — app links at 0x4000). Telemetry: FTDI on D0/D1
(`/dev/ttyUSB0`, 115200). Native USB enumerates as `054C:05C4`; kernel
`hid-playstation` binds automatically (feature report 0x81 served →
"Registered DualShock4 controller").

| Test | Result |
|---|---|
| Compile battery: 5 examples + 3 tests × nanor4/minima | **16/16 OK** (~17–21% flash) |
| Regression spot-compiles rpipico + Grand Central M4 | OK |
| `TestBasicFunctionality` (on-board) | PASS 6/FAIL 0 at PRE-USB stage |
| `TestOutputCallbacks` + `test_output_packets.py` | **ALL TESTS PASSED**; board `PASS=44 FAIL=0 RUMBLE_PKTS=19 LED_PKTS=19` (control SET_REPORT transport, deferred dispatch) |
| `test_e2e.py` vs `BasicGamepad` (evdev latency) | **PASS**; worst p99 6.08 ms, max 6.82 ms (<10 ms threshold) |
| `AutoCycle` pcap validation (usbmon3, DURATION=75, 1430 packets) | **RESULT: PASS**, P2..P11 all PASS (2 groups each), 0 counter/battery errors |

Notes:
- Output reports use control SET_REPORT only (IN-only interface, authentic
  DS4-v1 shape). `scripts/test_output_packets.py` auto-detects the missing
  OUT endpoint and falls back to ctrl transfers (`wValue = 0x0200 | id`).
- The stock core's HID IN interval is fixed (~10 ms), so AutoCycle phases run
  ~2.5× longer than on 4 ms platforms — capture with `DURATION=75`.
- User callbacks MUST be dispatched outside the USB IRQ (FspTimer pump) — see
  AGENTS.md "Renesas CRITICAL".
