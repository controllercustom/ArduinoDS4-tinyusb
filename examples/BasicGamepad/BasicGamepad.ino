// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// BasicGamepad — UART command parser for DS4Gamepad.
// Accepts KEY=VALUE\n commands over DS4_DEBUG_SERIAL and sends DS4 reports.
//
// Commands:
//   BTN_SQUARE=1, BTN_CROSS=0, BTN_L1=1, BTN_PS=0, etc.  (0 or 1)
//   LX=64, LY=64, RX=64, RY=64                           (signed -127..127, 0=center)
//   HAT=0                                                (0..7 direction, 8=centered)
//   TRIG_L=200, TRIG_R=200                               (unsigned 0..255)
//   TP1=960,471                                             (x,y for finger 0)
//   TP1=                                                    (lift finger 0)
//   RELEASE                                                  (release all + send)

#include <DS4Gamepad.h>
#include <cstring>

DS4Gamepad gamepad;

static int16_t lx = 0, ly = 0, rx = 0, ry = 0;

// DS4Gamepad sticks take the full int16 range (-32768..32767, 0 = center);
// the UART protocol uses human-scale -127..127, so scale it here.
static int16_t stickVal(int16_t v) {
  return (int16_t)((int32_t)v * 32767 / 127);
}

// Triggers take 0..32768; the UART protocol uses 0..255.
static uint16_t trigVal(int v) {
  return (uint16_t)((int32_t)v * 32768 / 255);
}

static int clampAxis(int v) {
  return (v < -127) ? -127 : ((v > 127) ? 127 : v);
}

static int clampTrig(int v) {
  return (v < 0) ? 0 : ((v > 255) ? 255 : v);
}

static void exec(const char *key, const char *val) {
  if (strcmp(key, "BTN_SQUARE") == 0)   { gamepad.setButton(DS4_BTN_SQUARE, atoi(val) != 0); }
  else if (strcmp(key, "BTN_CROSS") == 0)    { gamepad.setButton(DS4_BTN_CROSS, atoi(val) != 0); }
  else if (strcmp(key, "BTN_CIRCLE") == 0)   { gamepad.setButton(DS4_BTN_CIRCLE, atoi(val) != 0); }
  else if (strcmp(key, "BTN_TRIANGLE") == 0) { gamepad.setButton(DS4_BTN_TRIANGLE, atoi(val) != 0); }
  else if (strcmp(key, "BTN_L1") == 0)       { gamepad.setButton(DS4_BTN_L1, atoi(val) != 0); }
  else if (strcmp(key, "BTN_R1") == 0)       { gamepad.setButton(DS4_BTN_R1, atoi(val) != 0); }
  else if (strcmp(key, "BTN_L2") == 0)       { gamepad.setButton(DS4_BTN_L2, atoi(val) != 0); }
  else if (strcmp(key, "BTN_R2") == 0)       { gamepad.setButton(DS4_BTN_R2, atoi(val) != 0); }
  else if (strcmp(key, "BTN_SHARE") == 0)    { gamepad.setButton(DS4_BTN_SHARE, atoi(val) != 0); }
  else if (strcmp(key, "BTN_OPTIONS") == 0)  { gamepad.setButton(DS4_BTN_OPTIONS, atoi(val) != 0); }
  else if (strcmp(key, "BTN_L3") == 0)       { gamepad.setButton(DS4_BTN_L3, atoi(val) != 0); }
  else if (strcmp(key, "BTN_R3") == 0)       { gamepad.setButton(DS4_BTN_R3, atoi(val) != 0); }
  else if (strcmp(key, "BTN_PS") == 0)       { gamepad.setButton(DS4_BTN_PS, atoi(val) != 0); }
  else if (strcmp(key, "BTN_TOUCHPAD") == 0) { gamepad.setButton(DS4_BTN_TOUCHPAD, atoi(val) != 0); }
  else if (strcmp(key, "LX") == 0)  { lx = clampAxis(atoi(val)); gamepad.setStickLeft(stickVal(lx), stickVal(ly)); }
  else if (strcmp(key, "LY") == 0)  { ly = clampAxis(atoi(val)); gamepad.setStickLeft(stickVal(lx), stickVal(ly)); }
  else if (strcmp(key, "RX") == 0)  { rx = clampAxis(atoi(val)); gamepad.setStickRight(stickVal(rx), stickVal(ry)); }
  else if (strcmp(key, "RY") == 0)  { ry = clampAxis(atoi(val)); gamepad.setStickRight(stickVal(rx), stickVal(ry)); }
  else if (strcmp(key, "HAT") == 0)     { gamepad.setHat((uint8_t)atoi(val)); }
  else if (strcmp(key, "TRIG_L") == 0)  { gamepad.setLeftTrigger(trigVal(clampTrig(atoi(val)))); }
  else if (strcmp(key, "TRIG_R") == 0)  { gamepad.setRightTrigger(trigVal(clampTrig(atoi(val)))); }
  else if (strcmp(key, "TP1") == 0) {
    if (strlen(val) == 0) {
      gamepad.setTouch(0, false, 0, 0);
    } else {
      int x = 0, y = 0;
      sscanf(val, "%d,%d", &x, &y);
      gamepad.setTouch(0, true, (uint16_t)x, (uint16_t)y);
    }
  }
  else if (strcmp(key, "TP2") == 0) {
    if (strlen(val) == 0) {
      gamepad.setTouch(1, false, 0, 0);
    } else {
      int x = 0, y = 0;
      sscanf(val, "%d,%d", &x, &y);
      gamepad.setTouch(1, true, (uint16_t)x, (uint16_t)y);
    }
  }
  else if (strcmp(key, "RELEASE") == 0) {
    gamepad.releaseAll();
    bool ok = gamepad.send();
    DS4_DEBUG_SERIAL.print("SENT=");
    DS4_DEBUG_SERIAL.println(ok ? "1" : "0");
    return;
  }
  else {
    DS4_DEBUG_SERIAL.print("UNKNOWN_CMD=");
    DS4_DEBUG_SERIAL.println(key);
    return;
  }
  bool ok = gamepad.send();
  DS4_DEBUG_SERIAL.print("SENT=");
  DS4_DEBUG_SERIAL.println(ok ? "1" : "0");
}

static char buf[32];
static size_t pos = 0;

void setup() {
  DS4_DEBUG_SERIAL.begin(115200);
  delay(200);
  gamepad.begin();
  DS4_DEBUG_SERIAL.println("READY");
}

void loop() {
  while (DS4_DEBUG_SERIAL.available() > 0) {
    char c = DS4_DEBUG_SERIAL.read();
    if (c == '\n' || c == '\r') {
      if (pos > 0) {
        buf[pos] = '\0';
        char *eq = (char *)memchr(buf, '=', pos);
        if (eq) {
          *eq = '\0';
          exec(buf, eq + 1);
        }
        pos = 0;
      }
    } else if (pos < sizeof(buf) - 1) {
      buf[pos++] = c;
    }
  }
}
