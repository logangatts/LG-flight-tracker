#include "photo_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFiClientSecure.h>
#include <atomic>

#include "config.h"

namespace photo {

namespace {

std::atomic<State> s_state{State::Idle};
Profile s_profile = {};
char s_reqHex[8] = {0};
char s_reqIata[4] = {0};
char s_logoIata[4] = {0};
std::atomic<bool> s_pending{false};
std::atomic<bool> s_logoPending{false};
std::atomic<bool> s_imgReady{false};
std::atomic<bool> s_infoReady{false};

PNG* s_png = nullptr;
uint16_t* s_imgBuf = nullptr;
int s_imgW = 0, s_imgH = 0;

int pngLineCb(PNGDRAW* draw) {
  static uint16_t line[512];
  if (draw->y >= s_imgH || draw->iWidth > 512) return 1;
  s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  memcpy(&s_imgBuf[draw->y * s_imgW], line,
         min(draw->iWidth, s_imgW) * sizeof(uint16_t));
  return 1;
}

// GET a URL, body into PSRAM. Returns length, 0 on failure.
size_t httpGet(const char* url, uint8_t** out, size_t maxLen) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);  // no chunked bodies
  if (!http.begin(client, url)) return 0;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[profile] HTTP %d (%s)\n", code, url);
    http.end();
    return 0;
  }
  int len = http.getSize();
  size_t cap = (len > 0) ? (size_t)len : 128 * 1024;
  if (cap > maxLen) {
    http.end();
    return 0;
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
  if (!buf) {
    http.end();
    return 0;
  }
  NetworkClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t lastData = millis();
  while (http.connected() || stream->available()) {
    size_t avail = stream->available();
    if (!avail) {
      if (len > 0 && got >= (size_t)len) break;
      if (millis() - lastData > cfg::kHttpTimeoutMs) break;
      delay(5);
      continue;
    }
    if (got + avail > cap) break;
    int r = stream->readBytes(buf + got, min(avail, (size_t)4096));
    if (r <= 0) break;
    got += r;
    lastData = millis();
    if (len > 0 && got >= (size_t)len) break;
  }
  http.end();
  buf[got] = '\0';
  *out = buf;
  return got;
}

void freeImage() {
  if (s_profile.img) {
    free(s_profile.img);
    s_profile.img = nullptr;
  }
}

// Airline logo from avs.io: vector-rendered server-side at the exact pixel
// size requested — always crisp, tiny download, plain baseline PNG.
bool fetchLogo(const char* iata) {
  char url[96];
  snprintf(url, sizeof(url), cfg::kLogoApi, iata);
  uint8_t* png = nullptr;
  size_t len = 0;
  // One retry: a single dropped TLS fetch shouldn't cost the logo.
  for (int attempt = 0; attempt < 2 && !len; attempt++) {
    len = httpGet(url, &png, 64 * 1024);
  }
  if (!len) return false;
  // Unknown carriers return a ~1.25KB generic placeholder — monogram looks
  // better, so treat that as "no logo". (Threshold sits just above the
  // measured placeholder; real logos at 220x80 start around 2KB.)
  if (len < 1400) {
    free(png);
    Serial.printf("[profile] no real logo for %s (placeholder)\n", iata);
    return false;
  }

  bool ok = false;
  if (s_png->openRAM(png, (int)len, pngLineCb) == PNG_SUCCESS) {
    s_imgW = s_png->getWidth();
    s_imgH = s_png->getHeight();
    if (s_imgW > 0 && s_imgH > 0 && s_imgW <= 300 && s_imgH <= 120) {
      s_imgBuf =
          (uint16_t*)heap_caps_malloc(s_imgW * s_imgH * 2, MALLOC_CAP_SPIRAM);
      if (s_imgBuf) {
        memset(s_imgBuf, 0xFF, s_imgW * s_imgH * 2);  // white card bg
        if (s_png->decode(nullptr, 0) == PNG_SUCCESS) {
          s_profile.img = s_imgBuf;
          s_profile.imgW = (uint16_t)s_imgW;
          s_profile.imgH = (uint16_t)s_imgH;
          s_imgReady = true;
          ok = true;
        } else {
          free(s_imgBuf);
        }
        s_imgBuf = nullptr;
      }
    }
    s_png->close();
  }
  free(png);
  if (!ok) Serial.printf("[profile] logo decode failed for %s\n", iata);
  return ok;
}

void fetchProfile(const char* hex, const char* iata) {
  memset(&s_profile, 0, sizeof(s_profile));
  strlcpy(s_profile.hex, hex, sizeof(s_profile.hex));

  // 1) Logo first — the visual lands fast (single small fetch).
  if (iata[0]) fetchLogo(iata);

  // 2) Registration info from adsbdb.
  char url[96];
  snprintf(url, sizeof(url), "%s%s", cfg::kAircraftApi, hex);
  uint8_t* body = nullptr;
  size_t len = httpGet(url, &body, 64 * 1024);
  if (len) {
    JsonDocument doc;
    if (deserializeJson(doc, (const char*)body, len) ==
        DeserializationError::Ok) {
      JsonObject ac = doc["response"]["aircraft"];
      if (!ac.isNull()) {
        strlcpy(s_profile.registration, ac["registration"] | "",
                sizeof(s_profile.registration));
        const char* manu = ac["manufacturer"] | "";
        const char* type = ac["type"] | "";
        snprintf(s_profile.typeDesc, sizeof(s_profile.typeDesc), "%s %s", manu,
                 type);
        strlcpy(s_profile.owner, ac["registered_owner"] | "",
                sizeof(s_profile.owner));
        strlcpy(s_profile.country, ac["registered_owner_country_iso_name"] | "",
                sizeof(s_profile.country));
        s_infoReady = true;
      }
    }
  }
  if (body) free(body);

  Serial.printf("[profile] %s: reg=%s type=\"%s\" logo=%s\n", hex,
                s_profile.registration, s_profile.typeDesc,
                s_profile.img ? "yes" : "monogram");
}

}  // namespace

void request(const char* hex, const char* airlineIata) {
  cancel();
  strlcpy(s_reqHex, hex, sizeof(s_reqHex));
  strlcpy(s_reqIata, airlineIata ? airlineIata : "", sizeof(s_reqIata));
  s_state = State::Pending;
  s_pending = true;
}

void requestLogo(const char* airlineIata) {
  if (!airlineIata || !airlineIata[0] || s_imgReady.load()) return;
  strlcpy(s_logoIata, airlineIata, sizeof(s_logoIata));
  s_logoPending = true;
}

void cancel() {
  s_pending = false;
  s_logoPending = false;
  s_imgReady = false;
  s_infoReady = false;
  freeImage();
  s_state = State::Idle;
}

State state() { return s_state.load(); }
const Profile& profile() { return s_profile; }
bool imageReady() { return s_imgReady.load(); }
bool infoReady() { return s_infoReady.load(); }
bool pending() { return s_pending.load() || s_logoPending.load(); }

void service() {
  if (s_pending.exchange(false)) {
    if (!s_png) s_png = new PNG();
    fetchProfile(s_reqHex, s_reqIata);
    if (s_state.load() == State::Pending) {
      s_state = (s_infoReady.load() || s_imgReady.load()) ? State::Ready
                                                          : State::Failed;
    } else {
      freeImage();
    }
    return;
  }
  if (s_logoPending.exchange(false)) {
    if (!s_png) s_png = new PNG();
    if (!s_imgReady.load() && s_state.load() != State::Idle) {
      if (fetchLogo(s_logoIata) && s_state.load() == State::Failed) {
        s_state = State::Ready;
      }
    }
  }
}

}  // namespace photo
