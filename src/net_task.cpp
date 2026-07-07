#include "net_task.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>

#include "adsb_client.h"
#include "app_state.h"
#include "config.h"
#include "geolocate.h"
#include "logo_cache.h"
#include "map_layer.h"
#include "photo_client.h"
#include "route_client.h"
#include "web_settings.h"

namespace net {

namespace {

WiFiManagerParameter* s_coordParam = nullptr;

void onSaveParams() {
  const char* v = s_coordParam->getValue();
  if (v && v[0]) {
    if (geolocate::saveManual(v)) {
      Serial.printf("[net] manual location saved: %s\n", v);
    } else {
      Serial.printf("[net] bad manual location: %s\n", v);
    }
  }
}

void taskFn(void*) {
  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(600);
  wm.setAPCallback(
      [](WiFiManager*) { gApp.netPhase = NetPhase::Portal; });
  WiFiManagerParameter coordParam(
      "coords", "Radar center override: lat, lon (blank = automatic)", "", 40);
  s_coordParam = &coordParam;
  wm.addParameter(&coordParam);
  wm.setSaveParamsCallback(onSaveParams);

  gApp.netPhase = NetPhase::Connecting;
  // The setup AP is WPA2-protected with the device password (shown on the
  // screen) — an open AP would let bystanders capture the WiFi credentials.
  char apPass[9];
  devicePassword(apPass);
  if (!wm.autoConnect(cfg::kSetupApName, apPass)) {
    Serial.println("[net] portal timed out, rebooting");
    ESP.restart();
  }
  gApp.wifiUp = true;
  Serial.printf("[net] WiFi up: %s\n", WiFi.localIP().toString().c_str());

  routes::loadPersist();  // yesterday's routes work before today's network

  gApp.netPhase = NetPhase::Locating;
  geolocate::loadStored();
  geolocate::run();

  gApp.netPhase = NetPhase::Running;
  websrv::start();

  // Watchdog: a wedged net task reboots the device back to a working radar
  // instead of freezing it. Subscribed only after the (blocking) WiFi
  // portal, which can legitimately sit for minutes.
  esp_task_wdt_config_t wdtCfg = {};
  wdtCfg.timeout_ms = 30000;
  wdtCfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdtCfg);
  esp_task_wdt_add(nullptr);

  uint32_t nextPollMs = 0;
  uint32_t nextRouteMs = 0;
  uint32_t lastPollStartMs = 0;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    websrv::service();

    if (WiFi.status() != WL_CONNECTED) {
      gApp.wifiUp = false;
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    gApp.wifiUp = true;

    if (gApp.relocateReq.exchange(false)) {
      gApp.netPhase = NetPhase::Locating;
      geolocate::reset();
      geolocate::run();
      gApp.netPhase = NetPhase::Running;
    }

    // Zoom changed: poll as soon as the rate limit allows (1 req/s APIs;
    // stay comfortably above it).
    if (gApp.pollNowReq.exchange(false)) {
      uint32_t earliest = lastPollStartMs + 1500;
      nextPollMs = ((int32_t)(earliest - now) > 0) ? earliest : now;
    }

    if (photo::pending()) {
      // A user just opened the aircraft page — this outranks everything.
      // Traffic polling resumes next cycle (one skipped beat at most).
      photo::service();
    } else if ((int32_t)(now - nextPollMs) >= 0) {
      lastPollStartMs = now;
      uint32_t retryAfter = 0;
      bool ok = adsb::poll(&retryAfter);
      if (!ok && retryAfter == 0) retryAfter = cfg::kAdsbPollMs;
      nextPollMs = millis() + (retryAfter ? retryAfter : cfg::kAdsbPollMs);
    } else if ((int32_t)(now - nextRouteMs) >= 0) {
      // Route lookups ride between traffic polls so TLS sessions don't stack.
      routes::serviceOne();
      nextRouteMs = millis() + cfg::kRouteLookupSpacingMs;
    } else {
      // Card logos first (a user is looking at the card), then map tiles.
      logos::service();
      maplayer::service();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void start() {
  xTaskCreatePinnedToCore(taskFn, "net", 16384, nullptr, 1, nullptr, 0);
}

}  // namespace net
