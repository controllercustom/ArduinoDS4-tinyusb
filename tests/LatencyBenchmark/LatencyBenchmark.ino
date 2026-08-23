// SPDX-License-Identifier: MIT
// DS4Gamepad — Latency benchmark emitter.
//
// Waits for "START" over DS4_DEBUG_SERIAL, then emits timestamped HID report
// events (buttons, sticks, triggers, touch) at high speed. Attach a host-side
// python script (test_e2e.py) that reads these timestamps and correlates
// them with Linux evdev events to measure end-to-end USB HID latency.
//
// DS4_DEBUG_SERIAL protocol:
//   TX lines:  TS:<us>:<seq>:<type>:<value>
//   TX lines:  BTN:<name>
//   TX lines:  RPT:<payload_bytes>:<btn_count>
//   TX lines:  READY / DONE
//   RX line:   START               (begins measurement run)
//   RX line:   SYNC                (responds with SYNC:<us> for clock align)

#include <DS4Gamepad.h>

#define ITERATIONS 50

static DS4Gamepad gamepad;
static volatile uint64_t g_seq = 0;
static volatile bool g_running = false;

enum : uint8_t {
    TYPE_BTN_PRESS   = 0,
    TYPE_BTN_RELEASE = 1,
    TYPE_STICK       = 2,
    TYPE_TRIGGER     = 3,
    TYPE_TOUCH       = 4,
    TYPE_HAT         = 5,
    TYPE_BEGIN       = 99,
};

static const char* buttonNames[] = {
    "SQUARE", "CROSS", "CIRCLE", "TRIANGLE",
    "L1", "R1", "L2", "R2",
    "SHARE", "OPTIONS", "L3", "R3",
    "PS", "TOUCHPAD"
};

static void emit(uint8_t type, uint16_t val) {
    g_seq++;
    DS4_DEBUG_SERIAL.print("TS:");
    // NOTE: cast to unsigned long (not unsigned long long) — the adafruit:nrf52
    // core's Print has ambiguous 64-bit print overloads. Values fit in 32 bits.
    DS4_DEBUG_SERIAL.print((unsigned long)ds4_micros());
    DS4_DEBUG_SERIAL.print(":");
    DS4_DEBUG_SERIAL.print((unsigned long)g_seq);
    DS4_DEBUG_SERIAL.print(":");
    DS4_DEBUG_SERIAL.print(type);
    DS4_DEBUG_SERIAL.print(":");
    DS4_DEBUG_SERIAL.println(val);
}

static void testButtonLatency() {
    for (uint8_t iter = 0; iter < ITERATIONS; ++iter) {
        for (uint8_t i = 0; i < DS4_BTN_COUNT; ++i) {
            emit(TYPE_BTN_PRESS, i);
            gamepad.press(i);
            gamepad.send();
            delayMicroseconds(200);

            emit(TYPE_BTN_RELEASE, i);
            gamepad.release(i);
            gamepad.send();
            delayMicroseconds(200);
        }
    }
}

static void testStickLatency() {
    for (uint8_t iter = 0; iter < ITERATIONS; ++iter) {
        emit(TYPE_STICK, 0);  gamepad.setStickLeft( 127, 0);   gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 1);  gamepad.setStickLeft(-127, 0);   gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 2);  gamepad.setStickLeft(0,  127);   gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 3);  gamepad.setStickLeft(0, -127);   gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 4);  gamepad.setStickRight(127, 0);  gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 5);  gamepad.setStickRight(-127, 0);  gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 6);  gamepad.setStickLeft(0, 0);      gamepad.send(); delayMicroseconds(200);
        emit(TYPE_STICK, 7);  gamepad.setStickRight(0, 0);     gamepad.send(); delayMicroseconds(200);
    }
}

static void testTriggerLatency() {
    for (uint8_t iter = 0; iter < ITERATIONS; ++iter) {
        emit(TYPE_TRIGGER, 0);  gamepad.setLeftTrigger(32768);  gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TRIGGER, 1);  gamepad.setLeftTrigger(0);      gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TRIGGER, 2);  gamepad.setRightTrigger(32768); gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TRIGGER, 3);  gamepad.setRightTrigger(0);     gamepad.send(); delayMicroseconds(200);
    }
}

static void testTouchLatency() {
    for (uint8_t iter = 0; iter < ITERATIONS; ++iter) {
        emit(TYPE_TOUCH, 0);  gamepad.setTouch(0, true,  960, 471); gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TOUCH, 1);  gamepad.setTouch(0, false, 0,   0);   gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TOUCH, 2);  gamepad.setTouch(1, true,  480, 235); gamepad.send(); delayMicroseconds(200);
        emit(TYPE_TOUCH, 3);  gamepad.setTouch(1, false, 0,   0);   gamepad.send(); delayMicroseconds(200);
    }
}

static void testHatLatency() {
    for (uint8_t iter = 0; iter < ITERATIONS; ++iter) {
        for (uint8_t h = 0; h <= 7; ++h) {
            emit(TYPE_HAT, h);
            gamepad.setHat(h);
            gamepad.send();
            delayMicroseconds(200);
        }
        emit(TYPE_HAT, DS4_HAT_CENTERED);
        gamepad.setHat(DS4_HAT_CENTERED);
        gamepad.send();
        delayMicroseconds(200);
    }
}

void setup() {
    DS4_DEBUG_SERIAL.begin(115200);
    delay(200);
    gamepad.begin();
    delay(200);

    DS4_DEBUG_SERIAL.print("RPT:63:");
    DS4_DEBUG_SERIAL.println(DS4_BTN_COUNT);

    for (uint8_t i = 0; i < DS4_BTN_COUNT; ++i) {
        DS4_DEBUG_SERIAL.print("BTN:");
        DS4_DEBUG_SERIAL.println(buttonNames[i]);
    }

    DS4_DEBUG_SERIAL.println("READY");
}

void loop() {
    if (!g_running) {
        if (DS4_DEBUG_SERIAL.available()) {
            String cmd = DS4_DEBUG_SERIAL.readStringUntil('\n');
            cmd.trim();
            if (cmd == "START") {
                g_seq = 0;
                g_running = true;
                emit(TYPE_BEGIN, 0);
            } else if (cmd == "SYNC") {
                DS4_DEBUG_SERIAL.print("SYNC:");
                DS4_DEBUG_SERIAL.println((unsigned long)ds4_micros());
            }
        }
        return;
    }

    testButtonLatency();
    testStickLatency();
    testTriggerLatency();
    testTouchLatency();
    testHatLatency();

    emit(TYPE_BEGIN, 1);
    g_running = false;
}
