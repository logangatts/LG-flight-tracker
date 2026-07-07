#include "geolocate.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "app_state.h"
#include "config.h"

namespace geolocate {

namespace {

constexpr const char* kNvsNs = "geo";

void store(float lat, float lon, LocSource src, float accM) {
  gApp.centerLat = lat;
  gApp.centerLon = lon;
  gApp.locSource = src;
  gApp.locAccuracyM = accM;

  Preferences p;
  p.begin(kNvsNs, false);
  p.putFloat("lat", lat);
  p.putFloat("lon", lon);
  p.putUChar("src", (uint8_t)src);
  p.end();
  Serial.printf("[geo] location set %.5f, %.5f (%s, acc %.0fm)\n", lat, lon,
                locSourceName(src), accM);
}

// POST scanned BSSIDs to beaconDB (Ichnaea/MLS-compatible, free, keyless).
bool tryWifiGeo() {
  Serial.println("[geo] scanning WiFi for beaconDB...");
  int16_t n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (n <= 1) {
    // A single AP can't multilaterate well, but beaconDB still tries; only
    // bail if we truly saw nothing.
    if (n <= 0) {
      Serial.println("[geo] scan found no networks");
      return false;
    }
  }

  JsonDocument doc;
  JsonArray aps = doc["wifiAccessPoints"].to<JsonArray>();
  int used = 0;
  for (int i = 0; i < n && used < 20; i++) {
    JsonObject ap = aps.add<JsonObject>();
    ap["macAddress"] = WiFi.BSSIDstr(i);
    ap["signalStrength"] = WiFi.RSSI(i);
    used++;
  }
  WiFi.scanDelete();

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();  // v1: no cert pinning
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  if (!http.begin(client, cfg::kGeoWifiApi)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[geo] beaconDB HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, http.getString());
  http.end();
  if (err) return false;
  if (!resp["location"]["lat"].is<float>()) return false;

  float lat = resp["location"]["lat"];
  float lon = resp["location"]["lng"];
  float acc = resp["accuracy"] | -1.0f;
  store(lat, lon, LocSource::WifiGeo, acc);
  return true;
}

bool tryIpGeo() {
  WiFiClient client;  // ip-api free tier is HTTP only
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  if (!http.begin(client, cfg::kGeoIpApi)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  JsonDocument resp;
  DeserializationError err = deserializeJson(resp, http.getString());
  http.end();
  if (err || strcmp(resp["status"] | "", "success") != 0) return false;

  store(resp["lat"], resp["lon"], LocSource::IpGeo, 15000.0f);
  return true;
}

}  // namespace

bool loadStored() {
  Preferences p;
  p.begin(kNvsNs, true);
  if (!p.isKey("lat")) {
    p.end();
    return false;
  }
  float lat = p.getFloat("lat");
  float lon = p.getFloat("lon");
  LocSource src = (LocSource)p.getUChar("src", (uint8_t)LocSource::Cached);
  p.end();

  gApp.centerLat = lat;
  gApp.centerLon = lon;
  gApp.locSource = (src == LocSource::Manual) ? LocSource::Manual
                                              : LocSource::Cached;
  Serial.printf("[geo] loaded stored location %.5f, %.5f (%s)\n", lat, lon,
                locSourceName(gApp.locSource));
  return true;
}

bool saveManual(const char* text) {
  float lat, lon;
  if (sscanf(text, "%f , %f", &lat, &lon) != 2) return false;
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
  store(lat, lon, LocSource::Manual, 0);
  return true;
}

bool run() {
  if (gApp.locSource == LocSource::Manual) return true;  // user pinned it
  if (tryWifiGeo()) return true;
  if (tryIpGeo()) return true;
  return gApp.locSource != LocSource::Unset;  // keep cached if online failed
}

void reset() {
  Preferences p;
  p.begin(kNvsNs, false);
  p.clear();
  p.end();
  gApp.locSource = LocSource::Unset;
}

}  // namespace geolocate
