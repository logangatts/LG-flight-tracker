#include "hal_encoder.h"

#include <Arduino.h>

#include "pins.h"

namespace hal {

namespace {

// Quadrature transition table: index = (prev<<2)|curr, value = -1/0/+1.
const int8_t kQuadTable[16] = {0, -1, 1, 0, 1, 0, 0, -1,
                               -1, 0, 0, 1, 0, 1, -1, 0};

// ISR-owned state. The UI loop renders for 30-60 ms per frame, far too slow
// to poll quadrature — edges must be caught by interrupts.
volatile uint8_t s_prevAB = 0;
volatile int32_t s_accum = 0;   // quarter-steps
volatile int32_t s_steps = 0;   // whole detents pending

// This encoder produces 2 quadrature transitions per physical detent
// (measured: 4 required two clicks per zoom step).
constexpr int kQuadPerDetent = 2;

void IRAM_ATTR encoderIsr() {
  uint8_t ab = (uint8_t)((gpio_get_level((gpio_num_t)PIN_ENC_A) << 1) |
                         gpio_get_level((gpio_num_t)PIN_ENC_B));
  if (ab == s_prevAB) return;
  s_accum += kQuadTable[(s_prevAB << 2) | ab];
  s_prevAB = ab;
  if (s_accum >= kQuadPerDetent) {
    s_steps++;
    s_accum -= kQuadPerDetent;
  } else if (s_accum <= -kQuadPerDetent) {
    s_steps--;
    s_accum += kQuadPerDetent;
  }
}

// Button (slow, human-speed) stays polled.
uint32_t s_lastPollMs = 0;
bool s_btnPrev = false;
uint32_t s_btnDownMs = 0;
bool s_clickPending = false;
bool s_longPending = false;
bool s_longFired = false;

}  // namespace

void encoderInit() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_KEY, INPUT_PULLUP);
  s_prevAB = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderIsr, CHANGE);
}

void encoderPoll() {
  uint32_t now = millis();
  if (now - s_lastPollMs < 5) return;
  s_lastPollMs = now;

  bool down = digitalRead(PIN_ENC_KEY) == LOW;
  if (down && !s_btnPrev) {
    s_btnDownMs = now;
    s_longFired = false;
  } else if (down && !s_longFired && (now - s_btnDownMs) >= 700) {
    s_longPending = true;
    s_longFired = true;
  } else if (!down && s_btnPrev) {
    if (!s_longFired && (now - s_btnDownMs) >= 30) s_clickPending = true;
  }
  s_btnPrev = down;
}

int encoderTakeSteps() {
  noInterrupts();
  int s = s_steps;
  s_steps = 0;
  interrupts();
  return s;
}

bool encoderTakeClick() {
  bool c = s_clickPending;
  s_clickPending = false;
  return c;
}

bool encoderTakeLongPress() {
  bool l = s_longPending;
  s_longPending = false;
  return l;
}

}  // namespace hal
