// SPDX-License-Identifier: MIT
// DS4Gamepad — Output callback (rumble + LED) unit tests.
//
// Registers rumble/LED callbacks and waits for host-injected output report 0x05
// packets, printing telemetry to DS4_DEBUG_SERIAL for the pyusb test harness to
// verify. Host harness: scripts/test_output_packets.py
//
// Report 0x05 payload (after the Report ID byte):
//   [0]=valid_flags [1]=flags [2]=rsvd [3]=motor_right
//   [4]=motor_left  [5]=lightbar_red [6]=lightbar_green [7]=lightbar_blue
// Callback contract: onRumble(leftMotor, rightMotor); onLed(lightbar_red).
//
// Note: the Linux hid-playstation driver may send a lightbar palette report on
// bind, so do not require zero callbacks before the host harness starts.

#include <DS4Gamepad.h>

static DS4Gamepad gamepad;

static uint16_t pass = 0;
static uint16_t fail = 0;
static bool usbReady = false;

#define CHECK(expr, msg) do { \
    if (expr) { pass++; } \
    else { fail++; DS4_DEBUG_SERIAL.print("FAIL: "); DS4_DEBUG_SERIAL.println(msg); } \
  } while(0)

static uint32_t rumbleCount = 0;
static uint8_t cbRumbleL = 0, cbRumbleR = 0;
static uint32_t ledCount = 0;
static uint8_t cbLedValue = 0;

static DS4Gamepad::DS4LED cbLedColor{0, 0, 0};

void onRumble(uint8_t leftMotor, uint8_t rightMotor) {
    rumbleCount++;
    cbRumbleL = leftMotor;
    cbRumbleR = rightMotor;
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "CB_RUMBLE:%u,%u\n", leftMotor, rightMotor);
}

void onLed(uint8_t ledValue) {
    ledCount++;
    cbLedValue = ledValue;
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "CB_LED:%u\n", ledValue);
}

static void onLedColor(const DS4Gamepad::DS4LED &c) {
    cbLedColor.r = c.r;
    cbLedColor.g = c.g;
    cbLedColor.b = c.b;
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "CB_LEDRGB:%u,%u,%u\n", c.r, c.g, c.b);
}

static void testInitialState() {
    DS4_DEBUG_SERIAL.println("--- Initial State ---");
    CHECK(gamepad.getLEDState() == 0, "getLEDState initial zero");
    CHECK(cbLedValue == 0, "led callback value initial zero");
    CHECK(cbRumbleL == 0 && cbRumbleR == 0, "rumble callback values initial zero");
    CHECK(rumbleCount == 0, "no rumble callback before USB mount");
    CHECK(ledCount == 0, "no led callback before USB mount");
}

static void testCallbackRegistration() {
    DS4_DEBUG_SERIAL.println("--- Callback Registration ---");
    gamepad.onRumble(onRumble);
    gamepad.onLed(onLed);
    gamepad.onLedColor(onLedColor);
    CHECK(true, "callbacks registered without crash");
}

static void waitForUsb() {
    uint32_t t0 = millis();
    while (!gamepad.ready() && (millis() - t0) < 5000UL) {
        delay(10);
    }
    if (gamepad.ready()) {
        usbReady = true;
        DS4_DEBUG_SERIAL.println("USB ready");
    } else {
        DS4_DEBUG_SERIAL.println("WARNING: USB not ready after 5s — OUT packet tests will be skipped");
    }
}

static void testUsbOutput() {
    if (!usbReady) return;
    DS4_DEBUG_SERIAL.println("--- USB Output (awaiting pyusb packets) ---");
    rumbleCount = 0;
    ledCount = 0;
}

void setup() {
    DS4_DEBUG_SERIAL.begin(115200);
    delay(200);

    gamepad.begin();
    delay(200);

    testInitialState();
    testCallbackRegistration();
    waitForUsb();
    testUsbOutput();

    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "\n=== PRE-USB RESULTS: PASS=%u FAIL=%u ===\n", pass, fail);
}

void loop() {
    static uint32_t lastCheckedLed = 0;
    if (ledCount > lastCheckedLed) {
        CHECK(gamepad.getLEDState() == cbLedValue, "getLEDState matches LED callback");
        DS4Gamepad::DS4LED gc = gamepad.getLEDColor();
        CHECK(gc.r == cbLedColor.r && gc.g == cbLedColor.g && gc.b == cbLedColor.b,
              "getLEDColor matches RGB callback");
        lastCheckedLed = ledCount;
    }

    static unsigned long lastReady = 0;
    if (usbReady && (millis() - lastReady) >= 500) {
        lastReady = millis();
        DS4_DEBUG_SERIAL.println("READY_FOR_PACKETS");
    }

    static unsigned long lastReport = 0;
    if ((millis() - lastReport) >= 1000UL && usbReady) {
        lastReport = millis();
        DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "SUMMARY: rumble=%u led=%u pass=%u fail=%u\n",
                       rumbleCount, ledCount, pass, fail);

        static uint8_t idleSeconds = 0;
        if (rumbleCount == 0 && ledCount == 0) {
            idleSeconds++;
            if (idleSeconds >= 15) {
                DS4_DEBUG_SERIAL.println("\n=== TIMEOUT: No OUT packets received ===");
                DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "FINAL RESULTS: PASS=%u FAIL=%u\n", pass, fail);
                while(1) delay(1000);
            }
        } else {
            idleSeconds = 0;
        }
    }

    if (DS4_DEBUG_SERIAL.available()) {
        String cmd = DS4_DEBUG_SERIAL.readStringUntil('\n');
        cmd.trim();
        if (cmd == "DONE") {
            uint32_t finalRumble = rumbleCount;
            uint32_t finalLed = ledCount;
            DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "\n=== FINAL RESULTS: PASS=%u FAIL=%u RUMBLE_PKTS=%u LED_PKTS=%u ===\n",
                           pass, fail, finalRumble, finalLed);
            if (fail == 0 && finalRumble > 0 && finalLed > 0) {
                DS4_DEBUG_SERIAL.println("ALL TESTS PASSED");
            } else if (finalRumble == 0 || finalLed == 0) {
                DS4_DEBUG_SERIAL.println("INCOMPLETE: Not all packet types received");
            } else {
                DS4_DEBUG_SERIAL.println("TESTS FAILED");
            }
            while(1) delay(1000);
        }
    }

    delay(2);
}
