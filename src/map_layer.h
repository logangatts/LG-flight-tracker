// Street-map underlay: CARTO dark tiles composited into an RGB565 canvas.
#pragma once

#include <stdint.h>

namespace maplayer {

void begin();

// Called from the net task: refetches the canvas if center/zoom changed.
// Does nothing when the map is disabled or WiFi is down.
void service();

// UI side: pointer to the current 390x390 RGB565 canvas, or nullptr if not
// ready. Never torn — buffers are swapped atomically after a full render.
const uint16_t* canvas();

// True once after each canvas swap (UI uses it to trigger a redraw).
bool takeDirty();

}  // namespace maplayer
