#include "aircraft_store.h"

#include <string.h>

#include "geo_math.h"

AircraftStore gAircraft;

namespace {
// Per-cycle "seen" flags live outside the struct to keep snapshots small.
bool s_seen[cfg::kMaxAircraft];
}  // namespace

void AircraftStore::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // Keep internal RAM free for TLS — the store lives in PSRAM.
  ac_ = (Aircraft*)heap_caps_calloc(cfg::kMaxAircraft, sizeof(Aircraft),
                                    MALLOC_CAP_SPIRAM);
  if (!ac_) {  // no PSRAM? fall back to internal
    ac_ = (Aircraft*)calloc(cfg::kMaxAircraft, sizeof(Aircraft));
  }
}

int AircraftStore::findByHex(const char* hex) {
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (ac_[i].used && strncmp(ac_[i].hex, hex, sizeof(ac_[i].hex) - 1) == 0)
      return i;
  }
  return -1;
}

void AircraftStore::beginUpdate() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  memset(s_seen, 0, sizeof(s_seen));
  xSemaphoreGive(mutex_);
}

int AircraftStore::upsert(const char* hex, const char* callsign,
                          const char* type, const char* squawk,
                          IconClass iconClass, float lat, float lon,
                          float trackDeg, float gsKt, int32_t altFt,
                          float vertRateFpm, float centerLat, float centerLon) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  int idx = findByHex(hex);
  if (idx < 0) {
    for (int i = 0; i < cfg::kMaxAircraft; i++) {
      if (!ac_[i].used) {
        idx = i;
        break;
      }
    }
    if (idx < 0) {
      xSemaphoreGive(mutex_);
      return -1;
    }
    Aircraft& a = ac_[idx];
    memset(&a, 0, sizeof(a));
    a.used = true;
    strlcpy(a.hex, hex, sizeof(a.hex));
    a.routeState = RouteState::Unknown;
  }

  Aircraft& a = ac_[idx];
  // Callsign can appear a few polls after first contact; route lookup keys on
  // it, so reset route state if it changes.
  if (callsign[0] && strncmp(a.callsign, callsign, sizeof(a.callsign)) != 0) {
    strlcpy(a.callsign, callsign, sizeof(a.callsign));
    a.routeState = RouteState::Unknown;
  }
  if (type[0]) strlcpy(a.type, type, sizeof(a.type));
  strlcpy(a.squawk, squawk, sizeof(a.squawk));
  a.iconClass = iconClass;

  // Trail: append when moved far enough.
  bool append = (a.trailCount == 0);
  if (!append) {
    uint8_t lastIdx =
        (a.trailHead + cfg::kTrailLen - 1) % cfg::kTrailLen;
    append = geo::distanceNm(a.trail[lastIdx].lat, a.trail[lastIdx].lon, lat,
                             lon) >= cfg::kTrailMinMoveNm;
  }
  if (append) {
    a.trail[a.trailHead] = {lat, lon, millis()};
    a.trailHead = (a.trailHead + 1) % cfg::kTrailLen;
    if (a.trailCount < cfg::kTrailLen) a.trailCount++;
  }

  a.lat = lat;
  a.lon = lon;
  a.trackDeg = trackDeg;
  a.gsKt = gsKt;
  a.altFt = altFt;
  a.vertRateFpm = vertRateFpm;
  a.distNm = geo::distanceNm(centerLat, centerLon, lat, lon);
  a.lastSeenMs = millis();
  s_seen[idx] = true;
  xSemaphoreGive(mutex_);
  return idx;
}

void AircraftStore::endUpdate(bool ok) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  uint32_t now = millis();
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (!ac_[i].used) continue;
    if (!s_seen[i] && (now - ac_[i].lastSeenMs) > cfg::kAircraftTtlMs) {
      ac_[i].used = false;
    }
  }
  if (ok) lastUpdateMs_ = now;
  apiOk_ = ok;
  xSemaphoreGive(mutex_);
}

void AircraftStore::applyRoute(const char* callsign, RouteState st,
                               const RouteInfo* info) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (!ac_[i].used) continue;
    if (strncmp(ac_[i].callsign, callsign, sizeof(ac_[i].callsign)) != 0)
      continue;
    ac_[i].routeState = st;
    if (info) ac_[i].route = *info;
  }
  xSemaphoreGive(mutex_);
}

void AircraftStore::setRoutePending(const char* callsign) {
  applyRoute(callsign, RouteState::Pending, nullptr);
}
void AircraftStore::setRouteFound(const char* callsign, const RouteInfo& info) {
  applyRoute(callsign, RouteState::Found, &info);
}
void AircraftStore::setRouteNone(const char* callsign) {
  applyRoute(callsign, RouteState::None, nullptr);
}
void AircraftStore::setRouteUnknown(const char* callsign) {
  applyRoute(callsign, RouteState::Unknown, nullptr);
}

void AircraftStore::setPriorityHex(const char* hex) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  strlcpy(priorityHex_, hex ? hex : "", sizeof(priorityHex_));
  xSemaphoreGive(mutex_);
}

bool AircraftStore::nextRouteCandidate(char* out) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  // Selected aircraft first — its route drives the visible card/page.
  if (priorityHex_[0]) {
    for (int i = 0; i < cfg::kMaxAircraft; i++) {
      if (ac_[i].used && ac_[i].routeState == RouteState::Unknown &&
          ac_[i].callsign[0] &&
          strncmp(ac_[i].hex, priorityHex_, sizeof(priorityHex_)) == 0) {
        strlcpy(out, ac_[i].callsign, 10);
        xSemaphoreGive(mutex_);
        return true;
      }
    }
  }
  float bestDist = 1e9f;
  int best = -1;
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (!ac_[i].used) continue;
    if (ac_[i].routeState != RouteState::Unknown) continue;
    if (!ac_[i].callsign[0]) continue;
    if (ac_[i].distNm < bestDist) {
      bestDist = ac_[i].distNm;
      best = i;
    }
  }
  if (best >= 0) strlcpy(out, ac_[best].callsign, 10);
  xSemaphoreGive(mutex_);
  return best >= 0;
}

bool AircraftStore::getPosByCallsign(const char* callsign, float* lat,
                                     float* lon) {
  bool found = false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (!ac_[i].used) continue;
    if (strncmp(ac_[i].callsign, callsign, sizeof(ac_[i].callsign)) != 0)
      continue;
    *lat = ac_[i].lat;
    *lon = ac_[i].lon;
    found = true;
    break;
  }
  xSemaphoreGive(mutex_);
  return found;
}

void AircraftStore::purgeNonCallsign(const char* prefix) {
  if (!prefix || !prefix[0]) return;
  size_t n = strlen(prefix);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (ac_[i].used && strncmp(ac_[i].callsign, prefix, n) != 0)
      ac_[i].used = false;
  }
  xSemaphoreGive(mutex_);
}

int AircraftStore::count() {
  int n = 0;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (ac_[i].used) n++;
  }
  xSemaphoreGive(mutex_);
  return n;
}

void AircraftStore::snapshot(AircraftSnapshot& out) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  out.count = 0;
  for (int i = 0; i < cfg::kMaxAircraft; i++) {
    if (ac_[i].used) out.ac[out.count++] = ac_[i];
  }
  out.lastUpdateMs = lastUpdateMs_;
  out.apiOk = apiOk_;
  xSemaphoreGive(mutex_);
}
