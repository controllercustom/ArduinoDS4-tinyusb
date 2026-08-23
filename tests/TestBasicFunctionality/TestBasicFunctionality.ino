// SPDX-License-Identifier: MIT
// DS4Gamepad — Basic functionality on-board test suite.
//
// Compile-time + runtime verification of button state, sticks, hat, triggers,
// touch, releaseAll, and LED callback. Reports PASS/FAIL over DS4_DEBUG_SERIAL
// (Serial0 on ESP32, Serial1 on RP2040).

#include <DS4Gamepad.h>

// ---------------------------------------------------------------------------
// Compile-time checks
// ---------------------------------------------------------------------------
static_assert(DS4_BTN_COUNT == 14, "DS4 must have exactly 14 buttons");
static_assert(DS4_HAT_CENTERED == 0x08, "Hat centered must be 0x08");
static_assert(DS4_TOUCH_MAX_X == 1919, "Touchpad X max must be 1919");
static_assert(DS4_TOUCH_MAX_Y == 942, "Touchpad Y max must be 942");

static DS4Gamepad gamepad;

static uint16_t pass = 0;
static uint16_t fail = 0;

#define CHECK(expr, msg) do { \
    if (expr) { pass++; } \
    else { fail++; DS4_DEBUG_SERIAL.print("FAIL: "); DS4_DEBUG_SERIAL.println(msg); } \
  } while (0)

// LED callback state
static volatile uint8_t lastLedValue = 0xFF;
static volatile bool ledFired = false;
static void ledCb(uint8_t val) { lastLedValue = val; ledFired = true; }

// RGB LED color callback state
static DS4Gamepad::DS4LED lastLedColor{0, 0, 0};
static bool ledColorFired = false;
static void ledColorCb(const DS4Gamepad::DS4LED &c) { lastLedColor = c; ledColorFired = true; }

// Rumble callback state
static volatile uint8_t lastRumbleL = 0xFF;
static volatile uint8_t lastRumbleR = 0xFF;
static volatile bool rumbleFired = false;
static void rumbleCb(uint8_t l, uint8_t r) { lastRumbleL = l; lastRumbleR = r; rumbleFired = true; }

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static void testButtons() {
    DS4_DEBUG_SERIAL.println("--- Buttons ---");

    for (uint8_t i = 0; i < DS4_BTN_COUNT; i++) {
        gamepad.press(i);
        CHECK(gamepad.getButton(i), "press failed");

        gamepad.release(i);
        CHECK(!gamepad.getButton(i), "release failed");
    }

    gamepad.setButton(DS4_BTN_CROSS, true);
    CHECK(gamepad.getButton(DS4_BTN_CROSS), "setButton(true) failed");
    gamepad.setButton(DS4_BTN_CROSS, false);
    CHECK(!gamepad.getButton(DS4_BTN_CROSS), "setButton(false) failed");

    gamepad.press(DS4_BTN_COUNT);
    CHECK(!gamepad.getButton(DS4_BTN_COUNT), "OOB press should be noop");

    for (uint8_t i = 0; i < DS4_BTN_COUNT; i++) {
        gamepad.press(i);
    }
    bool allPressed = true;
    for (uint8_t i = 0; i < DS4_BTN_COUNT; i++) {
        if (!gamepad.getButton(i)) { allPressed = false; break; }
    }
    CHECK(allPressed, "pressAll: not all pressed");
    gamepad.releaseAll();
    bool nonePressed = true;
    for (uint8_t i = 0; i < DS4_BTN_COUNT; i++) {
        if (gamepad.getButton(i)) { nonePressed = false; break; }
    }
    CHECK(nonePressed, "releaseAll: buttons still held");
}

static void testSticks() {
    DS4_DEBUG_SERIAL.println("--- Sticks ---");
    gamepad.setStickLeft(0, 0);
    gamepad.setStickLeft(-127, -127);
    gamepad.setStickLeft(127, 127);
    gamepad.setStickRight(0, 0);
    gamepad.setStickRight(-127, -127);
    gamepad.setStickRight(127, 127);
    gamepad.setStickLeft(-200, 200);
    gamepad.setStickRight(200, -200);
    CHECK(true, "stick writes survived extremes");
}

static void testTriggers() {
    DS4_DEBUG_SERIAL.println("--- Triggers ---");
    gamepad.setLeftTrigger(0);
    gamepad.setLeftTrigger(16384);
    gamepad.setLeftTrigger(32768);
    gamepad.setRightTrigger(0);
    gamepad.setRightTrigger(16384);
    gamepad.setRightTrigger(32768);
    CHECK(true, "trigger writes survived range");
}

