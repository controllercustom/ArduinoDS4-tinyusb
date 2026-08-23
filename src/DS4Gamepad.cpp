// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// Unified DS4 (DualShock 4-style) gamepad emulation.
// Supports ESP32-S3 (esp32 core), RP2040/Pico (arduino-pico core),
// SAMD21/SAMD51 (adafruit:samd core, USB Stack = TinyUSB) and
// nRF52840 (adafruit:nrf52 core — always builds TinyUSB).

#if (defined(ARDUINO_ARCH_RP2040) && defined(USE_TINYUSB)) || \
    (defined(ARDUINO_ARCH_SAMD) && defined(USE_TINYUSB)) || \
    (defined(ARDUINO_ARCH_NRF52) && defined(USE_TINYUSB)) || \
    defined(ARDUINO_ARCH_ESP32)

#include "DS4Gamepad.h"
#include <cstring>
#include "tusb.h"

#if defined(ARDUINO_ARCH_RP2040)
#include <pico/unique_id.h>
#elif defined(ARDUINO_ARCH_SAMD)
// Unique id is read directly from the NVM calibration rows (below); no extra
// includes required.
#elif defined(ARDUINO_ARCH_NRF52)
// Unique id comes from FICR->DEVICEID (via Arduino.h -> nrf.h); no extra
// includes required.
#else
#include <WiFi.h>
#include <esp_timer.h>
#endif

// Singleton for static callback forwarding.
static DS4Gamepad *s_instance = nullptr;

#if defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
// Read the chip's unique identity into a 6-byte MAC-style array.
//
// SAMD serial-number locations (NVM calibration rows, always readable):
//   SAMD21: words at 0x0080A00C and 0x0080A040..0x0080A048 (16 bytes total)
//   SAMD51: words at 0x008061FC and 0x00806010 (same source the adafruit:samd
//           core uses for its USB serial string)
// nRF52: FICR->DEVICEID[0..1] (64-bit factory unique; readable even with the
//   SoftDevice enabled). DEVICEADDR would be the alternative, DEVICEID is
//   canonical.
// Unknown families fall back to the static blob's default identity so any
// supported variant still builds and enumerates.
static void ds4ReadUniqueMac(uint8_t mac[6]) {
#if defined(__SAMD21__)
  const volatile uint32_t *sn_a = (const volatile uint32_t *)0x0080A00C;
  const volatile uint32_t *sn_b = (const volatile uint32_t *)0x0080A040;
  uint32_t w0 = sn_a[0];
  uint32_t w1 = sn_b[0];
#elif defined(__SAMD51__)
  const volatile uint32_t *sn_a = (const volatile uint32_t *)0x008061FC;
  const volatile uint32_t *sn_b = (const volatile uint32_t *)0x00806010;
  uint32_t w0 = sn_a[0];
  uint32_t w1 = sn_b[0];
#elif defined(NRF52840_XXAA) || defined(NRF52_SERIES)
  uint32_t w0 = NRF_FICR->DEVICEID[0];
  uint32_t w1 = NRF_FICR->DEVICEID[1];
#else
  uint32_t w0 = 0x6D66078BUL;   // fallback identity (matches rep_12 defaults)
  uint32_t w1 = 0x251C0866UL;
#endif
  mac[0] = (uint8_t)(w0 & 0xFF);
  mac[1] = (uint8_t)((w0 >> 8) & 0xFF);
  mac[2] = (uint8_t)((w0 >> 16) & 0xFF);
  mac[3] = (uint8_t)((w0 >> 24) & 0xFF);
  mac[4] = (uint8_t)(w1 & 0xFF);
  mac[5] = (uint8_t)((w1 >> 8) & 0xFF);
}
#endif /* ARDUINO_ARCH_SAMD || ARDUINO_ARCH_NRF52 */

