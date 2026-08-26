// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

#pragma once

// Unified DS4 (DualShock 4-style) gamepad emulation for:
//   - ESP32-S3   (esp32:esp32 core, Adafruit TinyUSB)
//   - RP2040     (arduino-pico core, USE_TINYUSB)
//   - SAMD21/51  (adafruit:samd core, USB Stack = TinyUSB)
//   - nRF52840   (adafruit:nrf52 core — always builds TinyUSB)
//   - Renesas RA4M1 (arduino:renesas_uno core — Nano R4 / UNO R4 Minima;
//     requires scripts/patch_renesas_core.sh + DISABLE_USB_SERIAL)
#if (defined(ARDUINO_ARCH_RP2040) && defined(USE_TINYUSB)) || \
    (defined(ARDUINO_ARCH_SAMD) && defined(USE_TINYUSB)) || \
    (defined(ARDUINO_ARCH_NRF52) && defined(USE_TINYUSB)) || \
    defined(ARDUINO_ARCH_ESP32) || \
    defined(ARDUINO_ARCH_RENESAS)

#include <Arduino.h>
#if defined(ARDUINO_ARCH_RENESAS)
// The stock arduino:renesas_uno core has no Adafruit TinyUSB wrapper; the
// patched core's weak hooks (__USBGetHIDReport/__USBGetVidPid) plus TinyUSB's
// own HID class driver provide the interface instead.
#include "tusb.h"
#else
#include <Adafruit_TinyUSB.h>
#endif
#include <functional>

#if defined(ARDUINO_ARCH_RP2040)
#include <pico/time.h>
#endif

// Debug telemetry UART. Most users never connect this — the device enumerates
// as a HID-only gamepad. ESP32: native UART0 (Serial0). RP2040, SAMD
// (TinyUSB build) and nRF52: UART1 (Serial1) — on those cores, Serial is the
// USB CDC that begin() drops.
#if defined(ARDUINO_ARCH_RP2040)
#define DS4_DEBUG_SERIAL Serial1
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_ARCH_RENESAS)
#define DS4_DEBUG_SERIAL Serial1
#else
#define DS4_DEBUG_SERIAL Serial0
#endif

// Formatted telemetry helper. The ESP32 and arduino-pico cores provide
// Print::printf(); the Arduino SAMD, adafruit:nrf52 and Renesas cores do not,
// so route through a vsnprintf shim there. Usage: DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "x=%u\n", x);
#if defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_ARCH_RENESAS)
#include <stdarg.h>
#include <stdio.h>
inline void ds4_dbg_printf(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  DS4_DEBUG_SERIAL.print(buf);
}
#define DS4_DBG_PRINTF(serial, fmt, ...) ds4_dbg_printf(fmt, ##__VA_ARGS__)
#else
#define DS4_DBG_PRINTF(serial, fmt, ...) (serial).printf(fmt, ##__VA_ARGS__)
#endif

// Microsecond timestamp helper (used by examples/tests for telemetry only).
uint64_t ds4_micros();

// ---------------------------------------------------------------------------
// Button indices — DS4 HID order (HID Button 1..14):
//   1 Square, 2 Cross, 3 Circle, 4 Triangle, 5 L1, 6 R1, 7 L2, 8 R2,
//   9 Share, 10 Options, 11 L3, 12 R3, 13 PS, 14 Touchpad-click
// ---------------------------------------------------------------------------
enum DS4Button : uint8_t {
  DS4_BTN_SQUARE   = 0,
  DS4_BTN_CROSS    = 1,
  DS4_BTN_CIRCLE   = 2,
  DS4_BTN_TRIANGLE = 3,
  DS4_BTN_L1       = 4,
  DS4_BTN_R1       = 5,
  DS4_BTN_L2       = 6,
  DS4_BTN_R2       = 7,
  DS4_BTN_SHARE    = 8,
  DS4_BTN_OPTIONS  = 9,
  DS4_BTN_L3       = 10,
  DS4_BTN_R3       = 11,
  DS4_BTN_PS       = 12,
  DS4_BTN_TOUCHPAD = 13,
  DS4_BTN_COUNT    = 14
};

// Hat switch values (DS4 convention: 8 = released/centered/null).
enum DS4Hat : uint8_t {
  DS4_HAT_UP         = 0x00,
  DS4_HAT_UP_RIGHT   = 0x01,
  DS4_HAT_RIGHT      = 0x02,
  DS4_HAT_DOWN_RIGHT = 0x03,
  DS4_HAT_DOWN       = 0x04,
  DS4_HAT_DOWN_LEFT  = 0x05,
  DS4_HAT_LEFT       = 0x06,
  DS4_HAT_UP_LEFT    = 0x07,
  DS4_HAT_CENTERED   = 0x08
};

// DS4 touchpad resolution.
#define DS4_TOUCH_MAX_X 1919
#define DS4_TOUCH_MAX_Y 942

