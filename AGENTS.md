# AGENTS.md — ArduinoDS4-tinyusb

## Overview

Unified Arduino library emulating a PlayStation DualShock 4-style USB gamepad
(VID `054C`, PID `05C4`) on **ESP32-S3**, **RP2040/Pico** and **SAMD21/SAMD51**
from one code base, using each core's built-in TinyUSB stack. Header +
implementation live in `src/`; the class is `DS4Gamepad`.

## Build & Upload (arduino-cli)

```bash
# ESP32-S3
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc" \
  --library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad

arduino-cli compile -u -p /dev/ttyUSB0 \
  --fqbn "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=default" \
  --library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad

# RP2040 / Pico
arduino-cli compile --fqbn "rp2040:rp2040:rpipico:usbstack=tinyusb" \
  --library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad

arduino-cli compile -u -p /dev/ttyACM0 \
  --fqbn "rp2040:rp2040:rpipico:usbstack=tinyusb" \
  pass "--library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad"

# SAMD21 / SAMD51 (adafruit:samd core; USB Stack menu MUST be tinyusb)
arduino-cli compile --fqbn "adafruit:samd:adafruit_feather_m0:usbstack=tinyusb" \
  --library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad
arduino-cli compile --fqbn "adafruit:samd:adafruit_grandcentral_m4:usbstack=tinyusb" \
  --library /home/pi/ArduinoDS4-tinyusb tests/TestBasicFunctionality

# nRF52840 (adafruit:nrf52 core — TinyUSB always built, no stack menu)
arduino-cli compile --fqbn "adafruit:nrf52:feather52840:softdevice=s140v6,debug=l0,debug_output=serial" \
  --library /home/pi/ArduinoDS4-tinyusb examples/BasicGamepad
# or via the per-test sketch.yaml profiles (no --library allowed with --profile):
arduino-cli compile --profile feather52840 tests/TestBasicFunctionality
```

**ESP32-S3 FQBN must** include `USBMode=default,CDCOnBoot=cdc`. `CDCOnBoot=cdc`
is required so the core calls `tinyusb_init()` at boot; the CDC-ACM interface is
removed at runtime by `begin()` via `TinyUSBDevice.clearConfiguration()`.

**RP2040 FQBN must** include `usbstack=tinyusb` (the arduino-pico core's
EarlePhilhower USB stack). `begin()` calls `Serial.end()` then re-applies the
HID descriptors so the restored USB device exposes only the DS4 HID interface.

**SAMD FQBN must** include `usbstack=tinyusb` (adafruit:samd default is the
plain Arduino/CDC stack, under which the library's guard compiles to nothing
and sketches fail with `'DS4Gamepad' does not name a type`). `begin()` behaves
like the RP2040 path: `Serial.end()`, re-apply descriptors. The adafruit:samd
core bundles `Adafruit_TinyUSB_Arduino`; do not install a second copy.

## Isolated / reproducible builds

Use `scripts/avenv.sh` + `scripts/build.sh` for concurrent-safe, pinned,
multi-core builds. `build.sh` takes a core argument and pins the matching
platform (and, for esp32 only, the separate `Adafruit TinyUSB Library@3.7.7`):

```bash
# scripts/build.sh <core> <sketch_dir>   core ∈ esp32|rp2040|samd|nrf52
./scripts/build.sh esp32 examples/BasicGamepad
./scripts/build.sh nrf52 tests/TestBasicFunctionality
```

It does NOT use `--profile` because arduino-cli forbids `--library`+`--profile`
and profile builds ignore `user/libraries`. Some sketch dirs (the tests) ship a
`sketch.yaml` with a `default_profile`; `build.sh` stages it aside during the
compile so arduino-cli uses our explicit `--fqbn`/`--library`. Set
`AVENV_GOLDEN="$HOME/.arduino15"` to reuse the existing installed toolchain
cache; the first `build.sh` run populates it via `core install` / `lib install`
(this repo is a library, so `aventools prime` — which compiles the target as a
sketch — does not apply). The isolated `user/libraries` starts empty, so
undeclared deps fail fast.

