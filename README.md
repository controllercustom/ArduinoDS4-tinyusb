# ArduinoDS4-tinyusb

A unified Arduino library that emulates a **Sony DualShock 4-style USB gamepad**
(VID `054C`, PID `05C4`, "Wireless Controller") on four TinyUSB-capable
platforms from a single code base:

- **ESP32-S3** (ESP32 Arduino core, Adafruit TinyUSB)
- **Raspberry Pi Pico / RP2040** (arduino-pico core, TinyUSB)
- **SAMD21 / SAMD51** (Adafruit SAMD core, USB Stack = TinyUSB)
- **nRF52840** (Adafruit nRF52 core — TinyUSB always built)
- **Arduino Nano R4 / UNO R4 Minima** (Renesas `arduino:renesas_uno` core —
  requires the one-time `scripts/patch_renesas_core.sh`)

It implements the full 64-byte DualShock 4 input report, the feature reports
the Linux `hid-playstation` driver expects for enumeration, a touchpad, and
rumble / LED output-report callbacks — so the board shows up and behaves like a
real PlayStation controller on Linux, Windows, and Android.

## Features

- Full 63-byte payload + 1-byte Report ID HID input report (sticks, d-pad,
  14 buttons, 6-bit report counter, triggers, battery, 2 touch fingers).
- Feature reports `0x02`, `0x12`, `0x81`, `0xA3` for correct driver enumeration.
- Output report `0x05` (rumble + LED RGB) with `onRumble()` / `onLed()` /
  `onLedColor()` callbacks.
- Touchpad with one or two fingers (`setTouch()`).
- Always-send design (matches real hardware: every IN poll is answered with a
  full DATA report). Optional auto-send timer via `setPollInterval(ms)`
  (cooperative on SAMD/nRF52 — see Notes).
- Single `DS4Gamepad` class; platform differences are handled internally.

## Supported Boards

| Board family        | Core / FQBN piece            | USB stack        |
|---------------------|------------------------------|------------------|
| ESP32-S3            | `esp32:esp32:esp32s3`        | Adafruit TinyUSB (`USBMode=default`) |
| Raspberry Pi Pico   | `rp2040:rp2040:rpipico`      | TinyUSB (`usbstack=tinyusb`) |
| SAMD21 (M0) / SAMD51 (M4) | `adafruit:samd:adafruit_feather_m0` / `adafruit:samd:adafruit_grandcentral_m4` | TinyUSB (`usbstack=tinyusb`) |
| nRF52840            | `adafruit:nrf52:feather52840` | TinyUSB (always built; pick SoftDevice S140 6.1.1) |
| Nano R4 / UNO R4 Minima | `arduino:renesas_uno:nanor4` / `:minima` | TinyUSB (patched core; see below) |

### Renesas Nano R4 / UNO R4 Minima (`arduino:renesas_uno`)

The stock core cannot present custom HID descriptors or the Sony VID/PID, so
run the bundled patch script **once** after installing the core:

```bash
scripts/patch_renesas_core.sh        # Linux; .macos.sh / .windows.ps1 siblings
```

Then compile with `DISABLE_USB_SERIAL` so the HID gamepad is interface 0:

```bash
arduino-cli compile --fqbn arduino:renesas_uno:nanor4 \
  --build-property compiler.cpp.extra_flags=-DDISABLE_USB_SERIAL \
  --library ~/ArduinoDS4-tinyusb examples/BasicGamepad
```

Upload via double-tap reset (DFU). Output reports travel over control
SET_REPORT — identical to real DS4-v1 USB hardware. Test telemetry uses
`Serial1` on D0/D1.

## Installation (Arduino IDE)

1. In the Arduino IDE choose **Sketch → Include Library → Add .ZIP Library…**
   and select this folder (or a ZIP of it), **or** copy it into your
   `libraries/` folder (e.g. `~/Arduino/libraries/ArduinoDS4-tinyusb`).
2. **ESP32-S3 only:** install **Adafruit TinyUSB Library** via
   **Tools → Manage Libraries…** (search "Adafruit TinyUSB"). The RP2040,
   SAMD and nRF52 cores bundle their own copy — nothing to install there.
3. Restart the IDE.
4. Open an example from **File → Examples → ArduinoDS4-tinyusb**.

No extra libraries are required beyond the above: every supported board core
ships its own TinyUSB stack, but on ESP32 the Adafruit Arduino-API wrapper is
a separate install (the library uses `Adafruit_USBD_HID` as its unified USB
layer on all four platforms).

## Quick Start

Open **Examples → ArduinoDS4-tinyusb → BasicGamepad**, select your board, and
upload. The board enumerates as a DualShock 4 controller; the example drives a
few inputs so you can see it respond in a gamepad tester.

Minimal sketch:

```cpp
#include <DS4Gamepad.h>

DS4Gamepad gamepad;

void setup() {
  gamepad.begin();
}

void loop() {
  gamepad.press(DS4_BTN_CROSS);
  gamepad.setStickLeft(0, 100);
  gamepad.send();
  delay(100);
  gamepad.releaseAll();
  gamepad.send();
  delay(100);
}
```

## API

