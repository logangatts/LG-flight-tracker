// Callsign -> route lookup via adsbdb.com, with a small RAM cache.
#pragma once

#include <stdint.h>

namespace routes {

// Load the persisted route cache from flash (call once, before lookups).
void loadPersist();

// Look up one pending callsign (if any). Call every few seconds from the net
// task; it self-limits to one HTTP request per call.
void serviceOne();

// Optional FlightAware AeroAPI (user-supplied key, web settings).
void setAeroKey(const char* key);  // "" disables
bool aeroActive();
uint32_t aeroUsedThisMonth();

}  // namespace routes
