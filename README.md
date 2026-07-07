# Flight Radar

A clean-room ADS-B desk radar for the **LilyGO T-Encoder Pro** (ESP32-S3, 1.2" round 390×390 AMOLED). Inspired by FlightScnr but written from scratch (FlightScnr's firmware is CC BY-NC-SA and its code is GPL-adjacent via LilyGO's repo; this project uses only BSD/MIT libraries so it stays commercially clean for the future product).

## Features (v0.1)

- **Sweeping radar** with range rings, phosphor-green theme, ~30 fps
- **Live traffic** from airplanes.live (adsb.lol automatic fallback), 3 s polling
- **Auto location** — no typing coordinates:
  1. WiFi geolocation via beaconDB (free, keyless, ~20–50 m)
  2. IP geolocation fallback (city level)
  3. Optional manual `lat, lon` override in the setup portal
- **Routes instead of flight numbers** — blips are labeled `ATL>DFW` (via free adsbdb.com lookups, cached) with callsign fallback for GA/military
- **Flight trails** — fading history line behind each aircraft (accumulated locally)
- **Projected route line** — tap a plane: dotted great-circle line to its destination
- **Touch + encoder navigation** — tap a plane for a detail card (callsign, type, route, altitude, speed, distance); knob rotates through range presets (5/10/20/40/80/150 nm); knob click clears selection; knob long-press re-runs geolocation
- **Panel auto-detect** — supports both T-Encoder Pro display revisions (SH8601+CHSC5816 and CO5300+CST816) at runtime

## First boot

1. Flash (below), power on.
2. The screen shows **"Join WiFi FlightRadar-Setup"** — connect with your phone, a captive portal opens; pick your WiFi and (optionally) enter a manual `lat, lon`.
3. The device finds its own location and starts sweeping.

## Build & flash

```sh
# from this directory
~/.platformio/penv/bin/pio run                 # compile
~/.platformio/penv/bin/pio run -t upload       # flash over USB-C
~/.platformio/penv/bin/pio device monitor      # serial logs @115200
```

If upload doesn't auto-detect the port: hold the knob (BOOT) while plugging in USB, then `pio run -t upload`.

## Architecture

| File | Role |
|---|---|
| `src/main.cpp` | setup + UI loop (core 1) |
| `src/net_task.cpp` | WiFi portal → geolocate → poll loop (core 0) |
| `src/adsb_client.cpp` | airplanes.live/adsb.lol poller (filtered JSON parse) |
| `src/route_client.cpp` | adsbdb callsign→route, PSRAM cache |
| `src/geolocate.cpp` | beaconDB / ip-api / manual, NVS-cached |
| `src/aircraft_store.cpp` | mutex-guarded aircraft + trail store |
| `src/ui_radar.cpp` | LVGL 9 custom-draw radar, detail panel |
| `src/hal_display.cpp` | panel auto-detect, Arduino_GFX + LVGL glue |
| `src/hal_encoder.cpp` | polled quadrature + button |
| `src/pins.h` | T-Encoder Pro pin facts |
| `src/config.h` | all tunables |

Libraries: Arduino_GFX ≥1.6.1 (BSD), LVGL 9 (MIT), SensorLib (MIT), ArduinoJson (MIT), WiFiManager (MIT). Arduino core 3.0.7 via pioarduino.

## Roadmap

See [RESEARCH.md](RESEARCH.md): street-map underlay (Geoapify dark static maps), GPS module support, then port to the Waveshare ESP32-P4 4" round 720×720 for the production device (battery + USB-C + soft-latch power in a custom enclosure).
