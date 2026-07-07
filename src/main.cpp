// Flight Radar — a clean-room ADS-B desk radar.
// Target: LilyGO T-Encoder Pro (see pins.h). UI on core 1, network on core 0.

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <lvgl.h>

#include "aircraft_store.h"
#include "app_state.h"
#include "config.h"
#include "hal_display.h"
#include "hal_encoder.h"
#include "map_layer.h"
#include "net_task.h"
#include "ui_radar.h"

void setup() {
  Serial.begin(115200);
  Serial.printf("\nFlightRadar %s\n", cfg::kFwVersion);

  gAircraft.begin();
  maplayer::begin();
  hal::encoderInit();

  if (!hal::displayInit()) {
    Serial.println("FATAL: display init failed");
  }
  ui::init();

  net::start();

  // UI watchdog: if rendering/input ever wedges, reboot rather than freeze.
  // (Ensure the TWDT exists regardless of core defaults, then subscribe.)
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = 30000;
  wdtCfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
  esp_task_wdt_add(nullptr);
}

void loop() {
  esp_task_wdt_reset();
  hal::encoderPoll();
  ui::tick();
  lv_timer_handler();
  delay(2);
}