### Offline builds (broken DNS / no network)

The aventools `downloads` dir is per-build, so **library archives are
re-downloaded on every build even with `AVENV_GOLDEN` set** (golden caches
cores/tools only). When DNS breaks (arduino-cli symptom:
`Download failed: ... dial tcp: lookup ... write udp ...: operation not
permitted`), compile ESP32 sketches with `scripts/offline_build.sh`, which
seeds `Adafruit TinyUSB Library@3.7.7` from the local
`~/.arduino15/staging/libraries/*.zip` into the per-build sketchbook and skips
`lib install`:

```bash
AVENV_GOLDEN="$HOME/.arduino15" ./scripts/offline_build.sh examples/AutoCycle -u -p /dev/ttyUSB0
```

TinyUSB's other declared deps (Adafruit NeoPixel, SdFat - Adafruit Fork,
SPIFlash, MIDI) are tag-dependencies that nothing here `#include`s — they are
not needed to compile. Other local cache sources for seeding:
`~/.arduino15/internal/<Lib>_<ver>_<hash>/` (materialized by earlier
`--profile` builds). RP2040/SAMD/nRF52 need no external libraries, so plain
`build.sh <core>` works offline once their cores are in the golden cache.

## Dependencies

- **ESP32-S3**: `esp32:esp32` core v3.3.x+ (built-in TinyUSB).
- **RP2040**: `rp2040:rp2040` core v6.0.0+ (built-in TinyUSB).
- **SAMD21/51**: `adafruit:samd` core v1.7.x (bundled TinyUSB; select via USB
  Stack menu). The stock `arduino:samd` Zero core has no TinyUSB and is NOT
  supported by this library.
- **nRF52840**: `adafruit:nrf52` core v1.7.0, SoftDevice S140 6.1.1 only
  (`softdevice=s140v6` — any other value fails link). Bundled TinyUSB; no USB
  Stack menu exists.
- **ESP32-S3 extra dependency**: `Adafruit_TinyUSB_Arduino` (Library Manager,
  "Adafruit TinyUSB Library") must be installed for ESP32 builds — unlike
  rp2040/samd/nrf52, that core does not bundle it. The library vendors its own
  TinyUSB stack on ESP32, so keep `USBMode=default` (never `tinyusb`) to avoid
  linking two stacks.
- No other external Arduino libraries required.

## Architecture

- Two files in `src/`: `DS4Gamepad.h` (enums + class) and `DS4Gamepad.cpp`
  (HID descriptor + logic). A `s_instance` singleton forwards the static
  TinyUSB callbacks to the active object.
- Platform selection is a single guard:
  `#if (defined(ARDUINO_ARCH_RP2040) && defined(USE_TINYUSB)) || \
      (defined(ARDUINO_ARCH_SAMD) && defined(USE_TINYUSB)) || \
      (defined(ARDUINO_ARCH_NRF52) && defined(USE_TINYUSB)) || \
      defined(ARDUINO_ARCH_ESP32)`
- HID descriptor is a `static const` byte array in `DS4Gamepad.cpp`.
- Always-send design (no dirty flag): every report is transmitted even if
  identical to the last, matching original hardware. Optional auto-send timer
  via `setPollInterval(ms)` — default **0** (disabled), requiring explicit
  `send()`.
- The 6-bit report counter lives in byte 7 (bits 2..7) plus PS/Touchpad-click
  bits; independent of the vendor timestamp bytes [10..11] (both verified
  against controller captures).

### Report layout (matches `tests/validate_autocycle_pcap.py`)

On the wire: Report ID `0x01` + 63 payload bytes. Offsets in the payload:

| byte | field |
|------|-------|
| 0..3 | LX, LY, RX, RY (unsigned 8-bit, 0x80 = center) |
| 4 | d-pad (low nibble) + face buttons (high nibble) |
| 5 | L1 R1 L2 R2 SHARE OPTIONS L3 R3 |
| 6 | counter (bits 2..7) + T-Pad click (bit1) + PS (bit0) |
| 7..8 | L2, R2 triggers |
| 9..10 | timestamp |
| 29 | battery (0x1B = 100% Full) |
| 32 | touch packets active (always 0x01) |
| 33 | touch packet counter |
| 34..37 | finger 0 |
| 38..41 | finger 1 |

### Auto-send vs explicit send

For test/validation sketches that set state then call `send()`, **disable
auto-send** (`setPollInterval(0)`) to avoid race conditions: with auto-send
enabled, the timer can fire between `releaseAll()` and the setters, capturing
reset/idle frames on the wire before intended values are written.

### Auto-send on SAMD/nRF52 (cooperative — differs from ESP32/RP2040)

No free-running OS timer is used on SAMD or nRF52 (ESP32 has esp_timer,
RP2040 repeating_timer; the nRF52 FreeRTOS software timer is deliberately
avoided for parity with the reference ports). Auto-send is therefore
**cooperative**: due sends are flushed from every public setter entry point
and from `ready()` via `_pumpAutoSend()` (a no-op on the other platforms).
Apps calling any library API per loop iteration get loop-rate polling; apps
that never call in get nothing. Default remains **0 = disabled**, requiring
explicit `send()` calls.

## API

```cpp
#include <DS4Gamepad.h>
DS4Gamepad gamepad;
gamepad.begin();
gamepad.press(DS4_BTN_CROSS);
gamepad.setStickLeft(0, 100);     // signed -32768..32767
gamepad.setDpad(DS4_HAT_UP);
gamepad.setLeftTrigger(32768);    // unsigned 0..32768
gamepad.setTouch(0, true, 960, 471);
gamepad.send();
gamepad.releaseAll();
```

Output report 0x05 (rumble + LED) callbacks: `onRumble(left, right)`,
`onLed(red)`, `onLedColor({r,g,b})`.

## Key Technical Details

- **VID/PID**: `054C:05C4`.
- **USB strings**: "Sony Computer Entertainment" / "Wireless Controller".
- **Report size**: 63 payload bytes + 1-byte Report ID = 64 on the wire.
- **Feature reports**: `0x02` (calibration), `0x12` (pairing/MAC), `0x81`
  (firmware info), `0xA3` (manufacture date).
- **Output report 0x05**: rumble + LED RGB — callbacks via `onLed()` /
  `onRumble()` / `onLedColor()`. Arrives on interrupt EP1 OUT as a 32-byte
  report (valid_flag0 at [0] >= 0x03 from the Linux driver; motor_right [3],
  motor_left [4], lightbar RGB [5..7]). TinyUSB delivers interrupt-OUT with
  `report_id=0` and the ID byte in the buffer, while control SET_REPORT strips
  it — `_setFeature()` normalizes both.
- **Battery**: 100% Full (wired/charging) via byte 30 = `0x1B`.
- **Unique MAC**: each board patches the `0x12` pairing blob with its factory
  identity (ESP32 `WiFi.macAddress()`, RP2040 `pico_get_unique_board_id()`,
  SAMD21 NVM calibration words at `0x0080A00C`+`0x0080A040..48`, SAMD51 user
  row `0x008061FC`/`0x00806010`, nRF52 `FICR->DEVICEID[0..1]`; unknown
  families fall back to a fixed constant) so multiple boards co-enumerate.

## Gotchas

- **ESP32 `USBHID::SendReport()` is broken** in some core 3.3.x builds — the
  library uses `Adafruit_USBD_HID::sendReport()` instead.
- **`CDCOnBoot=cdc` is mandatory** on ESP32-S3 — without it the core never
  calls `tinyusb_init()` and the HID interface is orphaned.
