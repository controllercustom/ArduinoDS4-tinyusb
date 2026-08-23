// SPDX-License-Identifier: MIT
// Copyright (c) 2026 controllercustom@myyahoo.com

// AutoCycle — enhanced phased test for pcap analysis and hardware verification.
// P0-P11 phases, deterministic stimulus tables, ~4 min full cycle at 10 Hz loop.
// Telemetry on DS4_DEBUG_SERIAL (Serial0 on ESP32, Serial1 on RP2040).

#include <DS4Gamepad.h>

DS4Gamepad gamepad;

static void onLed(uint8_t val) {
  DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "CB_LED:%u\n", val);
}

static void onRumble(uint8_t left, uint8_t right) {
  DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "CB_RUMBLE:left=%u,right=%u\n", left, right);
}

static void printTS() {
  DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "TS:%llu\n", (unsigned long long)ds4_micros());
}

// Send an unmistakable "all-buttons-pressed" marker frame between phases.
static void sendPhaseMarker() {
  // Reset all state first — clears sticks, triggers, d-pad from prior phase.
  gamepad.releaseAll();

  for (uint8_t b = 0; b < DS4_BTN_COUNT; b++) {
    gamepad.press(b);
  }
  gamepad.send();
  DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "MARKER\n");
  // Wait two poll cycles so the marker frame is fully transmitted on wire.
  delay(8);

  gamepad.releaseAll();
}

// ---------------------------------------------------------------------------
// Phase state tables (deterministic, one entry per iteration)
// ---------------------------------------------------------------------------

// P1: individual button press cycle (all 14).
static const uint8_t kBtnOrder[DS4_BTN_COUNT] = {
  DS4_BTN_SQUARE, DS4_BTN_CROSS, DS4_BTN_CIRCLE, DS4_BTN_TRIANGLE,
  DS4_BTN_L1, DS4_BTN_R1, DS4_BTN_L2, DS4_BTN_R2,
  DS4_BTN_SHARE, DS4_BTN_OPTIONS, DS4_BTN_L3, DS4_BTN_R3,
  DS4_BTN_PS, DS4_BTN_TOUCHPAD
};

// P2: d-pad cardinal + diagonal directions.
static const uint8_t kDpadOrder[10] = {
  DS4_HAT_UP, DS4_HAT_UP_RIGHT, DS4_HAT_RIGHT, DS4_HAT_DOWN_RIGHT, DS4_HAT_DOWN,
  DS4_HAT_DOWN_LEFT, DS4_HAT_LEFT, DS4_HAT_UP_LEFT, DS4_HAT_CENTERED, DS4_HAT_CENTERED,
};

// P3/P4: stick sweep with intermediate positions for deadzone characterization.
// Full int16_t range (-32768..+32767) to validate mapStick() at extremes and midpoints.
static const uint8_t kSweepLen = 15;
static const int16_t kStickSweep[15][2] = {
  {-29491,      0}, // ~-90% X
  {-19660,      0}, // ~-60% X
  { -9830,      0}, // ~-30% X
  {     0,      0}, // center (hold)
  { +9830,      0}, // ~+30% X
  {+19660,      0}, // ~+60% X
  {+29491,      0}, // ~+90% X

  {     0, -29491}, // ~-90% Y
  {     0, -19660}, // ~-60% Y
  {     0,  -9830}, // ~-30% Y
  {     0,      0}, // center (hold)
  {     0, +24576}, // ~+75% Y
  {     0, +16384}, // ~+50% Y
  {     0, +29491}, // ~+90% Y

  {-32768, -32768}, // extreme corner — verifies mapStick() boundary clamping → byte[0]=0x00
};

// P3/P4: circle sweep sub-sequence (diagonal positions at varying radii).
static const uint8_t kCircleLen = 9;
static const int16_t kStickCircle[9][2] = {
  {-16384, -16384}, // ~50% radius: top-left
  {+16384, -16384}, // top-right
  {+16384, +16384}, // bottom-right
  {-16384, +16384}, // bottom-left

  {-24576,-24576}, // ~75% radius: same quadrants
  {+24576,-24576},
  {+24576,+24576},
  {-24576, +24576},

  {     0,      0}, // return to center
};

