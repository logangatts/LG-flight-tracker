// On-demand aircraft profile: airline logo (avs.io, vector-rendered PNG at
// exact size) + registration info (adsbdb). Photos were removed: remote
// photo pipelines (planespotters/wikimedia) proved too failure-prone.
#pragma once

#include <stdint.h>

namespace photo {

enum class State : uint8_t { Idle, Pending, Ready, Failed };

struct Profile {
  char hex[8];
  char registration[12];
  char typeDesc[40];    // e.g. "Boeing 737-800"
  char owner[40];       // registered owner / operator
  char country[24];     // country of registration
  uint16_t* img;        // airline logo, RGB565, PSRAM; nullptr = use monogram
  uint16_t imgW, imgH;
};

// UI side: ask for a profile (clears any previous one). airlineIata may be
// "" — no logo is fetched and the UI draws a monogram instead.
void request(const char* hex, const char* airlineIata);
// Fetch just the logo, later — for when the route (and thus the airline)
// resolves after the page was opened.
void requestLogo(const char* airlineIata);
void cancel();

State state();
const Profile& profile();
bool imageReady();
bool infoReady();

// Net task side.
void service();
bool pending();

}  // namespace photo
