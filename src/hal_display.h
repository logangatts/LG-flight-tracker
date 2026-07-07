// Display + touch HAL: panel auto-detect (SH8601/CO5300), LVGL 9 glue.
#pragma once

#include <lvgl.h>

namespace hal {

// Powers the panel, detects the revision, inits LVGL display + touch input.
// Returns false if the panel could not be initialized.
bool displayInit();

// 0..255 (AMOLED DCS brightness, no backlight PWM on this board).
void setBrightness(uint8_t level);

// True if the running board has the CST816/CO5300 (newer) panel.
bool isCo5300Panel();

}  // namespace hal