// P5/P6: smooth trigger ramp (0 -> full press -> release).
static const uint16_t kTriggerRamp[8] = {
    0,   4096,   8192,  16384,  24576,  32768,  16384,     0,
};

// P7/P8: touchpad steps. NOTE (verified against real DS4): byte[33] TPADACT is
// always 0x01 even with no finger down; lifted fingers keep their state+coords.
struct TouchStep { uint8_t f0down; uint16_t f0x, f0y; uint8_t f1down; uint16_t f1x, f1y; };

static const TouchStep kTouchSingle[12] = {
  // Drag left->right along bottom edge.
  { 1,    0,   942,  0,   0,     0},
  { 1,  639,  942,  0,   0,     0},
  { 1, 1279,  942,  0,   0,     0},
  { 1, 1919,  942,  0,   0,     0},
  // Drag top->bottom along left edge.
  { 1,    0,     0,  0,   0,     0},
  { 1,    0,   314,  0,   0,     0},
  { 1,    0,   629,  0,   0,     0},
  // Quick taps (down 1 iter -> up next).
  { 1,  960,   471,  0,   0,     0},
  { 0,  960,   471,  0,   0,     0},
  { 1,  960,   471,  0,   0,     0},
  // Lift and neutralize.
  { 0,    0,     0,  0,   0,     0},
};

static const TouchStep kTouchTwo[12] = {
  // Both fingers down at spread positions.
  { 1,  300, 300,  1, 1600,   600},
  { 1,  400, 350,  1, 1500,   550},
  // Pinch in (fingers move toward center).
  { 1,  700, 400,  1, 1200,   500},
  { 1,  850, 430,  1, 1050,   490},
  // Pinch out (fingers spread apart).
  { 1,  600, 200,  1, 1300,   700},
  { 1,  400, 150,  1, 1500,   800},
  // Lift finger-0 only (finger-1 retains coords).
  { 0,  600, 200,  1, 1300,   700},
  { 0,  600, 200,  1, 1400,   650},
  // Lift finger-1 only (finger-0 still up).
  { 0,  600, 200,  0, 1400,   650},
  // Both down again for final verification.
  { 1,    0,     0,  1, 1919,     942},
  // Full lift -> neutral.
  { 0,    0,     0,  0,    0,       0},
};

// P9: PS + TP-click isolation — per-iteration flags (bit 0 = press PS this iter,
// bit 1 = press TP-click).
static const uint8_t kPSFlags[10] = {
  0x00, // idle.
  0x01, // press PS alone.
  0x01, // hold PS.
  0x00, // release PS.
  0x02, // touchpad-click without finger down.
  0x02, // hold TP-click.
  0x00, // release.
  0x03, // both simultaneously.
  0x03, // hold both.
  0x00, // release all special buttons.
};

static const TouchStep kPSTouch[10] = {
  { 0,    0,   0,  0,   0,   0}, // idle.
  { 1, 960, 471,  0,   0,   0}, // finger down at center.
  { 1, 960, 471,  0,   0,   0}, // hold while PS pressed (above).
  { 1, 960, 471,  0,   0,   0}, // hold after PS release.
  { 1, 500, 200,  0,   0,   0}, // move finger while TP-click pressed.
  { 1, 500, 200,  0,   0,   0}, // hold.
  { 1, 960, 471,  0,   0,   0}, // return to center after release.
  { 1, 300, 300,  0,   0,   0}, // move while both PS+TP pressed.
  { 1, 960, 471,  0,   0,   0}, // return after release of both.
  { 0,    0,   0,  0,   0,   0}, // lift finger -> neutral.
};