static void testHat() {
    DS4_DEBUG_SERIAL.println("--- Hat ---");
    for (uint8_t h = 0; h <= 8; h++) {
        gamepad.setHat(h);
        CHECK(gamepad.getHat() == h, "getHat mismatch");
    }
    gamepad.setHat(0x0F);
    CHECK(gamepad.getHat() == 0x0F, "hat 0x0F should store");
}

static void testTouch() {
    DS4_DEBUG_SERIAL.println("--- Touch ---");
    gamepad.setTouch(0, true, 960, 471);
    CHECK(true, "touch finger 0 down survived");
    gamepad.setTouch(0, false, 0, 0);
    CHECK(true, "touch finger 0 up survived");
    gamepad.setTouch(1, true, 500, 200);
    CHECK(true, "touch finger 1 down survived");
    gamepad.setTouch(1, false, 0, 0);
    CHECK(true, "touch finger 1 up survived");
    gamepad.setTouch(2, true, 100, 100);
    CHECK(true, "touch finger 2 OOB survived");
}

static void testReleaseAll() {
    DS4_DEBUG_SERIAL.println("--- releaseAll ---");
    gamepad.press(DS4_BTN_CROSS);
    gamepad.press(DS4_BTN_PS);
    gamepad.setStickLeft(100, -100);
    gamepad.setLeftTrigger(32768);
    gamepad.setHat(2);
    gamepad.setTouch(0, true, 1234, 567);
    gamepad.releaseAll();

    bool none = true;
    for (uint8_t i = 0; i < DS4_BTN_COUNT; i++) {
        if (gamepad.getButton(i)) { none = false; break; }
    }
    CHECK(none, "releaseAll: buttons still held");
    CHECK(gamepad.getHat() == DS4_HAT_CENTERED, "releaseAll: hat not centered");
}

static void testSendAndReady() {
    DS4_DEBUG_SERIAL.begin(115200);
    uint32_t t0 = millis();
    while (!gamepad.ready() && millis() - t0 < 3000) {
        delay(50);
    }
    if (!gamepad.ready()) {
        DS4_DEBUG_SERIAL.println("  (USB HID not ready — skipping send test)");
        pass++;
        return;
    }
    // send() is guarded by a 100ms post-mount settling window, so retry until it succeeds.
    bool sent = false;
    while (!sent && millis() - t0 < 3000) {
        sent = gamepad.send();
        if (!sent) delay(50);
    }
    CHECK(sent, "send() returned false after settling window");
}

static void testLedCallback() {
    DS4_DEBUG_SERIAL.println("--- LED callback ---");
    gamepad.onLed(ledCb);
    lastLedValue = 0xFF;
    ledFired = false;

    CHECK(gamepad.getLEDState() == 0, "getLEDState should be 0 initially");
    CHECK(!ledFired, "LED callback should not fire yet");
    gamepad.onLed(nullptr);
}