// Touch finger state (2 slots).
struct DS4Touch {
  bool     down;
  uint8_t  id;
  uint16_t x, y;
};

class DS4Gamepad {
public:
  DS4Gamepad();

  void begin();

  void press(uint8_t btn);
  void release(uint8_t btn);
  void setButton(uint8_t btn, bool pressed);
  bool getButton(uint8_t btn) const;

  // Sticks: signed -32768..32767 (auto-mapped to uint8_t 0..255 in report).
  void setStickLeft(int16_t x, int16_t y);
  void setStickRight(int16_t x, int16_t y);

  // Hat/dpad: use DS4_HAT_* constants (8 = centered).
  void setHat(uint8_t hat);
  void setDpad(uint8_t dir);
  uint8_t getHat() const;

  // Analog triggers: unsigned 0..32768 (mapped to 0..255 internally).
  void setLeftTrigger(uint16_t value);
  void setRightTrigger(uint16_t value);

  // Touchpad: finger 0 or 1. x = 0..1919, y = 0..942. down=false lifts finger.
  void setTouch(uint8_t finger, bool down, uint16_t x, uint16_t y);

  // Send the current report to the host.
  bool send();

  // True when USB HID is ready to accept reports.
  bool ready();
  bool isConnected() const;

  // Zero everything (buttons, sticks, triggers, d-pad, touch). Call send() explicitly if a neutral report is needed.
  void releaseAll();

  // Host LED/rumble callbacks.
  using LedCallback = std::function<void(uint8_t ledValue)>;
  using RumbleCallback = std::function<void(uint8_t leftMotor, uint8_t rightMotor)>;
  void onLed(LedCallback cb);
  void onRumble(RumbleCallback cb);
  uint8_t getLEDState() const;

  // Full RGB LED support. Returns {0xFF,0xFF,0xFF} sentinel until first output report received.
  struct DS4LED { uint8_t r, g, b; };
  using LedColorCallback = std::function<void(const DS4LED&)>;
  void onLedColor(LedColorCallback cb);
  DS4LED getLEDColor() const;

  // Configurable auto-send poll interval (ms). Default 0 = manual send() only.
  // NOTE (SAMD): no OS timer here — auto-send is cooperative, flushed from the
  // public setters and ready(). See the private-member note below.
  uint32_t setPollInterval(uint32_t ms);

  // Adafruit TinyUSB HID callbacks (static forwarders -> instance).
  static uint16_t _getReportCb(uint8_t report_id, hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen);
  static void _setReportCb(uint8_t report_id, hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize);

  uint16_t _getFeature(uint8_t report_id, hid_report_type_t report_type,
                        uint8_t *buffer, uint16_t reqlen);
  void _setFeature(uint8_t report_id, hid_report_type_t report_type,
                  uint8_t const *buffer, uint16_t bufsize);

private:
#if !defined(ARDUINO_ARCH_RENESAS)
  Adafruit_USBD_HID hid;
#endif

  uint16_t buttonMask;
  uint8_t  axes[6];      // unsigned; sticks 0..255 (128 center), triggers 0..255
  uint8_t  hat;

  DS4Touch touch[2];
  uint8_t  touchId;
  uint8_t  touchCounter;
  uint16_t timestamp;
  uint8_t  reportCounter;

  uint8_t  ledStateValue;
  LedCallback ledCallback;
  RumbleCallback rumbleCallback;

  DS4LED ledColorValue{0xFF, 0xFF, 0xFF};
  bool   ledColorReceived = false;
  LedColorCallback ledColorCallback;

  bool _connected = false;
  mutable volatile bool _usbReady = false;
  mutable unsigned long _mountedAt = 0;

  uint32_t _pollIntervalMs = 0;
#if defined(ARDUINO_ARCH_RP2040)
  repeating_timer_t _timer;
  bool _timerStarted = false;
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_NRF52) || defined(ARDUINO_ARCH_RENESAS)
  // No free-running OS timer used on SAMD/nRF52/Renesas: auto-send is
  // cooperative. Due sends are flushed from the public setter entry points
  // and ready(); a sketch that calls any library API per loop iteration gets
  // loop-rate polling, one that never calls in gets no auto-sends.
  unsigned long _lastAutoSendMs = 0;
#else
  esp_timer_handle_t _timerHandle = nullptr;
#endif

  bool sendGamepadReport();
  void _refreshMount() const;
  void _pumpAutoSend();
#if defined(ARDUINO_ARCH_RP2040)
  static bool _timerCallback(struct repeating_timer *t);
#elif !defined(ARDUINO_ARCH_SAMD) && !defined(ARDUINO_ARCH_NRF52) && !defined(ARDUINO_ARCH_RENESAS)
  static void IRAM_ATTR _timerCallback(void* arg);
#endif
};

#endif /* platform guard */
