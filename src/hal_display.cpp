#include "hal_display.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include "SensorLib.h"
#include "TouchDrvCHSC5816.hpp"
#include "TouchDrvCSTXXX.hpp"
#include "config.h"
#include "pins.h"

namespace hal {

namespace {

Arduino_DataBus* s_bus = nullptr;
Arduino_GFX* s_gfx = nullptr;
bool s_isCo5300 = false;

TouchDrvCHSC5816 s_touchChsc;
TouchDrvCSTXXX s_touchCst;
bool s_touchOk = false;

lv_display_t* s_disp = nullptr;

// One full-frame render buffer in PSRAM. With a partial (strip) buffer LVGL
// re-executes the whole scene draw once per strip — 10x the scene cost per
// frame. Full mode renders the scene exactly once; the QSPI flush of the
// whole frame is ~15 ms. Also keeps internal RAM free for TLS.
uint8_t* s_buf1 = nullptr;

bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  uint32_t w = lv_area_get_width(area);
  uint32_t h = lv_area_get_height(area);
  // Panel wants RGB565 big-endian over QSPI.
  lv_draw_sw_rgb565_swap(px_map, w * h);
  s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)px_map, w, h);
  lv_display_flush_ready(disp);
}

// Panel controller rejects odd-aligned windows — round dirty areas to even.
void roundAreaCb(lv_event_t* e) {
  lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

// Touch is sampled by a dedicated high-priority task every 15 ms into a
// small event queue. LVGL's own read callback only runs between renders —
// with 100 ms+ full-frame renders, quick taps landed entirely inside one
// render window and were never seen.
struct TouchSample {
  int16_t x, y;
  bool pressed;
};
constexpr int kTouchQLen = 16;
TouchSample s_touchQ[kTouchQLen];
volatile uint8_t s_tqHead = 0, s_tqTail = 0;
TouchSample s_touchLast = {0, 0, false};

void touchTask(void*) {
  bool prevPressed = false;
  int16_t lastX = 0, lastY = 0;  // last real contact position
  for (;;) {
    int16_t x = 0, y = 0;
    bool touched = false;
    if (s_touchOk) {
      if (s_isCo5300) {
        touched = s_touchCst.getPoint(&x, &y, 1) > 0;
      } else {
        touched = s_touchChsc.getPoint(&x, &y, 1) > 0;
      }
    }
    if (touched && (x < 0 || x >= cfg::kScreenW || y < 0 ||
                    y >= cfg::kScreenH)) {
      touched = false;
    }
    if (touched) {
      lastX = x;
      lastY = y;
    }
    // Queue transitions and drags; skip idle samples. A release reports the
    // LAST TOUCHED position — the chip reads (0,0) once the finger is gone,
    // which the swipe detector would misread as a flick to the corner.
    if (touched || prevPressed) {
      uint8_t next = (s_tqHead + 1) % kTouchQLen;
      if (next != s_tqTail) {
        s_touchQ[s_tqHead] = {lastX, lastY, touched};
        s_tqHead = next;
      }
    }
    prevPressed = touched;
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void touchReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
  if (s_tqTail != s_tqHead) {
    s_touchLast = s_touchQ[s_tqTail];
    s_tqTail = (s_tqTail + 1) % kTouchQLen;
  }
  if (s_touchLast.pressed) {
    data->point.x = s_touchLast.x;
    data->point.y = s_touchLast.y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->point.x = s_touchLast.x;
    data->point.y = s_touchLast.y;
    data->state = LV_INDEV_STATE_RELEASED;
  }
  // Make LVGL drain the queue now rather than waiting a render cycle.
  data->continue_reading = (s_tqTail != s_tqHead);
}

}  // namespace

bool displayInit() {
  // AMOLED supply enable must precede panel init.
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, HIGH);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // Reset the touch controller, then probe to identify the panel revision.
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(120);

  if (i2cProbe(TOUCH_ADDR_CST816)) {
    s_isCo5300 = true;
  } else if (i2cProbe(TOUCH_ADDR_CHSC5816)) {
    s_isCo5300 = false;
  } else {
    // No touch ACK — default to the current production panel.
    Serial.println("[disp] touch probe failed, assuming CO5300 panel");
    s_isCo5300 = true;
  }
  Serial.printf("[disp] panel: %s\n", s_isCo5300 ? "CO5300/CST816"
                                                 : "SH8601/CHSC5816");

  s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_SDIO0,
                                PIN_LCD_SDIO1, PIN_LCD_SDIO2, PIN_LCD_SDIO3);
  if (s_isCo5300) {
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0, cfg::kScreenW,
                               cfg::kScreenH);
  } else {
    s_gfx = new Arduino_SH8601(s_bus, PIN_LCD_RST, 0, cfg::kScreenW,
                               cfg::kScreenH);
  }
  if (!s_gfx->begin(40000000)) {
    Serial.println("[disp] gfx begin failed");
    return false;
  }
  s_gfx->fillScreen(RGB565_BLACK);
  setBrightness(220);

  // Touch driver init.
  if (s_isCo5300) {
    s_touchCst.setPins(PIN_TOUCH_RST, PIN_TOUCH_INT);
    s_touchOk = s_touchCst.begin(Wire, TOUCH_ADDR_CST816, PIN_I2C_SDA,
                                 PIN_I2C_SCL);
  } else {
    s_touchChsc.setPins(PIN_TOUCH_RST, PIN_TOUCH_INT);
    s_touchOk = s_touchChsc.begin(Wire, TOUCH_ADDR_CHSC5816, PIN_I2C_SDA,
                                  PIN_I2C_SCL);
  }
  Serial.printf("[disp] touch %s\n", s_touchOk ? "ok" : "FAILED");

  // LVGL
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  size_t bufBytes = cfg::kScreenW * cfg::kScreenH * 2;
  s_buf1 = (uint8_t*)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  if (!s_buf1) {
    Serial.println("[disp] buffer alloc failed");
    return false;
  }

  s_disp = lv_display_create(cfg::kScreenW, cfg::kScreenH);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(s_disp, s_buf1, nullptr, bufBytes,
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(s_disp, flushCb);
  lv_display_add_event_cb(s_disp, roundAreaCb, LV_EVENT_INVALIDATE_AREA,
                          nullptr);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchReadCb);

  // Touch sampling must outrank the render loop (prio 1) or taps get lost
  // inside long frames. After init, only this task touches the I2C bus.
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 3, nullptr, 1);

  return true;
}

void setBrightness(uint8_t level) {
  if (!s_gfx) return;
  // Both panels are Arduino_OLED subclasses upstream — setBrightness sends
  // DCS 0x51.
  static_cast<Arduino_OLED*>(s_gfx)->setBrightness(level);
}

bool isCo5300Panel() { return s_isCo5300; }

}  // namespace hal
