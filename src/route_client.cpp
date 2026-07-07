#include "route_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

#include "airports_data.h"
#include "app_state.h"

#include "aircraft_store.h"
#include "config.h"
#include "geo_math.h"

namespace routes {

namespace {

// Route databases are static callsign->route mappings and go stale when
// airlines reshuffle flight numbers. Reject a route if the aircraft is
// nowhere near the origin->destination great circle (tar1090-style check).
bool routePlausible(const char* callsign, const RouteInfo& info) {
  if (info.origLat == 0 && info.origLon == 0) return true;  // no coords
  if (info.destLat == 0 && info.destLon == 0) return true;

  float acLat, acLon;
  if (!gAircraft.getPosByCallsign(callsign, &acLat, &acLon)) return true;

  float routeDist = geo::distanceNm(info.origLat, info.origLon, info.destLat,
                                    info.destLon);
  if (routeDist < 100) return true;  // short hops: geometry too tight to judge

  // Cross-track distance of the aircraft from the great-circle route.
  float d13 = geo::distanceNm(info.origLat, info.origLon, acLat, acLon) /
              geo::kEarthRadiusNm;  // angular
  float b13 = geo::bearingDeg(info.origLat, info.origLon, acLat, acLon) *
              geo::kDeg2Rad;
  float b12 = geo::bearingDeg(info.origLat, info.origLon, info.destLat,
                              info.destLon) *
              geo::kDeg2Rad;
  float xtdNm = fabsf(asinf(sinf(d13) * sinf(b13 - b12))) *
                geo::kEarthRadiusNm;

  // Also reject if the plane is far beyond the destination ("overshoot").
  float distToDest = geo::distanceNm(acLat, acLon, info.destLat, info.destLon);
  float distFromOrig = geo::distanceNm(acLat, acLon, info.origLat, info.origLon);
  bool overshoot = distFromOrig > routeDist * 1.25f + 100;

  float threshold = max(80.0f, routeDist * 0.15f);
  if (xtdNm > threshold || overshoot) {
    Serial.printf(
        "[route] %s: %s->%s implausible (xtd %.0f nm, thr %.0f) — hiding\n",
        callsign, info.origIata, info.destIata, xtdNm, threshold);
    (void)distToDest;
    return false;
  }
  return true;
}

// Route cache: RAM (PSRAM) for speed, mirrored to LittleFS so it survives
// reboots. The device gets MORE reliable over time — daily regulars resolve
// instantly and offline. Entries age out after kRouteTtlDays so seasonal
// schedule changes still refresh.
enum : uint8_t { kSrcDb = 0, kSrcAero = 1, kSrcAirlabs = 2 };

struct CacheEntry {
  char callsign[10];
  bool found;
  uint8_t srcKind;    // kSrcDb/kSrcAero/kSrcAirlabs — sets the TTL below
  uint16_t epochDay;  // day the route was fetched (0 = unknown clock)
  RouteInfo info;
};

CacheEntry* s_cache = nullptr;   // allocated in PSRAM
uint16_t s_cacheCount = 0;
uint16_t s_cacheNext = 0;        // round-robin eviction
uint32_t s_fileRecords = 0;
bool s_fsOk = false;

constexpr uint32_t kFileMagic = 0x46525433;  // "FRT3" (srcAero -> srcKind)

// ---- optional enhanced route sources (user-supplied keys) ----
char s_aeroKey[72] = {0};
uint16_t s_aeroMonth = 0;
uint32_t s_aeroCount = 0;

char s_airlabsKey[48] = {0};
uint16_t s_airlabsMonth = 0;
uint32_t s_airlabsCount = 0;

void aeroSaveCounters() {
  Preferences p;
  p.begin("keys", false);
  p.putUShort("aeroM", s_aeroMonth);
  p.putUInt("aeroC", s_aeroCount);
  p.end();
}

void airlabsSaveCounters() {
  Preferences p;
  p.begin("keys", false);
  p.putUShort("alM", s_airlabsMonth);
  p.putUInt("alC", s_airlabsCount);
  p.end();
}

uint16_t todayEpochDay() {
  uint32_t s = gApp.epochSec.load();
  return s ? (uint16_t)(s / 86400 % 65536) : 0;
}

CacheEntry* cacheFind(const char* cs) {
  for (uint16_t i = 0; i < s_cacheCount; i++) {
    if (strncmp(s_cache[i].callsign, cs, sizeof(s_cache[i].callsign)) == 0)
      return &s_cache[i];
  }
  return nullptr;
}

CacheEntry* cachePut(const char* cs, bool found, const RouteInfo* info,
                     uint16_t epochDay) {
  if (!s_cache) {
    s_cache = (CacheEntry*)ps_malloc(sizeof(CacheEntry) * cfg::kRouteCacheMax);
    if (!s_cache) return nullptr;
  }
  CacheEntry* e = cacheFind(cs);
  if (!e) {
    if (s_cacheCount < cfg::kRouteCacheMax) {
      e = &s_cache[s_cacheCount++];
    } else {
      e = &s_cache[s_cacheNext];
      s_cacheNext = (s_cacheNext + 1) % cfg::kRouteCacheMax;
    }
  }
  memset(e, 0, sizeof(*e));
  strlcpy(e->callsign, cs, sizeof(e->callsign));
  e->found = found;
  e->epochDay = epochDay;
  if (info) e->info = *info;
  return e;
}

void persistRewrite() {
  if (!s_fsOk) return;
  File f = LittleFS.open(cfg::kRouteCacheFile, "w");
  if (!f) return;
  uint32_t magic = kFileMagic;
  f.write((uint8_t*)&magic, sizeof(magic));
  s_fileRecords = 0;
  for (uint16_t i = 0; i < s_cacheCount; i++) {
    if (!s_cache[i].found) continue;  // only persist positives
    f.write((uint8_t*)&s_cache[i], sizeof(CacheEntry));
    s_fileRecords++;
  }
  f.close();
  Serial.printf("[route] cache compacted: %lu records\n",
                (unsigned long)s_fileRecords);
}

void persistAppend(const CacheEntry& e) {
  if (!s_fsOk) return;
  if (s_fileRecords >= cfg::kRouteCacheMax * 2u) {
    persistRewrite();
    return;
  }
  File f = LittleFS.open(cfg::kRouteCacheFile, "a");
  if (!f) return;
  if (f.size() == 0) {
    uint32_t magic = kFileMagic;
    f.write((uint8_t*)&magic, sizeof(magic));
  }
  f.write((const uint8_t*)&e, sizeof(CacheEntry));
  s_fileRecords++;
  f.close();
}

}  // namespace

// Persistent TLS connection to adsbdb: the handshake (~1-2s) dominated each
// lookup and every drop used to permanently mark the plane "no route".
WiFiClientSecure* s_client = nullptr;
HTTPClient* s_http = nullptr;
uint32_t s_backoffUntil = 0;

void releaseConnection() {
  // Free the ~45KB of internal RAM the TLS session pins. Keep-alive only
  // pays while the queue is busy; holding it at idle starved the rest of
  // the system (observed: largest free block pinned at ~45KB -> freezes).
  if (s_http) s_http->end();
  if (s_client) s_client->stop();
}

bool airportCoords(const char* iata, float* lat, float* lon) {
  if (!iata[0]) return false;
  // Flash DB is sorted large->small, so the first match is the major field.
  for (int i = 0; i < kAirportCount; i++) {
    if (strcmp(kAirports[i].code, iata) == 0) {
      *lat = kAirports[i].lat;
      *lon = kAirports[i].lon;
      return true;
    }
  }
  return false;
}

bool aeroBudgetOk() {
  uint16_t m = todayEpochDay() / 30;
  if (m && m != s_aeroMonth) {
    s_aeroMonth = m;
    s_aeroCount = 0;
    aeroSaveCounters();
  }
  return s_aeroCount < cfg::kAeroMonthlyCap;
}

bool airlabsBudgetOk() {
  uint16_t m = todayEpochDay() / 30;
  if (m && m != s_airlabsMonth) {
    s_airlabsMonth = m;
    s_airlabsCount = 0;
    airlabsSaveCounters();
  }
  return s_airlabsCount < cfg::kAirlabsMonthlyCap;
}

// Query AeroAPI for the callsign's filed flights and pick the active leg.
bool tryAeroApi(const char* cs, RouteInfo* out) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 50 * 1024)
    return false;

  char url[128];
  snprintf(url, sizeof(url), cfg::kAeroApi, cs);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  if (!http.begin(client, url)) return false;
  http.addHeader("x-apikey", s_aeroKey);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[route] aeroapi %s HTTP %d\n", cs, code);
    http.end();
    return false;
  }
  s_aeroCount++;  // billed on successful result sets only
  aeroSaveCounters();

  JsonDocument filter;
  JsonObject ff = filter["flights"].add<JsonObject>();
  ff["origin"]["code_iata"] = true;
  ff["destination"]["code_iata"] = true;
  ff["actual_off"] = true;
  ff["actual_on"] = true;
  ff["cancelled"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  // Prefer the airborne leg (departed, not yet arrived); else first listed.
  JsonObject pick;
  for (JsonObject fl : doc["flights"].as<JsonArray>()) {
    if (fl["cancelled"] | false) continue;
    if (pick.isNull()) pick = fl;
    const char* off = fl["actual_off"] | "";
    const char* on = fl["actual_on"] | "";
    if (off[0] && !on[0]) {
      pick = fl;
      break;
    }
  }
  if (pick.isNull()) return false;

  memset(out, 0, sizeof(*out));
  strlcpy(out->origIata, pick["origin"]["code_iata"] | "?",
          sizeof(out->origIata));
  strlcpy(out->destIata, pick["destination"]["code_iata"] | "?",
          sizeof(out->destIata));
  if (out->origIata[0] == '?' || out->destIata[0] == '?') return false;
  airportCoords(out->origIata, &out->origLat, &out->origLon);
  airportCoords(out->destIata, &out->destLat, &out->destLon);
  return true;
}