// 0x12: pairing info / MAC (report id + 15 payload bytes). The 6 MAC bytes at
// indices 1..6 are patched in begin() from a per-board unique id (ESP32 factory
// WiFi MAC, RP2040 flash unique ID, SAMD NVM-calibration serial number, or
// nRF52 FICR DEVICEID) so two boards present DISTINCT identities to the host.
static uint8_t rep_12[] = {
  0x12,0x8B,0x09,0x07,0x6D,0x66,0x1C,0x08,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// Microsecond timestamp helper (telemetry only).
#if defined(ARDUINO_ARCH_RP2040)
uint64_t ds4_micros() { return time_us_64(); }
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
uint64_t ds4_micros() { return (uint64_t)micros(); }
#else
uint64_t ds4_micros() { return (uint64_t)esp_timer_get_time(); }
#endif

// ====================================================================
// HID Report Descriptor — PlayStation style gamepad, Report ID 1.
//
// The input-report layout mirrors a PlayStation controller (64-byte report):
//   [0]  report id (added by the stack via sendReport)
//   [1..4]  X, Y, Z, Rz  (sticks, unsigned 0..255, 128 = center)
//   [5]  d-pad low nibble (0..7 dir, 8 = null) + face buttons high nibble
//   [6]  L1 R1 L2 R2 Share Options L3 R3
//   [7]  counter (6 bits) + T-Pad click + PS
//   [8..9]  Rx, Ry (L2/R2 analog triggers, unsigned 0..255)
//   [10..63] vendor-defined: timestamp, gyro/accel, battery, touchpad, reserved
//
// This is the standard PlayStation-style gamepad descriptor. The
// input report is 63 payload bytes (64 incl. the report id).
// ====================================================================
static const uint8_t gp_report_desc[] = {
  // ---- Input report (ID 1) : 63-byte DS4 payload ----
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x09, 0x32,        //   Usage (Z)
  0x09, 0x35,        //   Usage (Rz)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x04,        //   Report Count (4)
  0x81, 0x02,        //   Input (Data,Var,Abs)         -> bytes 1..4 (sticks)
  0x09, 0x39,        //   Usage (Hat switch)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x07,        //   Logical Maximum (7)
  0x35, 0x00,        //   Physical Minimum (0)
  0x46, 0x3B, 0x01,  //   Physical Maximum (315)
  0x65, 0x14,        //   Unit (Eng Rot: Degrees)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x42,        //   Input (Data,Var,Abs,Null)    -> byte 5 low nibble (d-pad)
  0x65, 0x00,        //   Unit (None)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (Button 1)
  0x29, 0x0E,        //   Usage Maximum (Button 14)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x0E,        //   Report Count (14)
  0x81, 0x02,        //   Input (Data,Var,Abs)         -> byte5 hi nibble + byte6 + byte7 bits0..1
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x20,        //   Usage (Counter)
  0x75, 0x06,        //   Report Size (6)
  0x95, 0x01,        //   Report Count (1)
   0x15, 0x00,        //   Logical Minimum (0)
   0x25, 0x7F,        //   Logical Maximum (127)  <- matches original PlayStation gamepad descriptor (spec-bug, 6-bit field)
   0x81, 0x02,        //   Input (Data,Var,Abs)         -> byte 7 bits 2..7 (counter)
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x33,        //   Usage (Rx)
  0x09, 0x34,        //   Usage (Ry)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data,Var,Abs)         -> bytes 8..9 (L2/R2 triggers)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x21,        //   Usage (0x21)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x36,        //   Report Count (54)
  0x81, 0x02,        //   Input (Data,Var,Abs)         -> bytes 10..63 (imu/touch/reserved)
  0xC0,               // End Collection

  // ---- Feature report (ID 0x02) : calibration, 36 bytes payload ----
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
  0x85, 0x02,        //   Report ID (2)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x24,        //   Usage (0x24)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x24,        //   Report Count (36)
  0xB2, 0x02, 0x01,  //   Feature (Volatile)
  0xC0,               // End Collection

  // ---- Feature report (ID 0x12) : pairing info / MAC, 15 bytes payload ----
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
  0x85, 0x12,        //   Report ID (0x12)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x26,        //   Usage (0x26)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x0F,        //   Report Count (15)
  0xB2, 0x02, 0x01,  //   Feature (Volatile)
  0xC0,               // End Collection

  // ---- Output report (ID 0x05) : rumble + LED, 31 bytes payload ----
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
  0x85, 0x05,        //   Report ID (5)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x22,        //   Usage (0x22)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x1F,        //   Report Count (31)
  0x91, 0x02,        //   Output (Data,Var,Abs)
  0xC0,               // End Collection

  // ---- Feature report (ID 0x81) : firmware info, 48 bytes payload ----
  0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
  0x85, 0x81,        //   Report ID (0x81)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor Defined 0xFF00)
  0x09, 0x27,        //   Usage (0x27)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x30,        //   Report Count (48)
  0xB2, 0x02, 0x01,  //   Feature (Volatile)
  0xC0,               // End Collection
};

// Byte offsets WITHIN the SendReport payload (report id is prepended by the
// stack, so payload[0] corresponds to byte [1]).
enum {
  OFF_LX      = 0,   // report [1]
  OFF_LY      = 1,   // report [2]
  OFF_RX      = 2,   // report [3]
  OFF_RY      = 3,   // report [4]
  OFF_BTN0    = 4,   // report [5]  d-pad + face
  OFF_BTN1    = 5,   // report [6]  shoulders/menu/sticks
  OFF_BTN2    = 6,   // report [7]  counter + T-Pad click + PS
  OFF_L2      = 7,   // report [8]  L2 trigger
  OFF_R2      = 8,   // report [9]  R2 trigger
  OFF_TS      = 9,   // report [10..11] timestamp
  OFF_BATT    = 29,  // report [30] battery/cable-state
  OFF_TPADACT = 32,  // report [33] touch packets active
  OFF_TPACKN  = 33,  // report [34] touch packet counter
  OFF_F1      = 34,  // report [35..38] finger #1
  OFF_F2      = 38,  // report [39..42] finger #2
  PAYLOAD_LEN = 63   // report [1..63]
};

DS4Gamepad::DS4Gamepad()
  : buttonMask(0), hat(DS4_HAT_CENTERED), touchId(0), touchCounter(0),
    timestamp(0), reportCounter(0), ledStateValue(0) {
  axes[0] = 128;  // LX
  axes[1] = 128;  // LY
  axes[2] = 128;  // RX
  axes[3] = 128;  // RY
  axes[4] = 0;    // L2 trigger
  axes[5] = 0;    // R2 trigger
  memset(touch, 0, sizeof(touch));
  if (!s_instance) s_instance = this;
}

void DS4Gamepad::begin() {
  // Patch the pairing blob with a per-board unique id so multiple boards
  // present DISTINCT identities and are not rejected as duplicates by the
  // Linux hid-playstation driver.
  {
    uint8_t base[6] = {0};
#if defined(ARDUINO_ARCH_RP2040)
    pico_unique_board_id_t uid;
    pico_get_unique_board_id(&uid);
    memcpy(base, uid.id, 6);
#elif defined(ARDUINO_ARCH_SAMD)
    ds4ReadUniqueMac(base);
#elif defined(ARDUINO_ARCH_NRF52)
    ds4ReadUniqueMac(base);
#else
    WiFi.macAddress(base);
#endif
    rep_12[1] = base[0] | 0x02;  // set locally-administered bit
    rep_12[2] = base[1];
    rep_12[3] = base[2];
    rep_12[4] = base[3];
    rep_12[5] = base[4];
    rep_12[6] = base[5];
  }

#if defined(ARDUINO_ARCH_RP2040)
  // The arduino-pico core already initialized TinyUSB (in main.cpp) and added a
  // CDC interface (Serial). Drop the CDC so the host sees ONLY the HID
  // gamepad — matches a real PlayStation controller (no serial CDC). Serial.end()
  // calls TinyUSBDevice.clearConfiguration(), which resets VID/PID/strings, so
  // re-apply all descriptor fields afterwards. Do NOT call TinyUSBDevice.begin().
  Serial.end();
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
  // The adafruit:samd and adafruit:nrf52 cores initialize TinyUSB at boot
  // (TinyUSB_Device_Init in the core's main.cpp; on nrf52 there is no USB
  // Stack menu — TinyUSB is always built) and expose the CDC interface as
  // Serial. Drop the CDC so the host sees ONLY the HID gamepad (matches the
  // original PlayStation controller, which has no serial CDC). Re-apply all
  // descriptor fields afterwards. Do NOT call TinyUSBDevice.begin() — the
  // core already initialized the stack.
  Serial.end();
#else
  // The ESP32-S3 core only starts USB-OTG when a USB interface is enabled at
  // boot. The build must enable TinyUSB/CDC so the core calls tinyusb_init().
  // Then drop the auto-added CDC-ACM interface so the host sees only the HID
  // gamepad.
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }
  TinyUSBDevice.clearConfiguration();
#endif

  TinyUSBDevice.setID(0x054C, 0x05C4);                       // PlayStation style controller
  TinyUSBDevice.setManufacturerDescriptor("Sony Computer Entertainment");
  TinyUSBDevice.setProductDescriptor("Wireless Controller");

  hid.enableOutEndpoint(true);
  hid.setPollInterval(4);
  hid.setReportDescriptor(gp_report_desc, sizeof(gp_report_desc));
  hid.setReportCallback(_getReportCb, _setReportCb);
  hid.setStringDescriptor("Wireless Controller");
  hid.begin();

  _connected = true;

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

#if defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
  // Reset the cooperative auto-send cadence so a poll interval configured
  // before begin() does not fire an immediate burst after mount.
  _lastAutoSendMs = millis();
#endif

  // Start auto-send timer if poll interval is configured.
  if (_pollIntervalMs > 0) {
    setPollInterval(_pollIntervalMs);
  }
}

