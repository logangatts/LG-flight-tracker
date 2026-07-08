// Thread-safe store of tracked aircraft (net task writes, UI task reads).
#pragma once

#include <Arduino.h>

#include "config.h"

struct TrailPoint {
  float lat;
  float lon;
  uint32_t ms;  // millis() when recorded — trails fade out by age
};

enum class RouteState : uint8_t {
  Unknown,   // not looked up yet
  Pending,   // lookup in flight
  Found,     // route known
  None,      // 404 / no route (GA, military, charter)
};

struct RouteInfo {
  char origIata[5];
  char destIata[5];
  char airline[24];
  char airlineIata[4];  // 2-letter code for the logo lookup
  float origLat, origLon;
  float destLat, destLon;
};

// Icon silhouette class, derived from ADS-B emitter category + type code.
enum class IconClass : uint8_t { Prop, Jet, Heavy, Heli };

struct Aircraft {
  bool used;
  char hex[8];        // ICAO 24-bit id, lowercase
  char callsign[10];  // trimmed
  char type[6];       // ICAO type code e.g. B738
  char squawk[6];
  IconClass iconClass;

  // 7500 hijack / 7600 radio failure / 7700 emergency
  bool isEmergency() const {
    return squawk[0] == '7' && squawk[2] == '0' && squawk[3] == '0' &&
           (squawk[1] == '5' || squawk[1] == '6' || squawk[1] == '7');
  }

  // Airline flight: known route, or an ICAO airline callsign (3 letters +
  // digits, e.g. AAL212 — GA tail numbers like N9571H never match).
  bool isCommercial() const {
    if (routeState == RouteState::Found) return true;
    return isupper((unsigned char)callsign[0]) &&
           isupper((unsigned char)callsign[1]) &&
           isupper((unsigned char)callsign[2]) &&
           isdigit((unsigned char)callsign[3]);
  }
  float lat, lon;
  float trackDeg;     // ground track
  float gsKt;         // ground speed
  int32_t altFt;      // -1 = on ground
  float vertRateFpm;
  float distNm;       // from radar center (computed on update)
  uint32_t lastSeenMs;

  RouteState routeState;
  RouteInfo route;

  TrailPoint trail[cfg::kTrailLen];
  uint8_t trailHead;   // next write slot
  uint8_t trailCount;
};

// Snapshot copy handed to the UI each frame.
struct AircraftSnapshot {
  Aircraft ac[cfg::kMaxAircraft];
  uint8_t count;
  uint32_t lastUpdateMs;   // millis() of last successful ADS-B poll
  bool apiOk;
};

class AircraftStore {
 public:
  void begin();

  // --- net task side ---
  // Begin an update cycle: marks all as not-yet-refreshed.
  void beginUpdate();
  // Upsert one aircraft from an API response. Returns slot or -1 if full.
  int upsert(const char* hex, const char* callsign, const char* type,
             const char* squawk, IconClass iconClass, float lat, float lon,
             float trackDeg, float gsKt, int32_t altFt, float vertRateFpm,
             float centerLat, float centerLon);
  // Finish cycle: expire stale aircraft, record success/failure.
  void endUpdate(bool ok);

  // Route results (keyed by callsign — an aircraft may have respawned).
  void setRoutePending(const char* callsign);
  void setRouteFound(const char* callsign, const RouteInfo& info);
  void setRouteNone(const char* callsign);
  void setRouteUnknown(const char* callsign);  // transient failure: retry later

  // Next callsign needing a route lookup (nearest first; the priority hex —
  // the user's selected aircraft — jumps the queue). Copies into out (>=10).
  bool nextRouteCandidate(char* out);
  void setPriorityHex(const char* hex);  // "" clears

  // Current position of an aircraft by callsign (for route plausibility).
  bool getPosByCallsign(const char* callsign, float* lat, float* lon);

  // Immediately drop aircraft whose callsign doesn't start with prefix
  // (empty prefix = no-op). Called when the airline filter changes so the
  // switch is instant instead of waiting out the 30 s aging TTL.
  void purgeNonCallsign(const char* prefix);

  // --- UI task side ---
  void snapshot(AircraftSnapshot& out);
  int count();

 private:
  int findByHex(const char* hex);
  void applyRoute(const char* callsign, RouteState st, const RouteInfo* info);

  Aircraft* ac_ = nullptr;  // [kMaxAircraft], allocated in PSRAM
  char priorityHex_[8] = {0};
  uint32_t lastUpdateMs_ = 0;
  bool apiOk_ = false;
  SemaphoreHandle_t mutex_ = nullptr;
};

extern AircraftStore gAircraft;