// Query AirLabs (schedule-data source, like adsbdb but a maintained paid
// dataset) via flight_icao — no IATA conversion needed for the callsign.
bool tryAirlabsApi(const char* cs, RouteInfo* out) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 50 * 1024)
    return false;

  char url[160];
  snprintf(url, sizeof(url), cfg::kAirlabsApi, cs, s_airlabsKey);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[route] airlabs %s HTTP %d\n", cs, code);
    http.end();
    return false;
  }

  JsonDocument filter;
  JsonObject rf = filter["response"].add<JsonObject>();
  rf["dep_iata"] = true;
  rf["arr_iata"] = true;
  rf["airline_iata"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  // A wrong/expired key returns a JSON {"error":...} body, not an HTTP
  // error — treat any "error" object as a failed call, uncounted.
  if (!doc["error"].isNull()) {
    Serial.printf("[route] airlabs %s: %s\n", cs,
                  (const char*)(doc["error"]["message"] | "error"));
    return false;
  }
  s_airlabsCount++;  // only count calls that actually returned data
  airlabsSaveCounters();

  JsonObject pick = doc["response"][0];
  if (pick.isNull()) return false;

  memset(out, 0, sizeof(*out));
  strlcpy(out->origIata, pick["dep_iata"] | "?", sizeof(out->origIata));
  strlcpy(out->destIata, pick["arr_iata"] | "?", sizeof(out->destIata));
  if (out->origIata[0] == '?' || out->destIata[0] == '?') return false;
  strlcpy(out->airlineIata, pick["airline_iata"] | "", sizeof(out->airlineIata));
  airportCoords(out->origIata, &out->origLat, &out->origLon);
  airportCoords(out->destIata, &out->destLat, &out->destLon);
  return true;
}