```cpp
#include <DS4Gamepad.h>

DS4Gamepad gamepad;

gamepad.begin();                 // start USB HID
gamepad.ready();                 // true once mounted to a host

// Buttons (DS4_BTN_*): SQUARE, CROSS, CIRCLE, TRIANGLE, L1, R1, L2, R2,
//                       SHARE, OPTIONS, L3, R3, PS, TOUCHPAD
gamepad.press(DS4_BTN_CROSS);
gamepad.release(DS4_BTN_CROSS);
gamepad.setButton(DS4_BTN_CROSS, true);
gamepad.releaseAll();

// Sticks: signed -32768..32767 (mapped to the 8-bit HID range)
gamepad.setStickLeft(int16_t x, int16_t y);
gamepad.setStickRight(int16_t x, int16_t y);

// D-pad (DS4_HAT_*): UP, UP_RIGHT, RIGHT, DOWN_RIGHT, DOWN,
//                    DOWN_LEFT, LEFT, UP_LEFT, CENTERED
gamepad.setDpad(DS4_HAT_UP);
gamepad.setHat(DS4_HAT_UP);      // alias

// Triggers: unsigned 0..32768
gamepad.setLeftTrigger(uint16_t v);
gamepad.setRightTrigger(uint16_t v);

// Touchpad: finger 0 or 1, down flag, 0..1919 x 0..942
gamepad.setTouch(uint8_t finger, bool down, uint16_t x, uint16_t y);

// Output-report callbacks (host -> device)
gamepad.onRumble([](uint8_t left, uint8_t right) { /* ... */ });
gamepad.onLed([](uint8_t red) { /* ... */ });
gamepad.onLedColor([](const DS4Gamepad::DS4LED &c) { /* ... */ });

// Send / timing
gamepad.send();                  // explicit report (required if poll disabled)
gamepad.setPollInterval(0);      // 0 = disabled (explicit send); ms = auto-send
```

### Output reports

When the host sends output report `0x05` (rumble + LED), the registered
callbacks fire:

- `onRumble(uint8_t leftMotor, uint8_t rightMotor)`
- `onLed(uint8_t lightbarRed)`
- `onLedColor(const DS4LED &)` where `DS4LED { uint8_t r, g, b; }`

## Isolated Builds

`scripts/avenv.sh` (the aventools virtual environment) gives each build its own
cores/libraries/downloads/build-cache, so concurrent and reproducible builds
never clobber each other. `scripts/build.sh` wraps it for this library and
selects a core:

```bash
# Usage: scripts/build.sh <core> <sketch_dir>   (core ∈ esp32|rp2040|samd|nrf52)
./scripts/build.sh esp32 examples/BasicGamepad
./scripts/build.sh rp2040 tests/TestBasicFunctionality
./scripts/build.sh samd   examples/AutoCycle
./scripts/build.sh nrf52  examples/SimpleButtons

# Build every example on every core:
for c in esp32 rp2040 samd nrf52; do
  for ex in examples/*/; do scripts/build.sh "$c" "${ex%/}"; done
done
```

`build.sh` pins each core version (`esp32:esp32@3.3.11`, `rp2040:rp2040@6.0.0`,
`adafruit:samd@1.7.17`, `adafruit:nrf52@1.7.0`) and compiles with `--library`.
ESP32-S3 is the only core that does **not** bundle TinyUSB, so `build.sh`
additionally installs `Adafruit TinyUSB Library@3.7.7` for esp32 builds; the
other cores bundle it in their platform. Versions are also recorded in
`sketch.yaml`.

Speed up builds by reusing the existing installed toolchain cache:

```bash
export AVENV_GOLDEN="$HOME/.arduino15"
./scripts/build.sh esp32 examples/BasicGamepad   # first run caches core; later reuse it
```

Dependency-fail-fast: the isolated `user/libraries` starts empty, so a missing
or unpinned dependency fails the build.

## Examples

- **AutoCycle** — deterministic P0..P11 stimulus sweep used for host-side
  packet / pcap verification.
- **BasicGamepad** — simplest possible gamepad.
- **FullController** — exercises every input type.
- **JoystickTest** — stick dead-zone / range demonstration.
- **SimpleButtons** — button-press demonstration.

## Notes

- **VID/PID**: `054C:05C4` (Sony). **USB strings**:
  "Sony Computer Entertainment" / "Wireless Controller".
- **Battery** is reported as 100% Full (wired) via byte 30 = `0x1B`.
- On Linux the `hid-playstation` driver binds automatically; feature reports
  are served so enumeration succeeds.
- This is a **PC / Android**-style PlayStation controller. Connecting to an
  actual PS4 console requires Bluetooth link-key authentication that cannot be
  performed over USB.
- **SAMD**: `Tools ▸ USB Stack` **must** be set to TinyUSB — with the default
  "Arduino" stack the library compiles to nothing and sketches fail with
  `'DS4Gamepad' does not name a type`. `begin()` drops the USB CDC, so debug
  telemetry goes to the hardware UART (`DS4_DEBUG_SERIAL` = `Serial1`, D1=TX).
- **SAMD / nRF52**: auto-send (`setPollInterval`) is *cooperative* — there is
  no OS timer on these cores, so due sends are flushed from the setters and
  `ready()`; default remains disabled and explicit `send()` always works.
- Each board patches its unique identity into the pairing report: ESP32 factory
  WiFi MAC, RP2040 flash ID, SAMD NVM-calibration serial number, nRF52840 FICR
  DEVICEID — multiple boards co-enumerate without being rejected as duplicates.

## Testing

The `tests/` directory contains on-board unit tests and host-side Python
harnesses (see `AGENTS.md` and the `scripts/` directory for maintainer
workflows). Normal library users do not need them.

## License

MIT — see `LICENSE`.
