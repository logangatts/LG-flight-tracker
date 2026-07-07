#include "app_state.h"

AppState gApp;

void fmtAlt(int32_t altFt, char* out, size_t n) {
  if (gApp.altUnit.load() == 1) {
    long m = lroundf(altFt * 0.3048f);
    if (m >= 1000) {
      snprintf(out, n, "%.1fk m", m / 1000.0f);
    } else {
      snprintf(out, n, "%ld m", m);
    }
  } else {
    if (altFt >= 10000) {
      snprintf(out, n, "%ldk ft", (long)(altFt / 1000));
    } else {
      snprintf(out, n, "%ld ft", (long)altFt);
    }
  }
}

void fmtSpeed(float gsKt, char* out, size_t n) {
  switch (gApp.spdUnit.load()) {
    case 1:
      snprintf(out, n, "%.0f mph", gsKt * 1.15078f);
      break;
    case 2:
      snprintf(out, n, "%.0f km/h", gsKt * 1.852f);
      break;
    default:
      snprintf(out, n, "%.0f kt", gsKt);
  }
}

void devicePassword(char* out) {
  // Confusable-free alphabet (no 0/O, 1/I/L) — this gets read off a 1.2"
  // screen and typed on a phone.
  static const char kAlpha[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
  uint64_t x = ESP.getEfuseMac() ^ 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 8; i++) {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 29;
    out[i] = kAlpha[x % (sizeof(kAlpha) - 1)];
  }
  out[8] = '\0';
}

const char* locSourceName(LocSource s) {
  switch (s) {
    case LocSource::Cached:  return "cached";
    case LocSource::Manual:  return "manual";
    case LocSource::WifiGeo: return "wifi";
    case LocSource::IpGeo:   return "ip";
    default:                 return "none";
  }
}