// After the free DB comes up empty/wrong: try enhanced sources in order
// (AeroAPI's filed plans first — best for multi-leg through-flights — then
// AirLabs). Either, both, or neither may be configured.
bool tryEnhancedFallback(const char* cs) {
  RouteInfo info;
  if (s_aeroKey[0] && aeroBudgetOk() && tryAeroApi(cs, &info)) {
    CacheEntry* e = cachePut(cs, true, &info, todayEpochDay());
    if (e) {
      e->srcKind = kSrcAero;
      persistAppend(*e);
    }
    gAircraft.setRouteFound(cs, info);
    Serial.printf("[route] %s: %s -> %s (AeroAPI filed plan)\n", cs,
                  info.origIata, info.destIata);
    return true;
  }
  if (s_airlabsKey[0] && airlabsBudgetOk() && tryAirlabsApi(cs, &info)) {
    CacheEntry* e = cachePut(cs, true, &info, todayEpochDay());
    if (e) {
      e->srcKind = kSrcAirlabs;
      persistAppend(*e);
    }
    gAircraft.setRouteFound(cs, info);
    Serial.printf("[route] %s: %s -> %s (AirLabs)\n", cs, info.origIata,
                  info.destIata);
    return true;
  }
  return false;
}

void transientFailure(const char* cs, const char* why, int code) {
  Serial.printf("[route] %s transient failure (%s %d) — will retry\n", cs, why,
                code);
  gAircraft.setRouteUnknown(cs);  // stays eligible for retry
  s_backoffUntil = millis() + cfg::kRouteBackoffMs;
  releaseConnection();
}

