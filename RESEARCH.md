# Flight Radar Desk Gadget — Research & Plan
*Compiled 2026-07-05. Goal: a FlightScnr-inspired desk radar with a larger screen, battery + USB-C + sleek switch in a 3D-printed sellable-looking enclosure, auto-location (WiFi/GPS), street-map underlay, flight trails, and projected route lines.*

---

## 1. What FlightScnr actually is

**Hardware:** a single off-the-shelf LilyGO T-Encoder Pro (~$32–38): ESP32-S3R8 (16MB flash / 8MB PSRAM), integrated **1.2" round 390×390 AMOLED** on QSPI (SH8601 or CO5300 driver depending on panel revision, auto-detected), rotary encoder knob + push button + capacitive touch, USB-C, buzzer. No soldering. The MakerWorld enclosure is a simple printed stand the device screws into via its own threaded back ring.

**Software:** PlatformIO / Arduino framework, C++. **No LVGL** — vendored Arduino_GFX 1.3.7 with a custom offscreen framebuffer + dirty-rect blit renderer (~33ms frame budget, sweep interpolated per frame). Key structure:
- `src/main.cpp` (1621 lines) — screen state machine, watchdog
- `src/services/adsb_client.cpp` (896 lines) — background HTTPS fetch task pinned to core 0; render loop on core 1
- `src/services/` — NVS map-center store, 729-line settings web portal, WiFiManager captive portal (AP `FlightScnr-AP` → http://4.3.2.1, then http://flightscnr.local/), route lookup + LittleFS route cache, Tomorrow.io weather, timeapi.io timezone, NTP clock, night mode, shared-TLS-session heap guards
- `src/geo/flat_earth.cpp` — flat-earth lat/lon → screen projection
- Build-time codegen scripts pull tar1090-db + mwgg/Airports into header lookup tables; esptool-js WebFlasher hosted on GitHub Pages

**Data flow:** polls **adsb.fi** (`https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{nm}`) every 2s → ArduinoJson filtered parse → mutex-protected snapshot of ≤64 aircraft → renderer. Fresh TLS handshake per request (3–8s); lots of heap-watermark engineering around it. Optional route enrichment waterfall (AirLabs free 1k/mo → FlightAware AeroAPI → FR24) only fires on the flight-detail screen, cached to flash.

**Location today:** manually typed `lat, lon` in the web portal (default hardcoded SFO), saved to NVS. Timezone auto-derived from that. No GPS, no geolocation.

**Community asks (GitHub issues):** local dump1090 source (#10), orientation offset for wall mount (#15), multiple WiFi SSIDs (#16), aircraft-type alerts (#23), longer range (#13 → now 2–30mi presets), metric units, larger text. A port attempt to a 2.1" Elecrow panel hit heap issues (#1) — firmware is tightly single-target.

**⚠️ Licensing — the big one:**
- Firmware: **CC BY-NC-SA 4.0** — non-commercial + share-alike. **A fork can never be sold.**
- MakerWorld enclosure: Standard Digital File License — **no remixing, no redistribution, no commercial use.**
- Any "something we could sell" ambition requires **our own firmware and our own enclosure design**. Studying FlightScnr's architecture is fine; deriving code from it is not (for commercial use).

---

## 2. Display / platform options (larger screen)

| Option | Size / Res | Interface | Touch | Battery ckt | Price | Notes |
|---|---|---|---|---|---|---|
| **Waveshare ESP32-P4-WIFI6-Touch-LCD-4C** ⭐ big-screen pick | **4" round, 720×720** IPS, bonded toughened glass | MIPI-DSI + HW 2D accel (PPA) | 10-pt cap. | ❌ (RTC header only — needs external charger) | $74.99 (Waveshare) / $99.99 (Amazon) | ESP32-P4 @360MHz, 32MB PSRAM, WiFi-6 via ESP32-C6 co-proc. Fluid LVGL9 at 720×720. |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C | 3.4" round, 800×800 | MIPI-DSI | 10-pt | ❌ | ~$65–78 | Same P4 platform |
| **Waveshare ESP32-S3-Touch-LCD-2.8C** ⭐ value pick | 2.8" round, 480×480 IPS | RGB parallel | Cap. | ✅ LiPo charger + MX1.25 + power switch, IMU, RTC, TF, buzzer | ~$30–37 | Needs LVGL bounce-buffer tuning for smooth sweep |
| Waveshare ESP32-S3-Touch-LCD-2.1 | 2.1" round, 480×480 | RGB parallel | Cap. | ✅ | ~$28–34 | |
| LILYGO T-RGB 2.8" / 2.1" | 480×480 round | RGB (ST7701S) | GT911 | ✅ + microSD | $51.98 / $33.98 | |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | 1.75" round 466×466 **AMOLED** | QSPI | Cap. | ✅ | ~$27–30 | Watch-like polish, easy 60fps, but small |
| Waveshare ESP32-S3-Touch-LCD-1.85/1.85C | 1.85" round 360×360 | QSPI (ST77916) | Opt. | ✅ | ~$19–25 | |
| Guition JC3636W518 | 1.8" round 360×360 | QSPI | Yes | ? | ~$13–18 AliExpress | Budget wildcard, good community support |
| Pi Zero 2 W + HyperPixel 2.1 Round | 2.1" 480×480 | DPI | Cap. | ❌ | ~$71 total | Full Linux/real maps but smaller screen, worse battery story |
| Pi + Waveshare 4" DSI round | 4" 720×720 | DSI | 10-pt | ❌ | ~$60–84 + Pi | DSI officially Pi 4/5/CM only, not Zero 2 W |

**Rendering notes:** QSPI = effortless 50–60fps at ≤466px. RGB-parallel 480×480 on S3 = ~25–33fps, PSRAM contention with WiFi causes tearing unless tuned (LVGL direct mode + bounce buffers). **P4 MIPI-DSI = the standout**: hardware blit/rotate/scale, WiFi on separate C6 chip so networking never steals display bandwidth.

---

## 3. Hardware packaging

**Power architecture:**
- On S3 boards (2.8C etc.): use the **onboard USB-C → charger → MX1.25 LiPo** path — load-sharing for free. Caveats: connector is MX1.25 *not* JST-PH (re-terminate pigtail), and battery pigtail polarity is not standardized — verify against silkscreen.
- On the P4-4C (no charger onboard): **Adafruit bq24074** ($14.95, true power-path) → board 5V in. Avoid bare TP4056 for run-while-charging (no power path — never terminates charge properly under load). IP5306 works but many boards cut output below ~50mA load.
- **Battery:** 2000mAh LiPo sweet spot (Adafruit #2011 $12.50 / PKCELL 803860 ~$9). Draw estimate: S3+WiFi ~90–120mA + backlight 40–100mA → 150–250mA avg → **2000mAh ≈ 7–11h**. P4 + 4" panel will be at the high end or above.

**Switch:**
- Sleekest: **soft-latching MOSFET on a momentary button** — press on, firmware powers itself off, true 0µA off, enables "hold to power off" UX (Mosaic Industries / RNT reference circuits).
- Premium visible: 12mm flush anti-vandal latching metal button w/ LED ring (~$3–5).
- Hidden: MSK-12C02 slide in a recessed base slot — but it's only rated 50mA, use as EN-line signal switch, not in the current path.

**GPS: CUT from the product (decided 2026-07-06).** WiFi geolocation via beaconDB proved out on real hardware at ~57m accuracy — more than enough for a radar centered on your house, with no antenna, no sky-view requirement, and $15–20 off the BOM. The web settings page provides a manual lat/lon override for edge cases. (If ever revisited: Quectel L86-M33 was the pick.)

**Power: USB-C only (decided 2026-07-06).** Single USB-C port on the rear does everything — charging and powering via the bq24074's USB-C input, plus firmware flashing through the board's own USB-C during assembly. No barrel jack, no proprietary cord; ships with (or without) a standard C-to-C cable.

**Enclosure guidelines:** 2.4mm min walls (3.2mm for shell); PETG or ASA, not PLA (heat creep over months); matte dark colors or vapor-smoothed ASA / resin for the sellable finish; M3 brass heat-set inserts (4.0–4.2mm pilot); USB-C cutout +0.5mm/side with inner chamfer, port on the back/bottom; light pipe from charge LED (clear resin or PMMA rod, 45° chamfer on bends); split line hidden at base or behind a snap-on bezel ring; **steel disc/washers 60–120g + TPU foot ring in the base** (weight = perceived quality, stops cable-tug tipping); display angled **30–60° back** — also gives a GPS patch partial sky view.

**Onboard ADS-B receiver (RTL-SDR): skip for v1.** ESP32-S3 USB is full-speed only — cannot host an RTL-SDR. Feasible on ESP32-P4 (working firmware exists for LilyGO T-Display-P4), but +100–300mA, external antenna needs, +$30–40 BOM. Possible "offline pro" SKU later.

---

## 4. Software / data plan (all features verified feasible)

**WiFi geolocation (no GPS) — YES.** ESP32 scans BSSIDs → POST to:
1. **beaconDB** (primary): `POST https://api.beacondb.net/v1/geolocate` — free, keyless (set a distinct User-Agent), MLS/Ichnaea request format, ~20–50m accuracy. (Mozilla Location Service is confirmed dead — shut down 2024; beaconDB is the successor.)
2. **ip-api.com** fallback: free, keyless, city-level (~5–25km) — still fine to center a 30mi radar.
3. Manual override in the web portal always retained. Cache fix in NVS.
- Google Geolocation API (~10k free calls/mo but needs user's own GCP key+billing) and Apple's unofficial WPS (free, protobuf, could break anytime) are optional extras.

**Live aircraft data:** all free aggregators share the ADSBexchange-v2 JSON shape → interchangeable parsing:
- **airplanes.live** (primary): `GET https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}` (≤250nm, 1 req/s) — bonus `desc`/`ownOp`/`year` fields for the detail screen, no key.
- **adsb.lol** (fallback): `/v2/point/...`. adsb.fi (what FlightScnr uses) is personal/non-commercial-only — another reason to switch. OpenSky is 400 credits/day anon — useless for a 2s sweep. ADSBx is paid.
- Poll every 2–3s (respect the 1 req/s limits).

**Route data ("DFW→DEN" + projected line) — YES:** `GET https://api.adsbdb.com/v0/callsign/{callsign}` — free, keyless, returns airline + **origin & destination airports WITH lat/lon** in one call (live-verified). 404 for charters/GA/military → fall back to showing callsign. Cache per callsign (route never changes mid-flight). hexdb.io exists but returned stale/wrong routes in testing.

**Trails — YES, local:** ring buffer per hex, ~30–60 positions × 64 aircraft ≈ trivial RAM. Decimate by distance/heading change. No API for pre-entry history worth using (OpenSky tracks endpoint is auth-gated + experimental).

**Street-map underlay — YES:**
- **Geoapify Static Maps** (recommended): free 3k credits/day w/ key, dark `dark-matter` styles, returns ONE PNG at exactly the resolution/center you ask — no tile-stitching math. A 720×720 map ≈ 3 credits.
- **CARTO dark_matter tiles** (`https://{a-d}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png`): keyless, perfect radar aesthetic, free for hobby/personal — commercial products need an Enterprise license.
- Raw OSM tiles: allowed for light live use with proper User-Agent, but wrong (light) style and shaky for a shipped product.
- Memory: 720×720 PNG decode ≈ 2MB RGBA — fine in 8MB PSRAM (trivial in P4's 32MB). Device is stationary → fetch once per location/zoom change. At volume: a tiny tile proxy (Cloudflare Worker) serving pre-scaled RGB565 removes decode entirely and all policy risk.

**Route line:** great-circle slerp between adsbdb's origin/destination coords, ~20–30 samples, dotted line ahead of aircraft, solid trail behind. Trivial math. OurAirports public-domain CSV (~80k airports) available if we ever want an offline DB.

---

## 5. Recommendation & build paths

**Path B (recommended): clean firmware on the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C.**
- 4" round 720×720 bonded glass is a dramatic upgrade over 1.2" — the "wow" screen.
- Clean-room LVGL 9 / ESP-IDF firmware = no CC BY-NC-SA taint → sellable.
- P4 has the headroom maps + trails + routes want (32MB PSRAM, HW blit).
- Needs: external bq24074 charging, soft-latch power circuit, optional L86 GPS on UART — all straightforward on a small carrier PCB or hand-wired v1.

**Path A (fast prototype): Waveshare ESP32-S3-Touch-LCD-2.8C (~$31)** — battery charging + switch + RTC + buzzer already onboard; get the full software stack working on it in weeks, port UI to P4 later (LVGL makes the port mostly config).

Sensible sequence: **buy both** (~$110 total), develop the firmware LVGL-first on the 2.8C while designing the enclosure around the P4-4C.

**Software milestones:**
1. Scaffold ESP-IDF + LVGL project, radar sweep + aircraft blips from airplanes.live (feature parity-ish)
2. WiFi geolocation (beaconDB → ip-api → manual portal) + NVS cache
3. adsbdb route lookup + cache; route shown instead of flight number (callsign fallback); dotted great-circle line ahead
4. Local trails behind aircraft
5. Optional dark street-map underlay (Geoapify static, toggleable radar/map mode)
6. GPS support (NMEA UART, auto-fix → NVS) when hardware lands
7. Better navigation: touch (tap plane → detail, swipe screens) — we have capacitive touch on all candidate boards; FlightScnr is knob-only

**Enclosure milestones:** design our own (cannot remix the MakerWorld one): angled puck, hidden USB-C rear port, soft-latch button, light pipe, weighted base, GPS patch window under a plastic (not metal/foil) top surface.
