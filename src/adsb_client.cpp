#include "adsb_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "aircraft_store.h"
#include "app_state.h"
#include "config.h"

namespace adsb {

namespace {

// Keep the (potentially large) parsed traffic document out of internal RAM.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
  }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    return q ? q : realloc(p, n);
  }
};
SpiRamAllocator s_psAlloc;

uint8_t s_urlIdx = 0;  // rotates through cfg::kAdsbUrlTemplates on failure

// Read the full HTTP body into a PSRAM buffer. Byte-wise TLS stream parsing
// is too slow (~10 KB/s) — busy airspace bodies are 40 KB+ and the server
// closes the connection mid-parse. Bulk 4 KB reads run at full WiFi speed.
// Returns bytes read, 0 on failure. Caller frees *out.
size_t readBody(HTTPClient& http, char** out) {
  int declared = http.getSize();  // Content-Length, or -1
  size_t cap = (declared > 0) ? (size_t)declared + 1 : 96 * 1024;
  char* buf = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!buf) return 0;

  NetworkClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t lastData = millis();
  while (http.connected() || stream->available()) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (declared > 0 && got >= (size_t)declared) break;
      if (millis() - lastData > cfg::kHttpTimeoutMs) break;
      delay(5);
      continue;
    }
    if (got + avail >= cap) {
      cap = cap * 2;
      char* nb = (char*)heap_caps_realloc(buf, cap, MALLOC_CAP_SPIRAM);
      if (!nb) break;
      buf = nb;
    }
    int r = stream->readBytes(buf + got, min(avail, (size_t)4096));
    if (r <= 0) break;
    got += r;
    lastData = millis();
    if (declared > 0 && got >= (size_t)declared) break;
  }
  buf[got] = '\0';
  *out = buf;
  return got;
}

// Pick an icon silhouette from the ADS-B emitter category (A1 light ...
// A5 heavy, A7 rotorcraft), falling back to ICAO type-code heuristics.
IconClass classify(const char* category, const char* type) {
  if (category[0] == 'A') {
    switch (category[1]) {
      case '1': return IconClass::Prop;   // light (< 15.5k lb): GA singles
      case '2': return IconClass::Prop;   // small: twins/turboprops
      case '3': return IconClass::Jet;    // large: narrowbody
      case '4': return IconClass::Jet;    // high-vortex (757)
      case '5': return IconClass::Heavy;  // heavy: widebody
      case '7': return IconClass::Heli;
    }
  }
  // No category: guess from the type code.
  if (type[0]) {
    static const char* kHeavyPrefixes[] = {"B74", "B77", "B78", "B76", "A33",
                                           "A34", "A35", "A38", "MD11"};
    for (const char* p : kHeavyPrefixes) {
      if (strncmp(type, p, strlen(p)) == 0) return IconClass::Heavy;
    }
    static const char* kPropPrefixes[] = {"C1",  "C2",  "C3",  "P2",  "PA",
                                          "SR2", "DA4", "DA2", "BE",  "M20",
                                          "RV",  "DV2", "GLID"};
    for (const char* p : kPropPrefixes) {
      if (strncmp(type, p, strlen(p)) == 0) return IconClass::Prop;
    }
    static const char* kHeliPrefixes[] = {"R22", "R44", "R66", "EC",  "AS3",
                                          "AS5", "B06", "B40", "B42", "S76",
                                          "A10", "AW", "H6", "UH"};
    for (const char* p : kHeliPrefixes) {
      if (strncmp(type, p, strlen(p)) == 0) return IconClass::Heli;
    }
  }
  return IconClass::Jet;
}

// Trim trailing spaces (API pads callsigns) into out.
void trimCopy(char* out, size_t outLen, const char* in) {
  size_t n = strlen(in);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
  size_t c = min(n, outLen - 1);
  memcpy(out, in, c);
  out[c] = '\0';
}

}  // namespace