void setAeroKey(const char* key) {
  strlcpy(s_aeroKey, key ? key : "", sizeof(s_aeroKey));
  Preferences p;
  p.begin("keys", false);
  p.putString("aero", s_aeroKey);
  p.end();
}

bool aeroActive() { return s_aeroKey[0] != 0; }
uint32_t aeroUsedThisMonth() { return s_aeroCount; }

void setAirlabsKey(const char* key) {
  strlcpy(s_airlabsKey, key ? key : "", sizeof(s_airlabsKey));
  Preferences p;
  p.begin("keys", false);
  p.putString("airlabs", s_airlabsKey);
  p.end();
}
bool airlabsActive() { return s_airlabsKey[0] != 0; }
uint32_t airlabsUsedThisMonth() { return s_airlabsCount; }

void loadPersist() {
  {
    Preferences p;
    p.begin("keys", true);
    strlcpy(s_aeroKey, p.getString("aero", "").c_str(), sizeof(s_aeroKey));
    s_aeroMonth = p.getUShort("aeroM", 0);
    s_aeroCount = p.getUInt("aeroC", 0);
    strlcpy(s_airlabsKey, p.getString("airlabs", "").c_str(),
            sizeof(s_airlabsKey));
    s_airlabsMonth = p.getUShort("alM", 0);
    s_airlabsCount = p.getUInt("alC", 0);
    p.end();
    if (s_aeroKey[0]) Serial.println("[route] AeroAPI key configured");
    if (s_airlabsKey[0]) Serial.println("[route] AirLabs key configured");
  }
  s_fsOk = LittleFS.begin(true /* format on first use */);
  if (!s_fsOk) {
    Serial.println("[route] LittleFS mount failed — cache is RAM-only");
    return;
  }
  File f = LittleFS.open(cfg::kRouteCacheFile, "r");
  if (!f) return;
  uint32_t magic = 0;
  f.read((uint8_t*)&magic, sizeof(magic));
  if (magic != kFileMagic) {  // format change — start over
    f.close();
    LittleFS.remove(cfg::kRouteCacheFile);
    return;
  }
  CacheEntry e;
  uint32_t n = 0;
  while (f.read((uint8_t*)&e, sizeof(e)) == sizeof(e)) {
    if (e.found && e.callsign[0]) {
      e.callsign[sizeof(e.callsign) - 1] = '\0';
      if (cachePut(e.callsign, true, &e.info, e.epochDay)) n++;
    }
  }
  f.close();
  s_fileRecords = n;
  Serial.printf("[route] loaded %lu cached routes from flash\n",
                (unsigned long)n);
}

