// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// FullController — GPIO + analog stick gamepad for DS4Gamepad.
// Reads 14 digital buttons, 4 d-pad directions, and 4 analog sticks
// from GPIO pins and sends DS4 HID reports.

#include <DS4Gamepad.h>

#define PIN_BTN_SQUARE  0
#define PIN_BTN_CROSS   1
#define PIN_BTN_CIRCLE  2
#define PIN_BTN_TRIANGLE 3
#define PIN_BTN_L1      4
#define PIN_BTN_R1      5
#define PIN_BTN_L2      6
#define PIN_BTN_R2      7
#define PIN_BTN_SHARE   8
#define PIN_BTN_OPTIONS 9
#define PIN_BTN_L3      10
#define PIN_BTN_R3      11
#define PIN_BTN_PS      12
#define PIN_BTN_TP      13
#define PIN_DPAD_UP     14
#define PIN_DPAD_DN     15
#define PIN_DPAD_LT     16
#define PIN_DPAD_RT     17
#define PIN_LX          18
#define PIN_LY          19
#define PIN_RX          20
#define PIN_RY          21

DS4Gamepad gamepad;

static const struct { uint8_t pin; uint8_t btn; } btn_pins[] = {
  {PIN_BTN_SQUARE,  DS4_BTN_SQUARE},
  {PIN_BTN_CROSS,   DS4_BTN_CROSS},
  {PIN_BTN_CIRCLE,  DS4_BTN_CIRCLE},
  {PIN_BTN_TRIANGLE,DS4_BTN_TRIANGLE},
  {PIN_BTN_L1,      DS4_BTN_L1},
  {PIN_BTN_R1,      DS4_BTN_R1},
  {PIN_BTN_L2,      DS4_BTN_L2},
  {PIN_BTN_R2,      DS4_BTN_R2},
  {PIN_BTN_SHARE,   DS4_BTN_SHARE},
  {PIN_BTN_OPTIONS, DS4_BTN_OPTIONS},
  {PIN_BTN_L3,      DS4_BTN_L3},
  {PIN_BTN_R3,      DS4_BTN_R3},
  {PIN_BTN_PS,      DS4_BTN_PS},
  {PIN_BTN_TP,      DS4_BTN_TOUCHPAD},
};

void setup() {
  for (size_t i = 0; i < sizeof(btn_pins) / sizeof(btn_pins[0]); i++) {
    pinMode(btn_pins[i].pin, INPUT_PULLUP);
  }
  pinMode(PIN_DPAD_UP, INPUT_PULLUP);
  pinMode(PIN_DPAD_DN, INPUT_PULLUP);
  pinMode(PIN_DPAD_LT, INPUT_PULLUP);
  pinMode(PIN_DPAD_RT, INPUT_PULLUP);

  DS4_DEBUG_SERIAL.begin(115200);
  gamepad.begin();
  delay(500);
}

void loop() {
  for (size_t i = 0; i < sizeof(btn_pins) / sizeof(btn_pins[0]); i++) {
    gamepad.setButton(btn_pins[i].btn, !digitalRead(btn_pins[i].pin));
  }

  uint8_t dpad = DS4_HAT_CENTERED;
  bool up   = !digitalRead(PIN_DPAD_UP);
  bool down = !digitalRead(PIN_DPAD_DN);
  bool left = !digitalRead(PIN_DPAD_LT);
  bool right= !digitalRead(PIN_DPAD_RT);

  if (up && !down && !left && !right) dpad = DS4_HAT_UP;
  else if (up && !down && right && !left) dpad = DS4_HAT_UP_RIGHT;
  else if (!up && !down && right && !left) dpad = DS4_HAT_RIGHT;
  else if (!up && down && right && !left) dpad = DS4_HAT_DOWN_RIGHT;
  else if (!up && down && !left && !right) dpad = DS4_HAT_DOWN;
  else if (!up && down && left && !right) dpad = DS4_HAT_DOWN_LEFT;
  else if (!up && !down && left && !right) dpad = DS4_HAT_LEFT;
  else if (up && !down && left && !right) dpad = DS4_HAT_UP_LEFT;
  gamepad.setHat(dpad);

  int16_t lx = (analogRead(PIN_LX) * 255 / 4095) - 127;
  int16_t ly = (analogRead(PIN_LY) * 255 / 4095) - 127;
  int16_t rx = (analogRead(PIN_RX) * 255 / 4095) - 127;
  int16_t ry = (analogRead(PIN_RY) * 255 / 4095) - 127;
  gamepad.setStickLeft(lx, ly);
  gamepad.setStickRight(rx, ry);

  gamepad.send();
  delay(8);
}
