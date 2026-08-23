// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// JoystickTest — Analog range sweep for DS4Gamepad.
// Sweeps sticks and triggers through their full range to verify HID report
// encoding. Watch the gamepad properties in your OS input settings.

#include <DS4Gamepad.h>

DS4Gamepad gamepad;

void setup() {
  DS4_DEBUG_SERIAL.begin(115200);
  gamepad.begin();
}

void loop() {
  static uint8_t phase = 0;

  switch (phase) {
    case 0: // Left stick sweep right
      for (int x = -127; x <= 127; x += 5) {
        gamepad.setStickLeft(x, 0);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 1: // Left stick sweep up
      for (int y = -127; y <= 127; y += 5) {
        gamepad.setStickLeft(0, y);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 2: // Right stick sweep right
      for (int x = -127; x <= 127; x += 5) {
        gamepad.setStickRight(x, 0);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 3: // Right stick sweep up
      for (int y = -127; y <= 127; y += 5) {
        gamepad.setStickRight(0, y);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 4: // Left trigger sweep
      for (uint16_t t = 0; t <= 32768; t += 1000) {
        gamepad.setLeftTrigger(t);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 5: // Right trigger sweep
      for (uint16_t t = 0; t <= 32768; t += 1000) {
        gamepad.setRightTrigger(t);
        gamepad.send();
        delay(5);
      }
      phase++;
      break;

    case 6: // Reset to center/zero
      gamepad.releaseAll();
      gamepad.send();
      delay(500);
      phase = 0;
      break;
  }
}
