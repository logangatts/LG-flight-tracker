#include "map_layer.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <math.h>

#include "app_state.h"
#include "config.h"
#include "geo_math.h"

namespace maplayer {

namespace {

constexpr int kW = cfg::kScreenW;
constexpr int kH = cfg::kScreenH;
constexpr int kTile = 256;

// One cached canvas per zoom preset, prefetched after the location fix so
// wheel zooming swaps instantly instead of waiting on tile downloads.
struct Level {
  uint16_t* buf;
  std::atomic<bool> valid{false};
  float lat, lon;  // center it was rendered for
};
Level s_levels[cfg::kZoomCount];
uint16_t* s_scratch = nullptr;   // render target, swapped into a level
uint16_t* s_tileBuf = nullptr;   // one decoded 256x256 tile
std::atomic<bool> s_dirty{false};

PNG* s_png = nullptr;

// PNG line callback: copy rows into s_tileBuf.
int pngLineCb(PNGDRAW* draw) {
  static uint16_t line[kTile];
  if (draw->y >= kTile) return 1;
  s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  memcpy(&s_tileBuf[draw->y * kTile], line,
         min(draw->iWidth, kTile) * sizeof(uint16_t));
  return 1;
}

size_t fetchTile(HTTPClient& http, WiFiClientSecure& client, uint8_t z,
                 uint32_t tx, uint32_t ty, uint8_t** out) {
  char url[128];
  snprintf(url, sizeof(url), cfg::kMapTileUrl, (unsigned)z, (unsigned long)tx,
           (unsigned long)ty);
  if (!http.begin(client, url)) return 0;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[map] tile HTTP %d (%s)\n", code, url);
    http.end();
    return 0;
  }
  int len = http.getSize();
  if (len <= 0 || len > 256 * 1024) {
    http.end();
    return 0;
  }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) {
    http.end();
    return 0;
  }
  NetworkClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t lastData = millis();
  while (got < (size_t)len) {
    size_t avail = stream->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > cfg::kHttpTimeoutMs) break;
      delay(5);
      continue;
    }
    int r = stream->readBytes(buf + got, min(avail, (size_t)4096));
    if (r <= 0) break;
    got += r;
    lastData = millis();
  }
  http.end();
  if (got != (size_t)len) {
    free(buf);
    return 0;
  }
  *out = buf;
  return got;
}

// Render one canvas at zoom z with oversample factor k (screen px covers k
// world px; k=1 for native levels, ~1.24 on the capped widest view).
bool renderCanvas(uint16_t* dst, float lat, float lon, uint8_t z, float k) {
  double cwx, cwy;
  geo::latLonToWorldPx(lat, lon, z, cwx, cwy);
  double tlx = cwx - (kW / 2.0) * k;
  double tly = cwy - (kH / 2.0) * k;
  double spanX = kW * (double)k;
  double spanY = kH * (double)k;

  memset(dst, 0, kW * kH * sizeof(uint16_t));

  long tx0 = (long)floor(tlx / kTile);
  long ty0 = (long)floor(tly / kTile);
  long tx1 = (long)floor((tlx + spanX - 1) / kTile);
  long ty1 = (long)floor((tly + spanY - 1) / kTile);
  long nMax = 1L << z;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setReuse(true);

  int okCount = 0, total = 0;
  for (long ty = ty0; ty <= ty1; ty++) {
    if (ty < 0 || ty >= nMax) continue;
    for (long tx = tx0; tx <= tx1; tx++) {
      total++;
      long wrapX = ((tx % nMax) + nMax) % nMax;
      uint8_t* buf = nullptr;
      size_t len =
          fetchTile(http, client, z, (uint32_t)wrapX, (uint32_t)ty, &buf);
      if (!len) continue;

      memset(s_tileBuf, 0, kTile * kTile * sizeof(uint16_t));
      int rc = s_png->openRAM(buf, (int)len, pngLineCb);
      if (rc == PNG_SUCCESS) {
        rc = s_png->decode(nullptr, 0);
        s_png->close();
      }
      free(buf);
      if (rc != PNG_SUCCESS) {
        Serial.printf("[map] png decode failed %d\n", rc);
        continue;
      }
      okCount++;

      // Blit tile into dst, sampling world->screen at factor k.
      double tileWx = tx * (double)kTile;
      double tileWy = ty * (double)kTile;
      int y0 = max(0, (int)ceil((tileWy - tly) / k));
      int y1v = min(kH - 1, (int)floor((tileWy + kTile - 1 - tly) / k));
      int x0 = max(0, (int)ceil((tileWx - tlx) / k));
      int x1v = min(kW - 1, (int)floor((tileWx + kTile - 1 - tlx) / k));
      // Dim to ~75% while blitting (RGB565: 1/2 + 1/4 of each channel) so
      // the UI can draw the canvas fully opaque — an alpha blend of a
      // 390x390 image in software costs more than the whole rest of the
      // scene.
      auto dim = [](uint16_t p) -> uint16_t {
        return (uint16_t)(((p >> 1) & 0x7BEF) + ((p >> 2) & 0x39E7));
      };
      for (int y = y0; y <= y1v; y++) {
        int srcY = (int)(tly + y * (double)k - tileWy);
        if (srcY < 0 || srcY >= kTile) continue;
        uint16_t* dstRow = &dst[y * kW];
        uint16_t* srcRow = &s_tileBuf[srcY * kTile];
        if (k == 1.0f) {
          int srcX0 = (int)(tlx + x0 - tileWx);
          for (int x = x0; x <= x1v; x++) dstRow[x] = dim(srcRow[srcX0 + x - x0]);
        } else {
          for (int x = x0; x <= x1v; x++) {
            int srcX = (int)(tlx + x * (double)k - tileWx);
            if (srcX >= 0 && srcX < kTile) dstRow[x] = dim(srcRow[srcX]);
          }
        }
      }
    }
  }
  Serial.printf("[map] z%u k=%.2f: %d/%d tiles (heap %u)\n", z, k, okCount,
                total, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  return okCount > 0 && okCount == total;
}

}  // namespace