// ---- Handshake / feature reports the PS4/PC may query -----------------------

uint16_t DS4Gamepad::_getReportCb(uint8_t report_id, hid_report_type_t report_type,
                                 uint8_t *buffer, uint16_t reqlen) {
  if (!s_instance) return 0;
  return s_instance->_getFeature(report_id, report_type, buffer, reqlen);
}

uint16_t DS4Gamepad::_getFeature(uint8_t report_id, hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
  if (report_type != HID_REPORT_TYPE_FEATURE) return 0;

  // 0xA3: manufacture date/time string identifier
  static const uint8_t rep_a3[] = {
    0xA3,0x41,0x75,0x67,0x20,0x20,0x33,0x20,0x32,0x30,0x31,0x33,0x00,0x00,0x00,0x00,
    0x00,0x30,0x37,0x3A,0x30,0x31,0x3A,0x31,0x32,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00,0x31,0x03,0x00,0x00,0x00,0x49,0x00,0x05,0x00,0x00,0x80,0x03,0x00
  };
  // 0x02: IMU calibration (report id + 36 payload bytes)
  static const uint8_t rep_02[] = {
    0x02,0x01,0x00,0x00,0x00,0x00,0x00,0x87,0x22,0x7B,0xDD,0xB2,0x22,0x47,0xDD,0xBD,
    0x22,0x43,0xDD,0x1C,0x02,0x1C,0x02,0x7F,0x1E,0x2E,0xDF,0x60,0x1F,0x4C,0xE0,0x3A,
    0x1D,0xC6,0xDE,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  };
  // 0x81: firmware info (report id + 48 payload bytes)
  static const uint8_t rep_81[] = {
    0x81,0x00,0x01,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
  };

  const uint8_t *src = nullptr; uint16_t n = 0;
  switch (report_id) {
    case 0xA3: src = rep_a3; n = sizeof(rep_a3); break;
    case 0x02: src = rep_02; n = sizeof(rep_02); break;
    case 0x12: src = rep_12; n = sizeof(rep_12); break;
    case 0x81: src = rep_81; n = sizeof(rep_81); break;
    default: break;
  }
  if (src && n) {
    uint16_t ncpy = (n > reqlen) ? reqlen : n;
    memcpy(buffer, src, ncpy);
    if (reqlen > ncpy) memset(buffer + ncpy, 0, reqlen - ncpy);
    return reqlen;
  }
  if (reqlen) memset(buffer, 0, reqlen);
  return reqlen;
}