void serviceOne() {
  if ((int32_t)(millis() - s_backoffUntil) < 0) return;

  char cs[10];
  if (!gAircraft.nextRouteCandidate(cs)) {
    releaseConnection();  // queue drained — give the RAM back
    return;
  }

  // Cache hit — no network needed (unless the entry aged out).
  if (CacheEntry* e = cacheFind(cs)) {
    uint16_t today = todayEpochDay();
    uint16_t ttl = e->srcKind == kSrcAero      ? cfg::kAeroTtlDays
                   : e->srcKind == kSrcAirlabs ? cfg::kAirlabsTtlDays
                                               : cfg::kRouteTtlDays;
    bool stale = e->found && e->epochDay && today &&
                 (uint16_t)(today - e->epochDay) > ttl;
    if (!stale) {
      if (e->found)
        gAircraft.setRouteFound(cs, e->info);
      else
        gAircraft.setRouteNone(cs);
      return;
    }
    e->callsign[0] = '\0';  // expired — fall through and refetch
  }

  // Don't open a fresh TLS session into a tight heap — defer instead.
  if ((!s_client || !s_client->connected()) &&
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 50 * 1024) {
    s_backoffUntil = millis() + 5000;
    return;
  }

  gAircraft.setRoutePending(cs);

  if (!s_client) {
    s_client = new WiFiClientSecure();
    s_client->setInsecure();
    s_http = new HTTPClient();
    s_http->setUserAgent(cfg::kUserAgent);
    s_http->setTimeout(cfg::kHttpTimeoutMs);
    s_http->setReuse(true);  // keep-alive: one handshake, many lookups
  }

  char url[96];
  snprintf(url, sizeof(url), "%s%s", cfg::kRouteApi, cs);
  if (!s_http->begin(*s_client, url)) {
    transientFailure(cs, "begin", 0);
    return;
  }
  int code = s_http->GET();
  if (code == 404) {
    // Not in the community DB. AeroAPI (if configured) may still have the
    // filed plan; otherwise cache the miss (RAM only — re-checks on reboot).
    s_http->end();  // with setReuse, keeps the socket for the next lookup
    if (tryEnhancedFallback(cs)) return;
    cachePut(cs, false, nullptr, todayEpochDay());
    gAircraft.setRouteNone(cs);
    return;
  }
  if (code != 200) {
    transientFailure(cs, "http", code);
    return;
  }

  String body = s_http->getString();
  s_http->end();  // with setReuse, keeps the socket for the next lookup
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    transientFailure(cs, "json", 0);
    return;
  }

  JsonObject fr = doc["response"]["flightroute"];
  if (fr.isNull()) {
    if (tryEnhancedFallback(cs)) return;
    cachePut(cs, false, nullptr, todayEpochDay());
    gAircraft.setRouteNone(cs);
    return;
  }

  RouteInfo info = {};
  strlcpy(info.origIata, fr["origin"]["iata_code"] | "?", sizeof(info.origIata));
  strlcpy(info.destIata, fr["destination"]["iata_code"] | "?",
          sizeof(info.destIata));
  strlcpy(info.airline, fr["airline"]["name"] | "", sizeof(info.airline));
  strlcpy(info.airlineIata, fr["airline"]["iata"] | "",
          sizeof(info.airlineIata));
  info.origLat = fr["origin"]["latitude"] | 0.0f;
  info.origLon = fr["origin"]["longitude"] | 0.0f;
  info.destLat = fr["destination"]["latitude"] | 0.0f;
  info.destLon = fr["destination"]["longitude"] | 0.0f;

  if (!routePlausible(cs, info)) {
    // Stale DB entry. The filed plan (AeroAPI) knows the real leg — this is
    // exactly where it shines (multi-leg Southwest through-flights).
    if (tryEnhancedFallback(cs)) return;
    cachePut(cs, false, nullptr, todayEpochDay());
    gAircraft.setRouteNone(cs);
    return;
  }

  CacheEntry* saved = cachePut(cs, true, &info, todayEpochDay());
  if (saved) persistAppend(*saved);
  gAircraft.setRouteFound(cs, info);
  Serial.printf("[route] %s: %s -> %s (%s)\n", cs, info.origIata,
                info.destIata, info.airline);
}

}  // namespace routes
