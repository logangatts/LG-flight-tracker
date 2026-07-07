// Small cross-task app state (atomics; no locking needed).
#pragma once

#include <Arduino.h>
#include <atomic>

#include "config.h"

enum class LocSource : uint8_t { Unset, Cached, Manual, WifiGeo, IpGeo };

enum class NetPhase : uint8_t { Boot, Portal, Connecting, Locating, Running };

// Plane color palette (web-selectable). Red is offered but not default:
// it competes with the emergency highlight.
struct PlaneColor {
  uint32_t hex;
  const char* name;
};
#ifdef SWA_THEME
// Southwest corporate palette (Heart livery): Bold Blue, Warm Red,
// Sunrise Yellow, Summit Silver.
inline constexpr PlaneColor kPlaneColors[] = {
    {0x304CB2, "bold blue"},      {0xD5152E, "warm red"},
    {0xFFBF27, "sunrise yellow"}, {0xCCCCCC, "summit silver"},
    {0xFFFFFF, "white"},
};
inline constexpr uint8_t kPlaneColorDefaultComm = 0;  // bold blue
inline constexpr uint8_t kPlaneColorDefaultPriv = 2;  // sunrise yellow
#else
inline constexpr PlaneColor kPlaneColors[] = {
    {0x4DA6FF, "blue"}, {0xFF5252, "red"},  {0xFFA64D, "orange"},
    {0xE8F6FF, "white"}, {0xFF7AD9, "pink"}, {0x3CFF81, "green"},
};
inline constexpr uint8_t kPlaneColorDefaultComm = 0;
inline constexpr uint8_t kPlaneColorDefaultPriv = 5;
#endif
inline constexpr uint8_t kPlaneColorCount =
    sizeof(kPlaneColors) / sizeof(kPlaneColors[0]);

struct AppState {
  // Radar center (written by geolocation / portal, read everywhere).
  std::atomic<float> centerLat{cfg::kFallbackLat};
  std::atomic<float> centerLon{cfg::kFallbackLon};
  std::atomic<LocSource> locSource{LocSource::Unset};
  std::atomic<float> locAccuracyM{-1};

  // UI
  std::atomic<uint8_t> zoomIdx{cfg::kZoomDefaultIdx};
  std::atomic<bool> mapEnabled{cfg::kMapDefaultOn};
  std::atomic<bool> wifiUp{false};
  std::atomic<NetPhase> netPhase{NetPhase::Boot};
  std::atomic<bool> relocateReq{false};  // long-press: re-run geolocation
  std::atomic<bool> pollNowReq{false};   // fetch traffic asap
  std::atomic<int> adsbLastCode{0};      // last traffic-API HTTP result
  std::atomic<uint32_t> epochSec{0};     // wall clock from the traffic API

  // Web settings page -> UI thread requests (display bus is UI-owned).
  std::atomic<int> briReq{-1};       // 0..3 = apply brightness level
  std::atomic<uint8_t> briIdx{3};    // current level, mirrored for the web UI
  std::atomic<bool> tutReq{false};   // open the tutorial

  // Units + display preferences (settable on-device and via web).
  std::atomic<uint8_t> altUnit{0};       // 0 = ft, 1 = m
  std::atomic<uint8_t> spdUnit{0};       // 0 = kt, 1 = mph, 2 = km/h
  std::atomic<bool> airlineColors{true}; // distinct color for airliners
  std::atomic<uint8_t> colComm{kPlaneColorDefaultComm};
  std::atomic<uint8_t> colPriv{kPlaneColorDefaultPriv};
  std::atomic<bool> textLarge{false};    // bump reading text one font step

  // Callsign-prefix filter, packed into a u32 (max 4 chars) so cross-task
  // access stays atomic. 0 = no filter.
  std::atomic<uint32_t> callsignFilter{0};

  void setCallsignFilter(const char* s) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && s && s[i]; i++) {
      v |= ((uint32_t)(uint8_t)toupper((unsigned char)s[i])) << (i * 8);
    }
    callsignFilter = v;
  }
  // Unpacks into out[5]; returns length (0 = no filter).
  int getCallsignFilter(char* out) const {
    uint32_t v = callsignFilter.load();
    int n = 0;
    for (; n < 4; n++) {
      char c = (char)((v >> (n * 8)) & 0xFF);
      if (!c) break;
      out[n] = c;
    }
    out[n] = '\0';
    return n;
  }

  // Native (unscaled) screen-edge radius in statute miles at tile zoom z.
  float nativeRadiusMi(uint8_t z) const {
    float mpp = 156543.03f * cosf(centerLat.load() * 0.0174533f) /
                (float)(1UL << z);
    return cfg::kScreenR * mpp / 1609.34f;
  }

  // Tile zoom for a preset: deepest level whose native radius still covers
  // the target (so k = native/target stays in [1, 2)).
  uint8_t zForIdx(uint8_t idx) const {
    float target = cfg::kZoomRadiiMi[idx];
    for (int z = 15; z >= 6; z--) {
      if (nativeRadiusMi((uint8_t)z) >= target) return (uint8_t)z;
    }
    return 6;
  }
  float kForIdx(uint8_t idx) const {
    return nativeRadiusMi(zForIdx(idx)) / cfg::kZoomRadiiMi[idx];
  }

  uint8_t zoomZ() const { return zForIdx(zoomIdx.load()); }
  float zoomK() const { return kForIdx(zoomIdx.load()); }

  // Displayed screen-edge radius: exactly the preset value.
  float radiusMi() const { return cfg::kZoomRadiiMi[zoomIdx.load()]; }
};

extern AppState gApp;

const char* locSourceName(LocSource s);

// Device-unique 8-char password (derived from the eFuse MAC — stable for
// the life of the device, never stored). Guards the web UI and the setup AP.
// out must hold >= 9 bytes.
void devicePassword(char* out);

// Format altitude/speed per the configured units (compact, unit included).
void fmtAlt(int32_t altFt, char* out, size_t n);
void fmtSpeed(float gsKt, char* out, size_t n);