void DS4Gamepad::_setReportCb(uint8_t report_id, hid_report_type_t report_type,
                             uint8_t const *buffer, uint16_t bufsize) {
  if (!s_instance) return;
  s_instance->_setFeature(report_id, report_type, buffer, bufsize);
}

void DS4Gamepad::_setFeature(uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
  if (report_type != HID_REPORT_TYPE_OUTPUT) return;

  // TinyUSB delivers output reports via two paths:
  //  - interrupt OUT (how Linux hid-playstation actually sends): report_id is
  //    reported as 0 and the Report ID byte is IN the buffer (buffer[0]==0x05)
  //  - SET_REPORT control: report_id is parsed and the stack strips the ID byte
  // Normalize both to "p points at the payload right after the Report ID".
  uint8_t const *p = buffer;
  uint16_t n = bufsize;
  if (report_id == 0) {
    if (n < 1 || p[0] != 0x05) return;
    p++;
    n--;
  } else if (report_id != 0x05) {
    return;
  }

  // PlayStation gamepad output report payload (Report ID removed):
  //   [0]=valid_flags [1]=flags [2]=rsvd [3]=motor_right
  //   [4]=motor_left  [5]=lightbar_red [6]=lightbar_green [7]=lightbar_blue
  if (n >= 8) {
    ledColorValue = {p[5], p[6], p[7]};
    ledColorReceived = true;
    ledStateValue = p[5];                       // red channel, existing
    if (ledCallback) ledCallback(ledStateValue);            // existing
    if (ledColorReceived && ledColorCallback) ledColorCallback(ledColorValue);  // RGB callback
    // Callback contract is (leftMotor, rightMotor); the report stores
    // motor_right first (offset 3) then motor_left (offset 4).
    if (rumbleCallback) rumbleCallback(p[4], p[3]);         // existing
  } else if (n >= 7) {
    ledStateValue = p[5];                       // red channel, truncated payload fallback
    if (ledCallback) ledCallback(ledStateValue);            // existing
    // Callback contract is (leftMotor, rightMotor); the report stores
    // motor_right first (offset 3) then motor_left (offset 4).
    if (rumbleCallback) rumbleCallback(p[4], p[3]);         // existing
  }
}

