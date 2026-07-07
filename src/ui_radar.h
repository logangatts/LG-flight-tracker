// Radar UI: sweep, aircraft blips, trails, route lines, detail panel.
#pragma once

namespace ui {

void init();

// Call every loop iteration (handles encoder, refresh, animation).
void tick();

}  // namespace ui