void begin() {
  size_t bytes = kW * kH * sizeof(uint16_t);
  for (auto& l : s_levels) {
    l.buf = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  }
  s_scratch = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  s_tileBuf =
      (uint16_t*)heap_caps_malloc(kTile * kTile * 2, MALLOC_CAP_SPIRAM);
  s_png = new PNG();
}

void service() {
  if (!gApp.mapEnabled.load() || !s_scratch) return;

  float lat = gApp.centerLat.load();
  float lon = gApp.centerLon.load();

  // Pick the level to (re)render: current zoom first, then prefetch the rest
  // (nearest zoom levels first). One level per call keeps the net task
  // responsive between traffic polls.
  uint8_t cur = gApp.zoomIdx.load();
  int target = -1;
  for (int pass = 0; pass < cfg::kZoomCount && target < 0; pass++) {
    int candidates[2] = {cur - pass, cur + pass};
    for (int c : candidates) {
      if (c < 0 || c >= cfg::kZoomCount) continue;
      Level& l = s_levels[c];
      if (!l.buf) continue;
      bool fresh = l.valid.load() && fabsf(lat - l.lat) < 1e-4f &&
                   fabsf(lon - l.lon) < 1e-4f;
      if (!fresh) {
        target = c;
        break;
      }
    }
  }
  if (target < 0) return;  // everything cached

  uint8_t z = gApp.zForIdx((uint8_t)target);
  if (!renderCanvas(s_scratch, lat, lon, z, gApp.kForIdx((uint8_t)target)))
    return;

  Level& l = s_levels[target];
  l.valid = false;
  uint16_t* old = l.buf;
  l.buf = s_scratch;
  s_scratch = old;
  l.lat = lat;
  l.lon = lon;
  l.valid = true;
  s_dirty = true;
}

const uint16_t* canvas() {
  if (!gApp.mapEnabled.load()) return nullptr;
  uint8_t cur = gApp.zoomIdx.load();
  Level& l = s_levels[cur];
  if (!l.valid.load()) return nullptr;
  // Only serve a canvas rendered for (roughly) the current center.
  if (fabsf(gApp.centerLat.load() - l.lat) > 1e-3f ||
      fabsf(gApp.centerLon.load() - l.lon) > 1e-3f)
    return nullptr;
  return l.buf;
}

bool takeDirty() { return s_dirty.exchange(false); }

}  // namespace maplayer