static void packFinger(uint8_t *dst, const DS4Touch &t) {
   // State byte: id while down, 0x80|id while lifted (verified against original
   // PlayStation controller captures). Coordinates keep the last contact position
   // on lift — the real hardware does NOT zero them.
  dst[0] = t.down ? (t.id & 0x7F) : (0x80 | (t.id & 0x7F));
  uint32_t packed = ((uint32_t)t.x & 0x0FFF) | (((uint32_t)t.y & 0x0FFF) << 12);
  dst[1] = packed & 0xFF;
  dst[2] = (packed >> 8) & 0xFF;
  dst[3] = (packed >> 16) & 0xFF;
}

bool DS4Gamepad::sendGamepadReport() {
  if (!tud_mounted()) return false;
  // Arm mount tracking so send() works even if the app never polls isConnected().
  _refreshMount();
  // Guard: don't write until USB is settled after mount.
  unsigned long elapsed = (unsigned long)(millis() - _mountedAt);
  if (!_usbReady || elapsed < 100UL) return false;

  uint8_t report[PAYLOAD_LEN];
  memset(report, 0, sizeof(report));

  report[OFF_LX] = axes[0];
  report[OFF_LY] = axes[1];
  report[OFF_RX] = axes[2];
  report[OFF_RY] = axes[3];

  // byte 5: d-pad (low nibble) + face buttons (high nibble)
  uint8_t b0 = (hat & 0x0F);
  if (buttonMask & (1u << DS4_BTN_SQUARE))   b0 |= 0x10;
  if (buttonMask & (1u << DS4_BTN_CROSS))    b0 |= 0x20;
  if (buttonMask & (1u << DS4_BTN_CIRCLE))   b0 |= 0x40;
  if (buttonMask & (1u << DS4_BTN_TRIANGLE)) b0 |= 0x80;
  report[OFF_BTN0] = b0;

  // byte 6: L1 R1 L2 R2 Share Options L3 R3
  uint8_t b1 = 0;
  if (buttonMask & (1u << DS4_BTN_L1))      b1 |= 0x01;
  if (buttonMask & (1u << DS4_BTN_R1))      b1 |= 0x02;
  if (buttonMask & (1u << DS4_BTN_L2))      b1 |= 0x04;
  if (buttonMask & (1u << DS4_BTN_R2))      b1 |= 0x08;
  if (buttonMask & (1u << DS4_BTN_SHARE))   b1 |= 0x10;
  if (buttonMask & (1u << DS4_BTN_OPTIONS)) b1 |= 0x20;
  if (buttonMask & (1u << DS4_BTN_L3))      b1 |= 0x40;
  if (buttonMask & (1u << DS4_BTN_R3))      b1 |= 0x80;
  report[OFF_BTN1] = b1;

  // byte 7: counter (bits 2..7) + T-Pad click (bit1) + PS (bit0).
  // The 6-bit counter increments once per report (verified against original
  // PlayStation controller captures: byte[7] steps +4 each frame, independent of
  // the timestamp bytes).
  uint8_t b2 = 0;
  if (buttonMask & (1u << DS4_BTN_PS))       b2 |= 0x01;
  if (buttonMask & (1u << DS4_BTN_TOUCHPAD)) b2 |= 0x02;
  b2 |= (uint8_t)((reportCounter & 0x3F) << 2);
  report[OFF_BTN2] = b2;

  report[OFF_L2] = axes[4];
  report[OFF_R2] = axes[5];

  // timestamp (bytes 10..11): free-running vendor counter. Original PlayStation
  // controller captures show it stepping ~698-816 per report (variable); we use a
  // fixed 188 which is functionally equivalent since no driver consumes this field.
  report[OFF_TS]     = timestamp & 0xFF;
  report[OFF_TS + 1] = (timestamp >> 8) & 0xFF;

  // battery: 0x1B = cable connected, level 11 => 100% Full
  report[OFF_BATT] = 0x1B;

  // touchpad. Byte [33] is the touch-packet count and is ALWAYS 1 on a real
  // PlayStation controller (even with no finger down). Finger up/down is
  // signalled by the state-byte bit 7 in [35]/[39]. Bytes report[44] and
  // report[49] carry two additional ALWAYS-EMPTY touch slots (state byte 0x80,
  // coords 0) on the original hardware — emitted to match byte-for-byte.
  report[OFF_TPADACT] = 0x01;
  report[OFF_TPACKN]  = touchCounter;
  packFinger(&report[OFF_F1], touch[0]);
  packFinger(&report[OFF_F2], touch[1]);
  report[43] = 0x80;  // empty touch slot [44]
  report[48] = 0x80;  // empty touch slot [49]

  timestamp += 188;

  bool sent = hid.sendReport(1, report, sizeof(report));
  // Advance the 6-bit counter only on a successful queue. If the endpoint is
  // busy (e.g. an explicit send() collides with the auto-send timer), a failed
  // send must NOT advance the counter — otherwise the on-wire sequence skips,
  // unlike the original PlayStation controller, which answers every 4ms poll
  // with a continuous counter.
  if (sent) {
    reportCounter = (reportCounter + 1) & 0x3F;
  }
  return sent;
}