bool poll(uint32_t* retryAfterMs) {
  *retryAfterMs = 0;
  float lat = gApp.centerLat.load();
  float lon = gApp.centerLon.load();
  // Always fetch at the WIDEST preset radius (+margin), independent of the
  // current zoom. Fetching only the zoomed-in circle starved the store:
  // zooming back out showed near-empty skies until several polls passed.
  int radiusNm =
      (int)min(cfg::kZoomRadiiMi[0] * 0.869f * 1.3f, 250.0f);

  char url[160];
  snprintf(url, sizeof(url), cfg::kAdsbUrlTemplates[s_urlIdx], lat, lon,
           radiusNm);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  http.useHTTP10(true);  // no chunked encoding -> stream parse
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  gApp.adsbLastCode = code;
  if (code == 429) {
    Serial.printf("[adsb] rate limited by host %u — rotating\n", s_urlIdx);
    http.end();
    // Rotate to the other aggregator AND back off — waiting on the same
    // throttled host just repeats the 429.
    s_urlIdx = (s_urlIdx + 1) % cfg::kAdsbUrlCount;
    *retryAfterMs = cfg::kAdsbBackoffMs;
    return false;
  }
  if (code != 200) {
    Serial.printf("[adsb] HTTP %d (%s)\n", code, url);
    http.end();
    // Try the next aggregator next cycle (same JSON shape).
    s_urlIdx = (s_urlIdx + 1) % cfg::kAdsbUrlCount;
    return false;
  }

  // Filter keeps memory bounded: ~10 of ~45 fields per aircraft.
  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  f["hex"] = true;
  f["flight"] = true;
  f["t"] = true;
  f["lat"] = true;
  f["lon"] = true;
  f["alt_baro"] = true;
  f["gs"] = true;
  f["track"] = true;
  f["baro_rate"] = true;
  f["category"] = true;
  f["squawk"] = true;
  filter["now"] = true;  // wall clock, used for route-cache aging

  char* body = nullptr;
  size_t bodyLen = readBody(http, &body);
  http.end();
  if (bodyLen == 0) {
    if (body) free(body);
    Serial.println("[adsb] empty body");
    s_urlIdx = (s_urlIdx + 1) % cfg::kAdsbUrlCount;
    return false;
  }

  JsonDocument doc(&s_psAlloc);
  // const char* forces ArduinoJson to copy strings into the doc (a mutable
  // char* would zero-copy — dangling pointers once body is freed).
  DeserializationError err =
      deserializeJson(doc, (const char*)body, bodyLen,
                      DeserializationOption::Filter(filter));
  free(body);
  if (err) {
    Serial.printf("[adsb] json error: %s (%u bytes)\n", err.c_str(),
                  (unsigned)bodyLen);
    return false;
  }

  // API timestamp: seconds or milliseconds depending on aggregator.
  double nowTs = doc["now"] | 0.0;
  if (nowTs > 1e12) nowTs /= 1000.0;
  if (nowTs > 1e9) gApp.epochSec = (uint32_t)nowTs;

  char csFilter[5];
  int csFilterLen = gApp.getCallsignFilter(csFilter);

  gAircraft.beginUpdate();
  int n = 0;
  for (JsonObject a : doc["ac"].as<JsonArray>()) {
    const char* hex = a["hex"] | "";
    if (!hex[0]) continue;
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;

    char cs[10];
    trimCopy(cs, sizeof(cs), a["flight"] | "");

    // Airline-filter mode: only track matching callsigns.
    if (csFilterLen && strncmp(cs, csFilter, csFilterLen) != 0) continue;

    // alt_baro is a number, or the string "ground".
    int32_t altFt = -1;
    if (a["alt_baro"].is<int>()) altFt = a["alt_baro"].as<int>();

    const char* type = a["t"] | "";
    gAircraft.upsert(hex, cs, type, a["squawk"] | "",
                     classify(a["category"] | "", type), a["lat"], a["lon"],
                     a["track"] | 0.0f, a["gs"] | 0.0f, altFt,
                     a["baro_rate"] | 0.0f, lat, lon);
    n++;
  }
  gAircraft.endUpdate(true);
  Serial.printf("[adsb] %d aircraft within %d nm (heap %u, largest %u)\n", n,
                radiusNm,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return true;
}

}  // namespace adsb
