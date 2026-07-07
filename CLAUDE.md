# FlightRadar — project rules

## Priority #1: responsiveness

Input latency outranks every feature. Taps and wheel turns must feel instant.
This was learned the hard way: individually-cheap render additions compounded
into 100–200ms frames that silently ate taps.

Rules that follow from it:
- Touch is sampled by a dedicated FreeRTOS task (prio 3, 15ms) into a queue
  (`hal_display.cpp`) — never sample input from the render path.
- Redraws are event-driven, never timer-driven: invalidate only when data
  actually changed (see the snapshot `lastUpdateMs` check in `ui_radar.cpp`).
- Anything added to `radarDrawCb` adds to every frame — estimate its cost
  first, and prefer precomputation outside the draw callback.
- If a feature and responsiveness conflict, responsiveness wins by default;
  surface the tradeoff instead of accepting slowdown.
- After UI changes, verify tap-to-select feel on hardware, not just a clean
  compile.

## Other standing constraints

- Clean-room firmware: BSD/MIT libraries only. Never copy from the GPL LilyGO
  repo or the CC BY-NC-SA FlightScnr project — this codebase must stay
  sellable.
- All HTTP bodies: `http.useHTTP10(true)` + bulk-read into PSRAM, then parse.
- Big buffers live in PSRAM; internal RAM is reserved for TLS (~45KB/session).
- Build: `~/.platformio/penv/bin/pio run` (env: tencoder-pro).
