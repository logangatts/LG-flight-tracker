// Rotary encoder + knob button (polled quadrature, no PCNT needed).
#pragma once

#include <stdint.h>

namespace hal {

void encoderInit();

// Call frequently (every loop). Internally rate-limited to ~2 ms.
void encoderPoll();

// Detents turned since last call (+CW / -CCW).
int encoderTakeSteps();

// True once per button press (debounced, on release < 700 ms).
bool encoderTakeClick();

// True once when held >= 700 ms.
bool encoderTakeLongPress();

}  // namespace hal