// Always-send design (matches original PlayStation controller): every report is
// transmitted even if it duplicates the previous one. The original hardware
// answers every 4ms IN poll with a full DATA report and never NAKs, so there is
// no dirty-flag suppression.
bool DS4Gamepad::send() {
  return sendGamepadReport();
}

void DS4Gamepad::press(uint8_t btn) {
  _pumpAutoSend();
  if (btn >= DS4_BTN_COUNT) return;
  buttonMask |= (1u << btn);
}

void DS4Gamepad::release(uint8_t btn) {
  _pumpAutoSend();
  if (btn >= DS4_BTN_COUNT) return;
  buttonMask &= ~(1u << btn);
}

void DS4Gamepad::setButton(uint8_t btn, bool pressed) {
  if (pressed) press(btn);
  else release(btn);
}

bool DS4Gamepad::getButton(uint8_t btn) const {
  if (btn >= DS4_BTN_COUNT) return false;
  return (buttonMask & (1u << btn)) != 0;
}

static inline uint8_t mapStick(int16_t val) {
  int32_t v = static_cast<int32_t>(val + 32768);
  if (v < 0) return 0U;
  if (v > 65535) return 255U;
  return static_cast<uint8_t>((v * 255U + 32768U) / 65536U);
}

void DS4Gamepad::setStickLeft(int16_t x, int16_t y) {
  _pumpAutoSend();
  axes[0] = mapStick(x);
  axes[1] = mapStick(y);
}