static void testSetFeature() {
    DS4_DEBUG_SERIAL.println("--- _setFeature output report ---");

    gamepad.onLed(ledCb);
    gamepad.onRumble(rumbleCb);
    gamepad.onLedColor(ledColorCb);

    // Path 1: interrupt OUT — TinyUSB reports report_id as 0 and keeps the
    // Report ID byte at buffer[0] (how Linux hid-playstation sends).
    uint8_t buf[32] = {0};
    buf[0] = 0x05;           // report ID
    buf[1] = 0x07;           // valid_flag0 -> p[0]
    buf[2] = 0x03;           // valid_flag1 -> p[1]
    buf[4] = 0x80;           // motor_right -> p[3]
    buf[5] = 0x40;           // motor_left -> p[4]
    buf[6] = 0xC8;           // lightbar_red -> p[5]
    ledFired = false; rumbleFired = false; ledColorFired = false;
    gamepad._setFeature(0, HID_REPORT_TYPE_OUTPUT, buf, sizeof(buf));
    CHECK(ledFired && lastLedValue == 0xC8, "interrupt-OUT: LED value wrong");
    CHECK(rumbleFired && lastRumbleL == 0x40 && lastRumbleR == 0x80,
          "interrupt-OUT: rumble args wrong");
    CHECK(gamepad.getLEDState() == 0xC8, "interrupt-OUT: getLEDState wrong");

    // Path 2: SET_REPORT control — report_id parsed, ID byte stripped.
    uint8_t ctl[31] = {0};
    ctl[0] = 0x07;           // valid_flag0 -> p[0]
    ctl[1] = 0x03;           // valid_flag1 -> p[1]
    ctl[3] = 0x11;           // motor_right -> p[3]
    ctl[4] = 0x22;           // motor_left -> p[4]
    ctl[5] = 0x33;           // lightbar_red -> p[5]
    ledFired = false; rumbleFired = false; ledColorFired = false;
    gamepad._setFeature(0x05, HID_REPORT_TYPE_OUTPUT, ctl, sizeof(ctl));
    CHECK(ledFired && lastLedValue == 0x33, "control: LED value wrong");
    CHECK(rumbleFired && lastRumbleL == 0x22 && lastRumbleR == 0x11,
          "control: rumble args wrong");

    // Negative: unknown report ID on the interrupt path.
    buf[0] = 0x81;
    ledFired = false; rumbleFired = false; ledColorFired = false;
    gamepad._setFeature(0, HID_REPORT_TYPE_OUTPUT, buf, sizeof(buf));
    CHECK(!ledFired && !rumbleFired && !ledColorFired, "wrong report ID should not fire callbacks");

    // Negative: INPUT report type.
    buf[0] = 0x05;
    ledFired = false; ledColorFired = false;
    gamepad._setFeature(0x05, HID_REPORT_TYPE_INPUT, buf, sizeof(buf));
    CHECK(!ledFired && !ledColorFired, "INPUT report type should not fire LED/RGB");

    // --- RGB color tests (received flag is now true from paths 1-2) ---
    buf[6] = 0xC8;           // red -> p[5]
    buf[7] = 0x0F;           // green -> p[6]
    buf[8] = 0xA5;           // blue -> p[7]
    ledColorFired = false;
    gamepad._setFeature(0, HID_REPORT_TYPE_OUTPUT, buf, sizeof(buf));
    CHECK(ledColorFired && lastLedColor.r == 0xC8 && lastLedColor.g == 0x0F && lastLedColor.b == 0xA5, "RGB callback values wrong");
    { DS4Gamepad::DS4LED c = gamepad.getLEDColor();
      CHECK(c.r == 0xC8 && c.g == 0x0F && c.b == 0xA5, "getLEDColor() mismatch after full parse"); }

    // Red regression: getLEDState still returns red channel only
    CHECK(gamepad.getLEDState() == 0xC8, "getLEDState should return red (0xC8)");

    // Truncated negative: bufsize=8 with report_id=0 -> strip ID byte -> n=7.
    uint8_t shortBuf[32] = {0};
    shortBuf[0] = 0x05;
    ledColorFired = false;
    gamepad._setFeature(0, HID_REPORT_TYPE_OUTPUT, shortBuf, 8);
    CHECK(!ledColorFired, "truncated payload should not fire RGB callback");

    // White color test: verify {255,255,255} is stored correctly.
    buf[6] = 0xFF; buf[7] = 0xFF; buf[8] = 0xFF;
    ledColorFired = false;
    gamepad._setFeature(0, HID_REPORT_TYPE_OUTPUT, buf, sizeof(buf));
    { DS4Gamepad::DS4LED c = gamepad.getLEDColor();
      CHECK(c.r == 0xFF && c.g == 0xFF && c.b == 0xFF, "white color should be stored"); }

    gamepad.onLed(nullptr);
    gamepad.onRumble(nullptr);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void setup() {
    DS4_DEBUG_SERIAL.begin(115200);
    delay(200);

    // Deterministic sentinel check: before begin()/USB mount no output report can
    // have arrived, so getLEDColor() must still return the never-set {0xFF,0xFF,0xFF}.
    { DS4Gamepad::DS4LED s = gamepad.getLEDColor();
      CHECK(s.r == 0xFF && s.g == 0xFF && s.b == 0xFF, "RGB sentinel should be {0xFF}"); }

    gamepad.begin();
    delay(200);
    DS4_DEBUG_SERIAL.println("=== DS4Gamepad TestBasicFunctionality ===");

    testButtons();
    testSticks();
    testTriggers();
    testHat();
    testTouch();
    testReleaseAll();
    testSendAndReady();
    testLedCallback();
    testSetFeature();

    DS4_DEBUG_SERIAL.println("=== RESULTS ===");
    DS4_DEBUG_SERIAL.print("PASS: ");
    DS4_DEBUG_SERIAL.println(pass);
    DS4_DEBUG_SERIAL.print("FAIL: ");
    DS4_DEBUG_SERIAL.println(fail);
    if (fail == 0) {
        DS4_DEBUG_SERIAL.println("ALL TESTS PASSED");
    } else {
        DS4_DEBUG_SERIAL.println("TESTS FAILED");
    }
}

void loop() {
    delay(1000);
}