// Phase iteration counts per phase (P0..P11).
static const uint8_t kPhaseIters[12] = {
  50,    // P0: idle baseline (~5s)
  DS4_BTN_COUNT * 3 + 6, // P1: each button pressed/released with gaps (≈48 iters)
  10,    // P2: d-pad cycle.
  kSweepLen + kCircleLen, // P3: left stick sweep + circle.
  kSweepLen + kCircleLen, // P4: right stick sweep + circle.
  8,     // P5: L2 smooth ramp (~1s).
  8,     // P6: R2 smooth ramp (~1s).
  12,    // P7: single-finger touchpad drag+taps (~1s).
  12,    // P8: two-finger pinch + selective lifts (~1s).
  10,    // P9: PS + TP-click isolation (~1s).
  35,    // P10: stress test (4 all-input iters + ~20 rapid-fire + trailing idle)
  50,    // P11: IMU rest / sustained idle (~5s).
};

// ---------------------------------------------------------------------------

void setup() {
  DS4_DEBUG_SERIAL.begin(115200);
  delay(200);
  gamepad.onLed(onLed);
  gamepad.onRumble(onRumble);
  gamepad.begin();
  gamepad.setPollInterval(0);   // disabled — explicit send() per iteration only

  unsigned long start = millis();
  while (!gamepad.ready() && (millis() - start) < 5000UL) {
    delay(10);
  }
  DS4_DEBUG_SERIAL.println("READY");
}