void DS4Gamepad::setStickRight(int16_t x, int16_t y) {
  _pumpAutoSend();
  axes[2] = mapStick(x);
  axes[3] = mapStick(y);
}

void DS4Gamepad::setHat(uint8_t h) {
  _pumpAutoSend();
  hat = h;
}

void DS4Gamepad::setDpad(uint8_t dir) {
  setHat(dir);
}

uint8_t DS4Gamepad::getHat() const {
  return hat;
}

void DS4Gamepad::setLeftTrigger(uint16_t value) {
  _pumpAutoSend();
  axes[4] = (value > 32768U) ? 255U : (uint8_t)(value * 255U / 32768U);
}

void DS4Gamepad::setRightTrigger(uint16_t value) {
  _pumpAutoSend();
  axes[5] = (value > 32768U) ? 255U : (uint8_t)(value * 255U / 32768U);
}

void DS4Gamepad::setTouch(uint8_t finger, bool down, uint16_t x, uint16_t y) {
  _pumpAutoSend();
  if (finger > 1) return;
  if (down && !touch[finger].down) {
    touch[finger].id = touchId++ & 0x7F;
  }
  // Only update coordinates while down; on lift (down=false) keep the last
  // contact position so the report matches original PlayStation controller
  // behavior (which retains it).
  if (down) {
    if (x > DS4_TOUCH_MAX_X) x = DS4_TOUCH_MAX_X;
    if (y > DS4_TOUCH_MAX_Y) y = DS4_TOUCH_MAX_Y;
    touch[finger].x = x;
    touch[finger].y = y;
  }
  touch[finger].down = down;
  touchCounter++;
}

void DS4Gamepad::releaseAll() {
  _pumpAutoSend();
  buttonMask = 0;
  axes[0] = 128; axes[1] = 128;
  axes[2] = 128; axes[3] = 128;
  axes[4] = 0;   axes[5] = 0;
  hat = DS4_HAT_CENTERED;
  touch[0].down = false;
  touch[1].down = false;
}

bool DS4Gamepad::ready() {
  _pumpAutoSend();
  return hid.ready();
}

void DS4Gamepad::_refreshMount() const {
  if (!_connected) return;
  bool mounted = tud_mounted();

  // Detect disconnect — reset mount tracking so re-connect gets a fresh settling window.
  if (!mounted && _usbReady) {
    _usbReady = false;
  }

  if (mounted && !_usbReady) {
    _mountedAt = millis();
    _usbReady = true;
  }
}

bool DS4Gamepad::isConnected() const {
  if (!_connected) return false;
  _refreshMount();
  // Allow writes only after a short settling period post-mount.
  if (_usbReady && ((unsigned long)(millis() - _mountedAt)) < 100UL) return false;
  return tud_mounted();
}

uint8_t DS4Gamepad::getLEDState() const {
  return ledStateValue;
}

DS4Gamepad::DS4LED DS4Gamepad::getLEDColor() const {
  if (!ledColorReceived) return DS4Gamepad::DS4LED{0xFF, 0xFF, 0xFF}; // never-set sentinel
  return ledColorValue;
}

void DS4Gamepad::onLed(LedCallback cb) {
  ledCallback = std::move(cb);
}

