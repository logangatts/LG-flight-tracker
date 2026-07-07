// Auto-location: NVS cache -> manual override -> WiFi geolocation (beaconDB)
// -> IP geolocation (ip-api.com). Writes result into gApp.
#pragma once

namespace geolocate {

// Load cached / manual location from NVS into gApp. Returns true if found.
bool loadStored();

// Store a manual "lat, lon" string (from the setup portal). Returns false on
// parse error.
bool saveManual(const char* text);

// Run online geolocation (requires WiFi). Skipped if a manual location is set.
// Updates gApp + NVS cache. Returns true if a location was obtained.
bool run();

// Clear manual override + cache (next run() re-locates).
void reset();

}  // namespace geolocate