void loop() {
  static uint8_t phase = 0;
  static uint8_t seq = 0;
  static uint32_t globalIter = 0;

  // ---------------------------------------------------------------- P0: idle baseline.
  if (phase == 0) {
    gamepad.releaseAll();
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=0 SEQ=%u\n", globalIter);
  }

  // ---------------------------------------------------------------- P1: individual buttons.
  else if (phase == 1) {
    gamepad.releaseAll();
    uint8_t btnIdx = seq % DS4_BTN_COUNT;
    uint8_t pressCycle = (seq / DS4_BTN_COUNT) % 3;
    if (pressCycle < 2) { // Press for 2 iterations, release for 1.
      gamepad.press(kBtnOrder[btnIdx]);
    }
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=1 SEQ=%u BTN=%d\n", globalIter, kBtnOrder[btnIdx]);
  }

  // ---------------------------------------------------------------- P2: d-pad cycle.
  else if (phase == 2) {
    gamepad.releaseAll();
    gamepad.setDpad(kDpadOrder[seq]);
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=2 SEQ=%u DPAD=0x%02X\n", globalIter, kDpadOrder[seq]);
  }

  // ---------------------------------------------------------------- P3: left stick sweep + circle.
  else if (phase == 3) {
    gamepad.releaseAll();
    int16_t sx, sy;
    if (seq < kSweepLen) {
      sx = kStickSweep[seq][0];
      sy = kStickSweep[seq][1];
      gamepad.setStickLeft(sx, sy);
    } else {
      uint8_t ci = seq - kSweepLen;
      sx = kStickCircle[ci][0];
      sy = kStickCircle[ci][1];
      gamepad.setStickLeft(sx, sy);
    }
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=3 SEQ=%u LX=(%d,%d)\n", globalIter, sx, sy);
  }

  // ---------------------------------------------------------------- P4: right stick sweep + circle.
  else if (phase == 4) {
    gamepad.releaseAll();
    int16_t sx, sy;
    if (seq < kSweepLen) {
      sx = kStickSweep[seq][0];
      sy = kStickSweep[seq][1];
      gamepad.setStickRight(sx, sy);
    } else {
      uint8_t ci = seq - kSweepLen;
      sx = kStickCircle[ci][0];
      sy = kStickCircle[ci][1];
      gamepad.setStickRight(sx, sy);
    }
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=4 SEQ=%u RX=(%d,%d)\n", globalIter, sx, sy);
  }

  // ---------------------------------------------------------------- P5: L2 smooth ramp.
  else if (phase == 5) {
    gamepad.releaseAll();
    gamepad.setLeftTrigger(kTriggerRamp[seq]);
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=5 SEQ=%u L2=%u\n", globalIter, kTriggerRamp[seq]);
  }

  // ---------------------------------------------------------------- P6: R2 smooth ramp.
  else if (phase == 6) {
    gamepad.releaseAll();
    gamepad.setRightTrigger(kTriggerRamp[seq]);
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=6 SEQ=%u R2=%u\n", globalIter, kTriggerRamp[seq]);
  }

  // --------------------------------------------------------------- P7: single-finger touchpad.
  else if (phase == 7) {
    gamepad.releaseAll();
    const TouchStep &t = kTouchSingle[seq];
    gamepad.setTouch(0, t.f0down != 0, t.f0x, t.f0y);
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=7 SEQ=%u F0=%c(%u,%u)\n", globalIter,
                     t.f0down ? 'D' : 'U', (unsigned)t.f0x, (unsigned)t.f0y);
  }

  // --------------------------------------------------------------- P8: two-finger touchpad.
  else if (phase == 8) {
    gamepad.releaseAll();
    const TouchStep &t = kTouchTwo[seq];
    gamepad.setTouch(0, t.f0down != 0, t.f0x, t.f0y);
    gamepad.setTouch(1, t.f1down != 0, t.f1x, t.f1y);
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=8 SEQ=%u F0=%c(%u,%u) F1=%c(%u,%u)\n", globalIter,
                     t.f0down ? 'D' : 'U', (unsigned)t.f0x, (unsigned)t.f0y,
                     t.f1down ? 'D' : 'U', (unsigned)t.f1x, (unsigned)t.f1y);
  }

  // --------------------------------------------------------------- P9: PS + TP-click isolation.
  else if (phase == 9) {
    gamepad.releaseAll();
    const TouchStep &t = kPSTouch[seq];
    gamepad.setTouch(0, t.f0down != 0, t.f0x, t.f0y);

    uint8_t flags = kPSFlags[seq];
    if (flags & 0x01) { // bit-0: PS button.
      gamepad.press(DS4_BTN_PS);
    } else {
      gamepad.release(DS4_BTN_PS);
    }
    if (flags & 0x02) { // bit-1: touchpad-click.
      gamepad.press(DS4_BTN_TOUCHPAD);
    } else {
      gamepad.release(DS4_BTN_TOUCHPAD);
    }

    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=9 SEQ=%u PS=%d TPclick=%s F0=%c(%u,%u)\n", globalIter,
                     (flags & 0x01) ? 1 : 0,
                     (flags & 0x02) ? "D" : "U",
                     t.f0down ? 'D' : 'U',
                     (unsigned)t.f0x, (unsigned)t.f0y);
  }

  // --------------------------------------------------------------- P10: stress test.
  else if (phase == 10) {
    gamepad.releaseAll();
    if (seq < 4) {
      // All inputs at once — every button pressed, sticks deflected, d-pad UP, triggers half.
      for (uint8_t b = 0; b < DS4_BTN_COUNT; b++) {
        gamepad.press(kBtnOrder[b]);
      }
      gamepad.setStickLeft(-30000, -30000);
      gamepad.setStickRight(30000, 30000);
      gamepad.setDpad(DS4_HAT_UP);
      gamepad.setLeftTrigger(16384);
      gamepad.setRightTrigger(16384);
      DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=10 SEQ=%u ALL_INPUTS\n", globalIter);
    } else if (seq < 25) {
      // Rapid-fire SQUARE button — press/release every iteration.
      uint8_t rapidSeq = seq - 4;
      gamepad.releaseAll();
      if ((rapidSeq % 2) == 0) {
        gamepad.press(DS4_BTN_SQUARE);
      } else {
        gamepad.release(DS4_BTN_SQUARE);
      }
    } else {
      // Trailing idle.
      DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=10 SEQ=%u STRESS_IDLE\n", globalIter);
    }
  }

  // --------------------------------------------------------------- P11: IMU rest / sustained idle.
  else if (phase == 11) {
    gamepad.releaseAll();
    DS4_DBG_PRINTF(DS4_DEBUG_SERIAL, "PHASE=11 SEQ=%u\n", globalIter);
  }

  printTS();

  seq++;
  globalIter++;

  // Explicit send + delay to guarantee at least one clean frame per iteration.
  (void)gamepad.send();
  delay(8);

  if (seq >= kPhaseIters[phase]) {
    sendPhaseMarker();          // unmistakable all-buttons marker between phases
    phase = (uint8_t)((phase + 1) % 12);
    seq = 0;
    if (phase == 0) {
      globalIter = 0; // full cycle complete -> start over.
    }
  }

  delay(100);
}