- **Stale Arduino cache**: `.libsdetect.d` errors → `rm -rf ~/.cache/arduino/`.
- **Linux `hid-playstation` driver**: feature report `0x81` must be served or
  the driver aborts probe.
- **Debug telemetry**: `DS4_DEBUG_SERIAL` expands to `Serial0` (ESP32-S3) or
  `Serial1` (RP2040, SAMD TinyUSB build, nRF52 — on those cores `Serial` is
  the USB CDC that `begin()` drops). This is an optional UART for test
  telemetry only — the HID device enumerates over the board's main USB; the
  telemetry UART is a separate serial port documented in `scripts/`.
- **SAMD/nRF52 `Print::printf()` does not exist** (and nRF52 `print(uint64_t)`
  is ambiguous): sketches must use `DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, fmt, ...)`
  (vsnprintf shim on SAMD/nRF52, passthrough to `printf` elsewhere) and avoid
  casting timestamps to `unsigned long long` for direct `print()` calls.
- **SAMD USB Stack menu**: with adafruit:samd's default "Arduino" stack the
  whole library compiles to nothing — sketch fails at `'DS4Gamepad' does not
  name a type`. Same failure shape as RP2040 without `usbstack=tinyusb`.
- **SAMD21 flash budget**: 256KB flash; TinyUSB + this library fits (~19% for
  examples) but keep `-Os` (the cores' default).
- **nRF52 flash budget**: 815 KB app region under SoftDevice S140 + UF2
  bootloader; this library uses 7–10%. `softdevice=s140v6` is the only valid
  FQBN option (anything else fails link).
- **nRF52 flashing**: HID-only firmware drops the CDC that bossac/nrfutil DFU
  needs, so use J-Link SWD (`loadfile Sketch.ino.hex`, device `nRF52840_xxAA`;
  the hex covers the app region only). Double-tap RESET → UF2 drive also works.
- **Stale build dirs / caches**: keep one arduino-cli `--build-path` per sketch
  (a shared dir leaves a stale `.hex` of whatever built last); odd
  `.libsdetect.d` errors → `rm -rf ~/.cache/arduino/`; profile builds resolve
  platforms into `~/.arduino15/internal/…` copies.
- **PS4 console**: this is a *PC*-style controller; console auth is impossible
  over USB.
- **Serial monitor purges boot output**: USB-UART bridges (CP210x etc.) buffer
  RX while no reader holds the tty, and the driver flushes them on first open.
  A monitor opened *after* a reset therefore misses everything the sketch
  printed during `setup()` — firmware looks silent/dead when it is fine (this
  masqueraded as "no telemetry" for a whole test session). To capture boot or
  `setup()` output, hold the port open ACROSS the reset: open with DTR/RTS
  deasserted (`dtr=False, rts=False`), then reboot by pulsing EN —
  `dtr=True; rts=False; sleep(0.15); dtr=False` (typical auto-reset circuit:
  EN low when DTR=1/RTS=0; IO0 low when RTS=1/DTR=0; both asserted = neutral).
- **Clone-CP210x wedge**: repeated pyserial opens can leave the bridge rejecting
  control requests — kernel logs `cp210x ttyUSB0: failed set request 0x12
  status: -110` and RX goes dead even though flashing still works. Fix without
  replugging: rebind the driver (`echo <if> | sudo tee
  /sys/bus/usb/drivers/cp210x/unbind`, then `/bind`; interface path from
  `readlink -f /sys/class/tty/ttyUSB0/device/../..`).
- **ESP32-S3 stuck in download mode after JTAG-serial flash**: flashing through
  native USB (`303a:1001`) may leave the board in ROM download mode despite
  esptool's "Hard resetting via RTS pin" — no USB disconnect appears in dmesg.
  Repeat esptool resets often fail too, and OpenOCD `adapter driver
  esp_usb_jtag` cannot attach while kernel `cdc_acm` binds the interface
  (unbinding does not reliably help). Press RST physically, or pulse EN via a
  wired UART bridge as above. Diagnose by listening across a reset: ROM banners
  on UART0 prove wiring is good and point at app-side issues instead.
- **ESP32-S3 boards without a UART bridge** (e.g. M5AtomS3): OTG-HID firmware
  drops CDC and USB-Serial-JTAG shares the single connector, so every flash
  needs manual download-mode entry and there is no telemetry path at all.
  Prefer boards with an onboard bridge for test runs.

## Testing

Verified SAMD/nRF52 results are recorded in `tests/BASELINE.md` — re-run and compare
against it after any library change.

### On-board unit tests

`tests/TestBasicFunctionality` compiles and runs runtime checks (buttons,
sticks, hat, triggers, touch, `releaseAll`, `send`/`ready`, LED/rumble/RGB
callbacks, `_setFeature()` paths). Build with the FQBN commands above pointed
at `tests/TestBasicFunctionality`.

`tests/TestOutputCallbacks` registers rumble/LED callbacks and waits for
host-injected output reports; host harness `scripts/test_output_packets.py`
verifies telemetry. Build + run:

```bash
arduino-cli compile -u -p <port> --fqbn "<FQBN>,UploadMode=default" \
  --library /home/pi/ArduinoDS4-tinyusb tests/TestOutputCallbacks
sudo python3 scripts/test_output_packets.py --uart <telemetry-uart>
```

`tests/LatencyBenchmark` is a tight send loop for throughput measurement.

### Host-side E2E latency test

`tests/test_e2e.py` sends commands to `BasicGamepad` over the telemetry UART
and measures input-to-evdev latency. Requires `pyserial`, `evdev`
(`tests/requirements.txt`).

```bash
cd /home/pi/ArduinoDS4-tinyusb/tests
pip install -r requirements.txt
# flash BasicGamepad first, then:
sudo python3 test_e2e.py --uart <telemetry-uart>
```

### AutoCycle pcap verification

`examples/AutoCycle/AutoCycle.ino` emits a deterministic P0..P11 stimulus cycle
with an all-buttons "marker" frame between phases. Capture the board's HID bus
with `scripts/capture_autocycle.sh` (or `scripts/latency_capture.py`) and
validate with `tests/validate_autocycle_pcap.py`:

```bash
sudo bash scripts/capture_autocycle.sh /tmp/ds4.pcap
python3 tests/validate_autocycle_pcap.py /tmp/ds4.pcap
```

### All-in-one runner

`scripts/run_tests.py` compiles the three test sketches for a given FQBN and
optionally uploads + runs the E2E harness. `--isolated <core>` (instead of
`--fqbn`) routes each compile through `scripts/build.sh`, i.e. the aventools
virtual environment with pinned cores/libraries:

```bash
python3 scripts/run_tests.py --isolated rp2040
python3 scripts/run_tests.py --isolated esp32 --upload --port /dev/ttyUSB0
```

### SAMD on-board verification (Arduino Zero cross-flash — verified)

The full USB-tier battery was run on an Arduino Zero (SAMD21G18A) using the
same cross-flash trick as the SAMDds4 reference: the Zero shares the Metro M0's
silicon and native USB pins, so the `adafruit_metro_m0:usbstack=tinyusb` build
runs unchanged. Flash with OpenOCD over EDBG (`arduino-cli upload` is broken on
both samd cores); telemetry arrives on an FTDI wired to D0/D1:

```bash
FQBN="adafruit:samd:adafruit_feather_m0:usbstack=tinyusb"   # or metro_m0
arduino-cli compile --fqbn "$FQBN" --build-path /tmp/opencode/ds4z \
  --library /home/pi/ArduinoDS4-tinyusb tests/TestBasicFunctionality
arm-none-eabi-objcopy -O binary /tmp/opencode/ds4z/*.ino.elf /tmp/opencode/ds4z/app.bin
openocd -f interface/cmsis-dap.cfg -c 'transport select swd' -f target/at91samdXX.cfg \
  -c "init" -c "reset halt" \
  -c "program /tmp/opencode/ds4z/app.bin verify 0x00002000" \
  -c "reset run" -c "shutdown"
# start the FTDI logger BEFORE reset to catch early setup() output
```

Results (2026-08-23, Zero + EDBG, telemetry `/dev/ttyUSB0` @115200):

| Test | Result |
|---|---|
| Compile battery: 5 examples + 3 tests, Feather M0 TinyUSB | all OK (17–24% flash) |
| Compile spot-check Metro M4 (SAMD51 path) | OK |
| Regression compile esp32s3 + rpipico | OK |
| `TestBasicFunctionality` (on-board) | **PASS 68 / FAIL 0** |
| `TestOutputCallbacks` + `test_output_packets.py` | **ALL TESTS PASSED**; board `PASS=50 FAIL=0 RUMBLE_PKTS=22 LED_PKTS=22` |
| `test_e2e.py` vs `BasicGamepad` (evdev latency) | **PASS**; worst p99 3.15 ms, max 3.62 ms (<10 ms threshold) |
| `AutoCycle` pcap validation (usbmon1, 670 reports) | **RESULT: PASS**, P0..P11 all PASS, 0 counter/battery errors |

Notes:
- **Grand Central M4 app base is `0x4000`** (16 KB bootloader, unlike the 8 KB
  Zero/Metro/Feather boards). Flashing a `.bin` at `0x2000` corrupts the
  bootloader's upper half — no USB enumeration, core loops in early boot code.
  Recovery + J-Link procedure: see `tests/BASELINE.md`.
- Board enumerates as `054C:05C4` ("Wireless Controller"); `hid-playstation`
  binds automatically and injects palette/rumble reports at bind time.
- Under `sudo`, arduino-cli uses root's data dir — run harnesses directly
  (`sudo python3 tests/test_e2e.py`) instead of `run_tests.py` compile+upload,
  which needs the user's core cache.

### ESP32-S3 on-board verification (dev module + CP210x bridge — verified)

Rig: generic ESP32-S3 dev module; the onboard CP210x (UART0) serves **both**
flashing and telemetry on `/dev/ttyUSB0` (sequential use only — logger and
esptool cannot share the port), while the native USB enumerates the DS4 HID.
Flashing needs no manual boot mode (auto-reset via the bridge). To see
telemetry from `setup()`, hold `/dev/ttyUSB0` open ACROSS reset (see Gotchas:
serial monitor purges boot output); harnesses that talk to a `loop()`-driven
sketch (`BasicGamepad`, `TestOutputCallbacks`) work with late-opened ports.

Results (2026-08-25, ESP32-S3 dev module, telemetry `/dev/ttyUSB0` @115200):

| Test | Result |
|---|---|
| Compile battery: 5 examples + 3 tests, isolated esp32@3.3.11 | all OK (~33–34% flash) |
| `TestBasicFunctionality` (on-board) | **PASS 68 / FAIL 0** |
| `TestOutputCallbacks` + `test_output_packets.py` | **ALL TESTS PASSED**; board `PASS=50 FAIL=0 RUMBLE_PKTS=22 LED_PKTS=22` |
| `test_e2e.py` vs `BasicGamepad` (evdev latency) | **PASS**; worst p99 2.11 ms, max 2.92 ms (<10 ms threshold) |
| `AutoCycle` pcap validation (usbmon3, 963 reports, 39 phase groups) | **RESULT: PASS**, P0..P11 all PASS, 0 counter/battery errors |
| `LatencyBenchmark` | 2652/2652 emissions, 0 sequence gaps (~4.8 s run ≈ 556 emits/s, UART-print bound) |

Matches the SAMD Zero baseline (`TestBasicFunctionality` 68/0). Board
enumerates as `054C:05C4`; `hid-playstation` binds automatically and injects
palette/rumble reports at bind time.

## License

MIT
