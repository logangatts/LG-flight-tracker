// Live traffic poller — airplanes.live /v2/point (adsb.lol fallback).
#pragma once

#include <stdint.h>

namespace adsb {

// One poll cycle: fetch aircraft around gApp center and update gAircraft.
// Returns true on success. Sets *retryAfterMs on rate limiting.
bool poll(uint32_t* retryAfterMs);

}  // namespace adsb
