#include "logo_cache.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFiClientSecure.h>

#include "config.h"

namespace logos {

namespace {

enum : uint8_t { kEmpty = 0, kPending, kReady, kFailed };

struct Entry {
  char iata[4];
  uint8_t state;
  uint16_t w, h;
  uint16_t* buf;
};

constexpr int kMaxEntries = 16;
constexpr int kLogoW = 60, kLogoH = 22;

Entry s_entries[kMaxEntries] = {};
uint8_t s_evict = 0;
SemaphoreHandle_t s_mutex = nullptr;
volatile int s_pendingCount = 0;

PNG* s_png = nullptr;
uint16_t* s_decodeBuf = nullptr;
int s_decodeW = 0, s_decodeH = 0;

int pngLineCb(PNGDRAW* draw) {
  static uint16_t line[128];
  if (draw->y >= s_decodeH || draw->iWidth > 128) return 1;
  s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  memcpy(&s_decodeBuf[draw->y * s_decodeW], line,
         min(draw->iWidth, s_decodeW) * sizeof(uint16_t));
  return 1;
}

void ensureMutex() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

Entry* findLocked(const char* iata) {
  for (auto& e : s_entries) {
    if (e.state != kEmpty && strncmp(e.iata, iata, sizeof(e.iata)) == 0)
      return &e;
  }
  return nullptr;
}

}  // namespace

bool get(const char* iata, Logo* out) {
  if (!iata || !iata[0]) return false;
  ensureMutex();
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Entry* e = findLocked(iata);
  if (!e) {
    // Claim a slot: first empty, else round-robin over non-pending.
    for (auto& c : s_entries) {
      if (c.state == kEmpty) {
        e = &c;
        break;
      }
    }
    if (!e) {
      for (int i = 0; i < kMaxEntries; i++) {
        Entry& c = s_entries[(s_evict + i) % kMaxEntries];
        if (c.state != kPending) {
          e = &c;
          s_evict = (s_evict + i + 1) % kMaxEntries;
          break;
        }
      }
    }
    if (e) {
      if (e->buf) {
        free(e->buf);
        e->buf = nullptr;
      }
      strlcpy(e->iata, iata, sizeof(e->iata));
      e->state = kPending;
      s_pendingCount++;
    }
    xSemaphoreGive(s_mutex);
    return false;
  }
  bool ready = (e->state == kReady && e->buf);
  if (ready) {
    out->buf = e->buf;
    out->w = e->w;
    out->h = e->h;
  }
  xSemaphoreGive(s_mutex);
  return ready;
}

bool pending() { return s_pendingCount > 0; }

void service() {
  if (s_pendingCount <= 0) return;
  ensureMutex();

  char iata[4] = {0};
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  for (auto& e : s_entries) {
    if (e.state == kPending) {
      strlcpy(iata, e.iata, sizeof(iata));
      break;
    }
  }
  xSemaphoreGive(s_mutex);
  if (!iata[0]) {
    s_pendingCount = 0;
    return;
  }

  // Fetch + decode outside the lock (slow).
  uint16_t* buf = nullptr;
  int w = 0, h = 0;
  {
    char url[80];
    snprintf(url, sizeof(url), "https://pics.avs.io/%d/%d/%s.png", kLogoW,
             kLogoH, iata);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setUserAgent(cfg::kUserAgent);
    http.setTimeout(cfg::kHttpTimeoutMs);
    http.useHTTP10(true);
    if (http.begin(client, url) && http.GET() == 200) {
      int len = http.getSize();
      if (len > 0 && len < 32 * 1024) {
        uint8_t* png = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (png) {
          NetworkClient* st = http.getStreamPtr();
          size_t got = 0;
          uint32_t lastData = millis();
          while (got < (size_t)len) {
            size_t avail = st->available();
            if (!avail) {
              if (!http.connected() ||
                  millis() - lastData > cfg::kHttpTimeoutMs)
                break;
              delay(5);
              continue;
            }
            int r = st->readBytes(png + got, min(avail, (size_t)2048));
            if (r <= 0) break;
            got += r;
            lastData = millis();
          }
          if (got == (size_t)len) {
            if (!s_png) s_png = new PNG();
            if (s_png->openRAM(png, len, pngLineCb) == PNG_SUCCESS) {
              s_decodeW = s_png->getWidth();
              s_decodeH = s_png->getHeight();
              if (s_decodeW > 0 && s_decodeW <= 80 && s_decodeH > 0 &&
                  s_decodeH <= 32) {
                s_decodeBuf = (uint16_t*)heap_caps_malloc(
                    s_decodeW * s_decodeH * 2, MALLOC_CAP_SPIRAM);
                if (s_decodeBuf) {
                  memset(s_decodeBuf, 0xFF, s_decodeW * s_decodeH * 2);
                  if (s_png->decode(nullptr, 0) == PNG_SUCCESS) {
                    buf = s_decodeBuf;
                    w = s_decodeW;
                    h = s_decodeH;
                  } else {
                    free(s_decodeBuf);
                  }
                  s_decodeBuf = nullptr;
                }
              }
              s_png->close();
            }
          }
          free(png);
        }
      }
    }
    http.end();
  }

  xSemaphoreTake(s_mutex, portMAX_DELAY);
  Entry* e = findLocked(iata);
  if (e && e->state == kPending) {
    e->buf = buf;
    e->w = (uint16_t)w;
    e->h = (uint16_t)h;
    e->state = buf ? kReady : kFailed;
    s_pendingCount--;
  } else if (buf) {
    free(buf);  // entry was evicted mid-fetch
  }
  xSemaphoreGive(s_mutex);
}

}  // namespace logos
