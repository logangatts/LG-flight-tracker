// Small airline-logo cache (avs.io, 60x22 RGB565) for the tap card.
// UI asks with get(); misses are fetched by the net task one per cycle.
#pragma once

#include <stdint.h>

namespace logos {

struct Logo {
  const uint16_t* buf;
  uint16_t w, h;
};

// True + filled out if the logo is cached and ready. Otherwise queues a
// fetch (if unknown) and returns false — call again next UI refresh.
bool get(const char* iata, Logo* out);

// Net task side: fetch/decode one pending entry.
void service();
bool pending();

}  // namespace logos