void DS4Gamepad::onRumble(RumbleCallback cb) {
  rumbleCallback = std::move(cb);
}

void DS4Gamepad::onLedColor(LedColorCallback cb) {
  ledColorCallback = std::move(cb);
}

uint32_t DS4Gamepad::setPollInterval(uint32_t ms) {
  uint32_t oldMs = _pollIntervalMs;
#if defined(ARDUINO_ARCH_RP2040)
  if (_timerStarted) {
    cancel_repeating_timer(&_timer);
    _timerStarted = false;
  }
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
  // Cooperative auto-send: nothing to cancel.
#else
  if (_timerHandle != nullptr) {
    esp_timer_stop(_timerHandle);
    esp_timer_delete(_timerHandle);
    _timerHandle = nullptr;
  }
#endif

  // 0 disables auto-poll. Enforce minimum floor for non-zero values to prevent
  // runaway timers with small intervals (1-3ms).
  if (ms > 0U && ms < 4U) {
    _pollIntervalMs = 4U;
  } else {
    _pollIntervalMs = ms;
  }

  if (_pollIntervalMs > 0) {
#if defined(ARDUINO_ARCH_RP2040)
    _timerStarted = add_repeating_timer_ms(static_cast<int32_t>(_pollIntervalMs),
                                           &DS4Gamepad::_timerCallback,
                                           this, &_timer);
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
    // Reset the cadence so switching intervals does not fire an immediate burst.
    _lastAutoSendMs = millis();
#else
    esp_timer_create_args_t timerArgs = {};
    timerArgs.callback = &DS4Gamepad::_timerCallback;
    timerArgs.arg = this;
    timerArgs.dispatch_method = ESP_TIMER_TASK;
    timerArgs.name = "ds4_poll";

    if (esp_timer_create(&timerArgs, &_timerHandle) == ESP_OK) {
      esp_timer_start_periodic(_timerHandle, static_cast<int64_t>(ms * 1000LL));
    }
#endif
  }

  return oldMs;
}

#if defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52)
// Cooperative auto-send pump (see header note): called from every public
// setter and ready(). No-op unless a poll interval is configured.
void DS4Gamepad::_pumpAutoSend() {
  if (_pollIntervalMs == 0 || !_connected) return;
  unsigned long now = millis();
  if ((unsigned long)(now - _lastAutoSendMs) < _pollIntervalMs) return;
  _lastAutoSendMs = now;
  sendGamepadReport();
}

#else /* !SAMD/nRF52: free-running timer callbacks */

// Auto-send runs from the OS-timer callback below; setters need not pump.
void DS4Gamepad::_pumpAutoSend() {}

#if defined(ARDUINO_ARCH_RP2040)
bool DS4Gamepad::_timerCallback(struct repeating_timer *t) {
  if (t == nullptr || t->user_data == nullptr) return true;
  auto self = reinterpret_cast<DS4Gamepad*>(t->user_data);

  if (!tud_mounted()) return true;
  // Arm mount tracking so auto-send works even if the app never polls isConnected().
  self->_refreshMount();
  // Guard: the 100ms post-mount settle window still applies.
  unsigned long elapsed = (unsigned long)(millis() - self->_mountedAt);
  if (!self->_usbReady || elapsed < 100UL) return true;

  self->sendGamepadReport();
  return true;
}

#else
void IRAM_ATTR DS4Gamepad::_timerCallback(void* arg) {
  if (arg == nullptr) return;
  auto self = reinterpret_cast<DS4Gamepad*>(arg);

  if (!tud_mounted()) return;
  // Arm mount tracking so auto-send works even if the app never polls isConnected().
  self->_refreshMount();
  // Guard: the 100ms post-mount settle window still applies.
  unsigned long elapsed = (unsigned long)(millis() - self->_mountedAt);
  if (!self->_usbReady || elapsed < 100UL) return;

  self->sendGamepadReport();
}
#endif /* ARDUINO_ARCH_RP2040 */

#endif /* !SAMD/nRF52 */

#endif /* platform guard */
