# FlightRadar — Bill of Materials & Print Plan (two-SKU line)
*2026-07-06. Mini ships second; flagship first (see RESEARCH.md). Prices are one-off/prototype qty.*

## Sourcing plan — three orders total

1. **Waveshare** (direct): both display boards. Cheaper than Amazon by $6–25/board; ships from Shenzhen (~1–2 wks).
2. **Adafruit** (one cart, all items verified **In Stock** 2026-07-06): battery, charger, soft-latch, JST cable, panel-mount USB-C.
3. **Amazon + local hardware store**: commodity parts Adafruit doesn't stock (their M3 inserts are out of stock, their 16mm buttons too chunky). Amazon links are searches, not ASINs — generic listings rot within months; pick a current well-reviewed one.

## Shared parts (both SKUs — one inventory)

| Part | Spec | Unit cost | Source | Notes |
|---|---|---|---|---|
| Battery | 2000mAh LiPo, protected, JST-PH | $12.50 | **Adafruit** [#2011](https://www.adafruit.com/product/2011) ✓ in stock | Same cell both SKUs. Mini needs MX1.25 re-termination (Waveshare connector) |
| Power switch module | Soft-latch (LTC2950): press on, firmware-off, 0µA standby | $5.95 | **Adafruit** [#1400](https://www.adafruit.com/product/1400) ✓ in stock | DIY MOSFET circuit (~$0.50) at volume |
| Button | 7mm flush momentary, black | ~$1 | [Amazon search](https://www.amazon.com/s?k=7mm+momentary+push+button+switch+flush+black) | Adafruit's 16mm units too bulky; black OOS there |
| USB-C extension | Small round panel-mount USB-C M-F | $4.95 | **Adafruit** [#6069](https://www.adafruit.com/product/6069) ✓ in stock (alt: [#4218](https://www.adafruit.com/product/4218)) | Routes board's port to the rear cutout |
| Battery pigtails | JST-PH 2-pin cable (+ MX1.25 pigtails for mini) | $0.75 + ~$1 | **Adafruit** [#261](https://www.adafruit.com/product/261) ✓ in stock · [MX1.25: Amazon search](https://www.amazon.com/s?k=mx1.25+2+pin+cable+battery) | |
| Zoom dial ("crown") | EC11-class rotary encoder, 24 detents, push switch, threaded panel-mount bushing | $4.50 | **Adafruit** [#377](https://www.adafruit.com/product/377) ✓ in stock · spares: [EC11 10-pack, Amazon](https://www.amazon.com/s?k=ec11+rotary+encoder+switch+d+shaft) · production: Bourns PEC11R (Mouser, ~$1.20 @ qty) | Panel-mounts through the shell wall with its own nut; 3 GPIO + GND. Rotate = zoom, press = deselect |
| Light pipe | 2mm clear acrylic rod, cut to length | ~$0.20 | [Amazon search](https://www.amazon.com/s?k=2mm+clear+acrylic+rod) | Same stock, different lengths |
| Fasteners | M3 brass heat-set inserts (4.0mm pilot) + M3×8 screws | ~$1 | [Amazon search](https://www.amazon.com/s?k=m3+brass+heat+set+inserts+3d+printing) | Adafruit #4255 exists but out of stock |
| Ballast | ⅜" steel flat washers, stacked in base pocket | ~$1.50–2.50 | Local hardware store | Mini ~70g (5), large ~120g (9) |
| Foot ring | TPU, printed | ~$0.30 | [Amazon: TPU filament](https://www.amazon.com/s?k=tpu+filament+95a+black) | Same profile, scaled diameter |
| Shell filament | PETG or ASA, matte dark (same color both SKUs) | mini ~$2 / large ~$3.50 | [Bambu matte PETG-HF](https://us.store.bambulab.com/products/petg-hf) or equivalent | One spool inventory |
| Firmware | Same codebase, per-board PlatformIO env | — | — | LVGL UI is resolution-parametric |
| Packaging insert, quick-start card | Same design, one variable line | ~$0.50 | — | |

*(Battery alternative if Adafruit ever runs dry: [PKCELL 803860 @ Parts Express](https://www.parts-express.com/Flat-3.7V-2000mAh-Rechargeable-Lithium-Polymer-803860-Battery-with-JST-Type-PH-2.0-Plug-142-111), ~$9.)*

## FlightRadar Mini (2.8" round)

| Part | Cost | Link |
|---|---|---|
| Waveshare ESP32-S3-Touch-LCD-2.8C (480×480 round, touch, **LiPo charger + power switch onboard**, IMU, RTC, buzzer) | ~$31 direct / ~$37 Amazon | [Waveshare direct](https://www.waveshare.com/esp32-s3-touch-lcd-2.8c.htm) · [Amazon](https://www.amazon.com/Waveshare-Capacitive-Development-Dual-core-Processor/dp/B0DMJZPH2R) |
| Shared parts (above) | ~$22 | — |
| **Parts total** | **~$53** | |

No charger board needed — the 2.8C charges the LiPo itself over its USB-C. That makes the mini the *simpler* electrical build: battery → board, done.

## FlightRadar (4" round, flagship)

| Part | Cost | Link |
|---|---|---|
| Waveshare ESP32-P4-WIFI6-Touch-LCD-4C (720×720 round, bonded glass, 10-pt touch, 32MB PSRAM) | $74.99 direct / ~$100 Amazon | [Waveshare direct](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.4c.htm) (page covers 3.4C/4C variants) · [Amazon](https://www.amazon.com/Waveshare-Development-Toughened-Microphones-Bluetooth/dp/B0F9YLX8J2) |
| Adafruit bq24074 power-path charger (**large-only** — P4 board has no charger) | $14.95 | [Adafruit #4755](https://www.adafruit.com/product/4755) |
| Shared parts (above) | ~$24 | — |
| **Parts total** | **~$114** | |

**The only electrical divergence between SKUs is the charger**: mini uses the board's onboard charging; large adds the bq24074 (USB-C ext → bq24074 → battery + 5V to board). Putting a bq24074 in the mini too would unify wiring but adds $15/unit for zero user benefit — rejected.

## 3D-printed parts (one parametric design, two sizes)

Both enclosures come from the same parametric model (OpenSCAD, size parameter) — same wedge-puck geometry, same assembly, so design fixes apply to both. Per unit, five printed parts:

| Part | Material | Shared? |
|---|---|---|
| 1. Main shell (wedge, ~55° face, **crown boss at 2 o'clock**) | PETG/ASA | Same model, scaled: mini ~Ø84×72mm, large ~Ø128×102mm |
| 2. Base plate (washer pocket, insert bosses, vent slots) | PETG/ASA | Same model, scaled |
| 3. Snap-on bezel ring (hides seam + screen adhesive) | PETG/ASA | Same profile, sized to each panel |
| 4. Button cap | PETG/ASA | **Identical STL both SKUs** |
| 5. Crown knob cap (D-shaft bore, knurled rim) | PETG/ASA | **Identical STL both SKUs** |
| 6. Foot ring | TPU | Same profile, scaled diameter |

Identical across both: wall spec (2.4mm), M3 insert bosses, USB-C rear cutout template (+0.5mm clearance, chamfered), light-pipe bore (2.1mm), button hole, crown mount, print orientation (shell face-down on textured plate).

**Crown mount spec (both shells):** local boss at the 2-o'clock position of the
bezel, wall thinned to 2.0mm across the boss face (EC11 bushings are ~5–7mm —
the nut needs thread engagement), 7.2mm through-hole + anti-rotation flat,
nut seats on the inside face. Knob cap: 6.0mm D-bore (flat 4.5mm), ~14mm dia
knurled. Wiring: encoder A/B/switch + GND.
- **Large (P4-4C):** wire to the 40-pin header — GPIO confirmed available.
- **Mini (2.8C):** spare-GPIO situation unverified; if the board exposes
  fewer than 3 usable pins the mini ships touch-only and the boss stays
  unpopulated (cap the hole with the printed plug). Verify on arrival.

Print estimates per unit: mini ~60g / ~5h; large ~140g / ~11h (0.4mm nozzle, 0.2mm layers).

**Status: dimensions are estimates.** Real CAD needs the boards' mounting drawings — Waveshare publishes 2D drawings for both; model after boards arrive and verify against calipers before committing bosses.

## Cost summary

| | Mini | Large |
|---|---|---|
| Electronics | ~$31 | ~$90 |
| Shared hardware (incl. crown encoder) | ~$25 | ~$25 |
| Filament | ~$2 | ~$4 |
| **Parts total** | **~$58** | **~$119** |
| Plausible price | $99 | $179–199 |

*(Prototype totals using the $6 Adafruit soft-latch module; the DIY MOSFET latch saves ~$5/unit at volume.)*

(Excludes labor, packaging shipping box, and the CARTO→Geoapify/self-hosted map licensing swap needed for sold units.)

## Evaluation buy (optional)

[Elecrow CrowPanel 2.1R rotary display](https://www.elecrow.com/crowpanel-2-1inch-hmi-esp32-rotary-display-480-480-ips-round-touch-knob-screen.html) ($35.70) — the "bigger T-Encoder Pro." Not the recommended mini base (smaller + costlier than the 2.8C, and the knob duplicates a control the flagship can't have), but worth one unit to judge whether knob-feel is worth revisiting.
