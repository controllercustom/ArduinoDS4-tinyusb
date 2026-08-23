// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// SimpleButtons — minimal DS4 button press/release demo.
// Presses and releases the Cross (X) button every 500ms.

#include <DS4Gamepad.h>

DS4Gamepad gamepad;

void setup() {
  DS4_DEBUG_SERIAL.begin(115200);
  gamepad.begin();
}

void loop() {
  static bool pressed = false;

  if (!pressed) {
    gamepad.press(DS4_BTN_CROSS);
    gamepad.send();
    DS4_DEBUG_SERIAL.println("Pressing Cross");
    pressed = true;
  } else {
    gamepad.release(DS4_BTN_CROSS);
    gamepad.send();
    DS4_DEBUG_SERIAL.println("Releasing Cross");
    pressed = false;
  }

  delay(500);
}
