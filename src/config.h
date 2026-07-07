// Flight Radar — global configuration
#pragma once

#include <stdint.h>

namespace cfg {

// ---------- Product ----------
inline constexpr const char* kDeviceName   = "FlightRadar";
inline constexpr const char* kSetupApName  = "FlightRadar-Setup";
inline constexpr const char* kUserAgent    = "FlightRadar/0.1 (personal desk gadget; contact: logangatts@gmail.com)";
inline constexpr const char* kFwVersion    = "0.1.0";

// ---------- Display ----------
inline constexpr int16_t kScreenW = 390;
inline constexpr int16_t kScreenH = 390;
inline constexpr int16_t kScreenR = 195;   // radius of the round panel

// ---------- Radar / map ----------
// Zoom presets are exact screen-edge radii in statute miles. Each preset
// renders from the web-mercator tile level whose native radius is the
// smallest one >= the target, resampled by k = native/target (1 <= k < 2),
// so the ring is exactly the labeled distance at any latitude.
// 25/15/7/3: all within ~1.3x of a native tile level (15 and 7 nearly
// exact). Values just above a native radius (8, 4, 20...) would resample
// ~1.9x and look muddy. 50 mi proved too broad to read on a 1.2" panel.
inline constexpr uint8_t  kZoomCount = 4;
inline constexpr float    kZoomRadiiMi[kZoomCount] = {25, 15, 7, 3};  // idx 0 = widest
inline constexpr uint8_t  kZoomDefaultIdx = 1;     // 15 mi — the arrival-watching view
inline constexpr uint32_t kFrameMs = 33;           // UI tick

inline constexpr const char* kMapTileUrl =
    "https://basemaps.cartocdn.com/dark_nolabels/%u/%lu/%lu.png";
inline constexpr bool kMapDefaultOn = true;

// ---------- Aircraft store ----------
inline constexpr uint8_t  kMaxAircraft = 96;  // store lives in PSRAM; DFW at 40nm easily tops 80
inline constexpr uint8_t  kTrailLen = 40;             // points per aircraft
inline constexpr float    kTrailMinMoveNm = 0.02f;    // append trail point after ~37 m
inline constexpr uint32_t kTrailFadeMs = 30000;       // segment fully faded at 30 s
inline constexpr uint32_t kAircraftTtlMs = 30000;     // drop if unseen for 30 s

// ---------- Networking ----------
inline constexpr uint32_t kAdsbPollMs = 3000;        // airplanes.live asks >= 1 s
inline constexpr uint32_t kAdsbBackoffMs = 15000;    // after 429 / failure streak
inline constexpr uint32_t kRouteLookupSpacingMs = 1500;  // keep-alive makes this cheap
inline constexpr uint32_t kRouteBackoffMs = 20000;       // after transient failures
inline constexpr uint16_t kRouteCacheMax = 1024;     // callsign->route cache (PSRAM)
inline constexpr const char* kRouteCacheFile = "/routes.bin";  // LittleFS persist
inline constexpr uint16_t kRouteTtlDays = 45;        // refetch after schedule changes
inline constexpr uint32_t kHttpTimeoutMs = 8000;

// URL templates (lat, lon, radius_nm). All return the same ADSBx-v2 JSON
// shape. airplanes.live's documented /v2/point endpoint 404s as of 2026-07,
// so adsb.lol is primary; adsb.fi is personal-use-only (fine for dev builds).
inline constexpr const char* kAdsbUrlTemplates[] = {
    "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
    "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d",
};
inline constexpr uint8_t kAdsbUrlCount = 2;
inline constexpr const char* kRouteApi = "https://api.adsbdb.com/v0/callsign/";
// Optional user-supplied FlightAware AeroAPI key (web settings): real filed
// flight plans for whatever the community DB misses.
inline constexpr const char* kAeroApi =
    "https://aeroapi.flightaware.com/aeroapi/flights/%s?max_pages=1";
inline constexpr uint32_t kAeroMonthlyCap = 900;  // stay inside $5 free credit
inline constexpr uint16_t kAeroTtlDays = 3;       // filed legs change daily

// Optional user-supplied AirLabs key: an alternative schedule-data source
// (like adsbdb, but a maintained paid dataset) — tried when AeroAPI is
// absent or also misses. Uses flight_icao so no IATA conversion is needed.
inline constexpr const char* kAirlabsApi =
    "https://airlabs.co/api/v9/routes?flight_icao=%s&api_key=%s";
inline constexpr uint32_t kAirlabsMonthlyCap = 900;  // conservative — verify your plan's limit
inline constexpr uint16_t kAirlabsTtlDays = 14;      // schedule data drifts slower than filed plans
inline constexpr const char* kAircraftApi = "https://api.adsbdb.com/v0/aircraft/";
// Airline logos, vector-rendered server-side at the requested pixel size.
inline constexpr const char* kLogoApi = "https://pics.avs.io/220/80/%s.png";
inline constexpr const char* kGeoWifiApi = "https://api.beacondb.net/v1/geolocate";
inline constexpr const char* kGeoIpApi = "http://ip-api.com/json/?fields=status,lat,lon,city";

// ---------- Device identity ----------
// mDNS hostname: settings page lives at http://<hostname>.local
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "flightradar"
#endif
inline constexpr const char* kHostname = DEVICE_HOSTNAME;

// ---------- Airline filter ----------
// Callsign-prefix filter ("SWA" = Southwest-only device). Runtime-settable
// via the web page; flavor builds (env:tencoder-pro-swa) bake in a default.
#ifndef DEFAULT_CALLSIGN_FILTER
#define DEFAULT_CALLSIGN_FILTER ""
#endif

// ---------- Location ----------
inline constexpr float kFallbackLat = 32.8998f;   // DFW as a harmless default
inline constexpr float kFallbackLon = -97.0403f;

}  // namespace cfg
