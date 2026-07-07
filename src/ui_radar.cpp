#include "ui_radar.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lvgl.h>
#include <math.h>

#include "hal_display.h"

#include "aircraft_store.h"
#include "app_state.h"
#include "config.h"
#include "geo_math.h"
#include "airports_data.h"
#include "hal_encoder.h"
#include "logo_cache.h"
#include "map_layer.h"
#include "photo_client.h"
#include "route_client.h"

namespace ui {

namespace {

// ---- palette ----
#ifdef SWA_THEME
// Southwest Heart livery: Bold Blue #304CB2, Warm Red #D5152E,
// Sunrise Yellow #FFBF27, Summit Silver #CCCCCC (verified corporate hexes).
// Neutrals (black bg, grays) carry the canvas; every colored element uses
// exact brand values. Warm Red is reserved for emergencies.
const lv_color_t kColBg = lv_color_hex(0x000000);
const lv_color_t kColGrid = lv_color_hex(0x304CB2);
const lv_color_t kColGridBright = lv_color_hex(0x304CB2);
const lv_color_t kColSweep = lv_color_hex(0xFFBF27);
const lv_color_t kColPlane = lv_color_hex(0x304CB2);
const lv_color_t kColPlaneGnd = lv_color_hex(0xCCCCCC);
const lv_color_t kColTrail = lv_color_hex(0xFFBF27);
const lv_color_t kColRoute = lv_color_hex(0xFFBF27);
const lv_color_t kColSel = lv_color_hex(0xFFFFFF);
const lv_color_t kColText = lv_color_hex(0xFFFFFF);
const lv_color_t kColTextDim = lv_color_hex(0xCCCCCC);
const lv_color_t kColAirport = lv_color_hex(0xCCCCCC);
const lv_color_t kColEmerg = lv_color_hex(0xD5152E);
const lv_color_t kColAirline = lv_color_hex(0x304CB2);
const lv_color_t kColCardBg = lv_color_hex(0x0A1030);
const lv_color_t kColPanelBg = lv_color_hex(0x060A20);
const lv_color_t kColHint = lv_color_hex(0x4A5578);
#else
// Radar phosphor (standard build)
const lv_color_t kColBg = lv_color_hex(0x000000);
const lv_color_t kColGrid = lv_color_hex(0x0d3a1e);
const lv_color_t kColGridBright = lv_color_hex(0x1c5c31);
const lv_color_t kColSweep = lv_color_hex(0x27f06c);
const lv_color_t kColPlane = lv_color_hex(0x3cff81);
const lv_color_t kColPlaneGnd = lv_color_hex(0x1a7a41);
const lv_color_t kColTrail = lv_color_hex(0x2bd05f);
const lv_color_t kColRoute = lv_color_hex(0x37b6ff);
const lv_color_t kColSel = lv_color_hex(0xfff27a);
const lv_color_t kColText = lv_color_hex(0x9df5bd);
const lv_color_t kColTextDim = lv_color_hex(0x4d8f66);
const lv_color_t kColAirport = lv_color_hex(0x3f8ea3);  // muted cyan
const lv_color_t kColEmerg = lv_color_hex(0xff4d4d);    // emergency squawk
const lv_color_t kColAirline = lv_color_hex(0xe8f6ff);  // airliners (ice white)
const lv_color_t kColCardBg = lv_color_hex(0x07200f);
const lv_color_t kColPanelBg = lv_color_hex(0x03140a);
const lv_color_t kColHint = lv_color_hex(0x2a4a36);
#endif

constexpr float kCx = cfg::kScreenW / 2.0f;
constexpr float kCy = cfg::kScreenH / 2.0f;

// Reading-text fonts, one step larger when the "Text size" setting is on.
const lv_font_t* fSm() {
  return gApp.textLarge.load() ? &lv_font_montserrat_12
                               : &lv_font_montserrat_10;
}
const lv_font_t* fBody() {
  return gApp.textLarge.load() ? &lv_font_montserrat_14
                               : &lv_font_montserrat_12;
}
const lv_font_t* fMed() {
  return gApp.textLarge.load() ? &lv_font_montserrat_20
                               : &lv_font_montserrat_16;
}

lv_obj_t* s_radar = nullptr;
lv_obj_t* s_topLabel = nullptr;
lv_obj_t* s_subLabel = nullptr;
lv_obj_t* s_rangeLabel = nullptr;
lv_obj_t* s_bootLabel = nullptr;

// Detail panel
lv_obj_t* s_panel = nullptr;
lv_obj_t* s_panelLogo = nullptr;
lv_image_dsc_t s_panelLogoDsc;
lv_obj_t* s_panelL1 = nullptr;
lv_obj_t* s_panelL2 = nullptr;
lv_obj_t* s_panelL3 = nullptr;

// Full-screen aircraft page (opened by tapping the detail panel)
lv_obj_t* s_acPage = nullptr;
lv_obj_t* s_acImg = nullptr;
lv_obj_t* s_acTitle = nullptr;
lv_obj_t* s_acRoute = nullptr;  // airline + route (from local data, instant)
lv_obj_t* s_acMono = nullptr;     // fallback roundel when no airline logo
lv_obj_t* s_acMonoLbl = nullptr;
lv_obj_t* s_acDiv1 = nullptr;
lv_obj_t* s_acDiv2 = nullptr;
lv_obj_t* s_acSec1 = nullptr;   // "AIRCRAFT"
lv_obj_t* s_acSec2 = nullptr;   // "LIVE"
lv_obj_t* s_acInfo = nullptr;
lv_obj_t* s_acLive = nullptr;
lv_image_dsc_t s_acImgDsc;
bool s_acPageOpen = false;
bool s_acImgShown = false;
char s_acLogoTried[4] = {0};  // late-logo guard: one retry per airline code

// Settings page (swipe up) + nearby list (swipe down)
lv_obj_t* s_setPage = nullptr;
lv_obj_t* s_setPg[2] = {nullptr, nullptr};  // settings sub-pages (dial flips)
uint8_t s_setPgIdx = 0;
lv_obj_t* s_setTitle = nullptr;
lv_obj_t* s_setFltLbl = nullptr;
lv_obj_t* s_setTxtLbl = nullptr;
lv_obj_t* s_setUnitLbl = nullptr;
lv_obj_t* s_setWifiLbl = nullptr;
lv_obj_t* s_setInfoLbl = nullptr;
bool s_setOpen = false;
uint8_t s_briIdx = 3;  // 25/50/75/100%
uint32_t s_wifiConfirmMs = 0;
const uint8_t kBriLevels[4] = {64, 128, 192, 255};

// First-boot tutorial (replayable from settings)
lv_obj_t* s_tutPage = nullptr;
lv_obj_t* s_tutTitle = nullptr;
lv_obj_t* s_tutBody = nullptr;
lv_obj_t* s_tutStep = nullptr;
bool s_tutOpen = false;
bool s_tutSeen = false;
uint8_t s_tutIdx = 0;

struct TutStep {
  const char* title;
  const char* body;
};
const TutStep kTutSteps[] = {
    {"FlightRadar",
     "Live aircraft around you,\non a real street map,\nupdated every few "
     "seconds."},
    {"The radar",
     "Each plane shows its route\n(like DFW > LAX), altitude\nand speed. The "
     "thin line\nshows where it's heading."},
    {"Controls",
     "Turn the knob to zoom.\nTap a plane for details.\nTap the info card\nfor "
     "photo & facts."},
    {"Swipe",
     "Swipe down: nearest\nplanes list.\nSwipe up: settings.\nHold the knob "
     "to\nre-find your location."},
    {"That's it",
     "White = airliners,\ngreen = small planes,\nred = emergency.\nAirports "
     "appear as\nyou zoom in.\n\nEnjoy the skies!"},
};
constexpr uint8_t kTutCount = sizeof(kTutSteps) / sizeof(kTutSteps[0]);

lv_obj_t* s_nearPage = nullptr;
bool s_nearOpen = false;
// Row hit-testing for the custom-drawn nearby list.
constexpr int kNearRows = 6;
constexpr int kNearRowH = 47;
constexpr int kNearTopY = 50;
int s_nearRowIdx[kNearRows];  // snapshot index per row, -1 = empty

void savePrefs() {
  Preferences p;
  p.begin("ui", false);
  p.putUChar("bri", s_briIdx);
  p.putUChar("altu", gApp.altUnit.load());
  p.putUChar("spdu", gApp.spdUnit.load());
  p.putBool("acol", gApp.airlineColors.load());
  p.putBool("txtl", gApp.textLarge.load());
  p.end();
}

void loadPrefs() {
  Preferences p;
  p.begin("ui", true);
  s_briIdx = p.getUChar("bri", 3);
  s_tutSeen = p.getBool("tut", false);
  gApp.altUnit = p.getUChar("altu", 0);
  gApp.spdUnit = p.getUChar("spdu", 0);
  gApp.airlineColors = p.getBool("acol", true);
  gApp.colComm = min(p.getUChar("ccol", kPlaneColorDefaultComm),
                     (uint8_t)(kPlaneColorCount - 1));
  gApp.colPriv = min(p.getUChar("pcol", kPlaneColorDefaultPriv),
                     (uint8_t)(kPlaneColorCount - 1));
  gApp.textLarge = p.getBool("txtl", false);
  char flt[8];
  strlcpy(flt, p.getString("flt", DEFAULT_CALLSIGN_FILTER).c_str(),
          sizeof(flt));
  gApp.setCallsignFilter(flt);
  p.end();
  if (s_briIdx > 3) s_briIdx = 3;
  hal::setBrightness(kBriLevels[s_briIdx]);
  gApp.briIdx = s_briIdx;
  gApp.mapEnabled = true;  // map is always on — it's the reference layer
}

// ---------- manual swipe detection ----------
// LVGL's built-in gesture recognizer is unreliable on a 390px round panel
// (it races click/scroll handling), so swipes are detected explicitly:
// record the press point, compare on release.
lv_point_t s_pressPt;
bool s_swiped = false;  // set on a swipe-release; click handlers must bail

void pressCb(lv_event_t*) {
  s_swiped = false;
  lv_indev_t* indev = lv_indev_active();
  if (indev) lv_indev_get_point(indev, &s_pressPt);
}

// Returns -1 = swipe up, +1 = swipe down, 0 = not a vertical swipe.
int releaseSwipeDir() {
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return 0;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  int dy = p.y - s_pressPt.y;
  int dx = p.x - s_pressPt.x;
  if (abs(dy) < 60 || abs(dy) < abs(dx) * 3 / 2) return 0;
  return dy < 0 ? -1 : 1;
}

AircraftSnapshot& snap() {  // lives in PSRAM, allocated on first use
  static AircraftSnapshot* s = []() {
    void* p = heap_caps_calloc(1, sizeof(AircraftSnapshot), MALLOC_CAP_SPIRAM);
    if (!p) p = calloc(1, sizeof(AircraftSnapshot));
    return (AircraftSnapshot*)p;
  }();
  return *s;
}
#define s_snap snap()
uint32_t s_lastSnapMs = 0;
uint32_t s_lastTextMs = 0;

// Screen position of each snapshot aircraft (for tap hit-testing).
float s_px[cfg::kMaxAircraft];
float s_py[cfg::kMaxAircraft];
bool s_onScreen[cfg::kMaxAircraft];

char s_selHex[8] = {0};  // selected aircraft ("" = none)

void updateDetailPanel();
const Aircraft* selectedAircraft();
void updateAircraftPage();

void openAircraftPage() {
  const Aircraft* sel = selectedAircraft();
  if (!sel) return;
  s_acPageOpen = true;
  s_acImgShown = false;
  s_acLogoTried[0] = '\0';
  photo::request(sel->hex, sel->routeState == RouteState::Found
                               ? sel->route.airlineIata
                               : "");
  lv_label_set_text(s_acTitle, sel->callsign[0] ? sel->callsign : sel->hex);

  // Everything we already know locally goes up instantly (one line).
  if (sel->routeState == RouteState::Found) {
    char rt[72];
    if (sel->route.airline[0]) {
      snprintf(rt, sizeof(rt), "%s  •  %s > %s", sel->route.airline,
               sel->route.origIata, sel->route.destIata);
    } else {
      snprintf(rt, sizeof(rt), "%s > %s", sel->route.origIata,
               sel->route.destIata);
    }
    lv_label_set_text(s_acRoute, rt);
  } else {
    lv_label_set_text(s_acRoute, sel->type[0] ? sel->type : "");
  }
  // Monogram roundel shows immediately (local data); an airline logo
  // replaces it if one arrives.
  char mono[4] = {0};
  for (int i = 0, o = 0; sel->callsign[i] && o < 3; i++) {
    if (isupper((unsigned char)sel->callsign[i])) mono[o++] = sel->callsign[i];
  }
  if (!mono[0]) strlcpy(mono, "GA", sizeof(mono));
  lv_label_set_text(s_acMonoLbl, mono);
  lv_obj_set_style_border_color(
      s_acMono, lv_color_hex(kPlaneColors[gApp.colComm.load()].hex), 0);
  lv_obj_clear_flag(s_acMono, LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text(s_acInfo, "...");
  lv_label_set_text(s_acLive, "");
  lv_obj_add_flag(s_acImg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_acPage, LV_OBJ_FLAG_HIDDEN);
  updateAircraftPage();
}

void closeAircraftPage() {
  s_acPageOpen = false;
  photo::cancel();
  lv_obj_add_flag(s_acPage, LV_OBJ_FLAG_HIDDEN);
}

void updateAircraftPage() {
  if (!s_acPageOpen) return;
  const Aircraft* sel = selectedAircraft();
  const photo::Profile& p = photo::profile();

  // Airline logo arrives pre-sized (220x80, vector-rendered) — swap it in
  // for the monogram, 1:1, no transforms.
  if (photo::imageReady() && p.img && !s_acImgShown) {
    s_acImgShown = true;
    memset(&s_acImgDsc, 0, sizeof(s_acImgDsc));
    s_acImgDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_acImgDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_acImgDsc.header.w = p.imgW;
    s_acImgDsc.header.h = p.imgH;
    s_acImgDsc.header.stride = p.imgW * 2;
    s_acImgDsc.data = (const uint8_t*)p.img;
    s_acImgDsc.data_size = (uint32_t)p.imgW * p.imgH * 2;
    lv_image_set_src(s_acImg, &s_acImgDsc);
    lv_image_set_scale(s_acImg, 256);
    lv_obj_clear_flag(s_acImg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_acMono, LV_OBJ_FLAG_HIDDEN);
  }

  // Late logo: the route (and thus the airline code) often resolves only
  // after the page opens — request the logo when it lands. Also serves as
  // one retry if the initial fetch failed transiently.
  if (!s_acImgShown && sel && sel->routeState == RouteState::Found &&
      sel->route.airlineIata[0] &&
      strncmp(s_acLogoTried, sel->route.airlineIata,
              sizeof(s_acLogoTried)) != 0 &&
      photo::state() != photo::State::Pending) {
    strlcpy(s_acLogoTried, sel->route.airlineIata, sizeof(s_acLogoTried));
    photo::requestLogo(sel->route.airlineIata);
  }

  // Registration info as soon as adsbdb answers.
  if (photo::infoReady()) {
    char line3[64] = "";
    if (p.country[0] && sel && sel->squawk[0]) {
      snprintf(line3, sizeof(line3), "\n%s  •  squawk %s", p.country,
               sel->squawk);
    } else if (p.country[0]) {
      snprintf(line3, sizeof(line3), "\n%s", p.country);
    } else if (sel && sel->squawk[0]) {
      snprintf(line3, sizeof(line3), "\nsquawk %s", sel->squawk);
    }
    char info[160];
    snprintf(info, sizeof(info), "%s  •  %s\n%s%s",
             p.registration[0] ? p.registration : "?",
             p.typeDesc[0] ? p.typeDesc : (sel ? sel->type : ""), p.owner,
             line3);
    lv_label_set_text(s_acInfo, info);
  } else if (photo::state() == photo::State::Failed) {
    lv_label_set_text(s_acInfo, sel && sel->type[0] ? sel->type
                                                    : "no records found");
  } else if (photo::state() == photo::State::Ready && !photo::infoReady()) {
    lv_label_set_text(s_acInfo, "");
  }


  // Live numbers keep updating while the page is open.
  if (sel) {
    static const char* kCard[8] = {"N",  "NE", "E",  "SE",
                                   "S",  "SW", "W",  "NW"};
    int hdg = ((int)((sel->trackDeg + 22.5f) / 45.0f)) & 7;
    char live[96];
    char spd[16];
    fmtSpeed(sel->gsKt, spd, sizeof(spd));
    if (sel->altFt < 0) {
      snprintf(live, sizeof(live), "on ground  •  %s  •  %.1f mi", spd,
               sel->distNm * 1.15078f);
    } else {
      char alt[16];
      fmtAlt(sel->altFt, alt, sizeof(alt));
      const char* vr = sel->vertRateFpm > 300    ? "  climbing"
                       : sel->vertRateFpm < -300 ? "  descending"
                                                 : "";
      snprintf(live, sizeof(live), "%s  •  %s  •  %s%s\n%.1f mi away", alt,
               spd, kCard[hdg], vr, sel->distNm * 1.15078f);
    }
    lv_label_set_text(s_acLive, live);
  } else {
    lv_label_set_text(s_acLive, "out of range");
  }

  // ----- section layout (logo/monogram slot is a fixed 84 px) -----
  int t = 62 + 84 + 6;
  lv_obj_align(s_acDiv1, LV_ALIGN_TOP_MID, 0, t);
  lv_obj_align(s_acSec1, LV_ALIGN_TOP_MID, 0, t + 6);
  lv_obj_align(s_acInfo, LV_ALIGN_TOP_MID, 0, t + 22);
  int t2 = t + 22 + (gApp.textLarge.load() ? 88 : 76);
  lv_obj_align(s_acDiv2, LV_ALIGN_TOP_MID, 0, t2);
  lv_obj_align(s_acSec2, LV_ALIGN_TOP_MID, 0, t2 + 6);
  lv_obj_align(s_acLive, LV_ALIGN_TOP_MID, 0, t2 + 22);
}

// ---------- drawing helpers ----------

inline void drawLine(lv_layer_t* layer, float x1, float y1, float x2, float y2,
                     lv_color_t color, int16_t width, lv_opa_t opa,
                     int16_t dashW = 0, int16_t dashG = 0) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.p1.x = x1;
  dsc.p1.y = y1;
  dsc.p2.x = x2;
  dsc.p2.y = y2;
  dsc.color = color;
  dsc.width = width;
  dsc.opa = opa;
  dsc.dash_width = dashW;
  dsc.dash_gap = dashG;
  dsc.round_start = 1;
  dsc.round_end = 1;
  lv_draw_line(layer, &dsc);
}

inline void drawCircle(lv_layer_t* layer, float cx, float cy, int16_t r,
                       lv_color_t color, int16_t width, lv_opa_t opa) {
  lv_draw_arc_dsc_t dsc;
  lv_draw_arc_dsc_init(&dsc);
  dsc.center.x = (int32_t)cx;
  dsc.center.y = (int32_t)cy;
  dsc.radius = r;
  dsc.start_angle = 0;
  dsc.end_angle = 360;
  dsc.color = color;
  dsc.width = width;
  dsc.opa = opa;
  lv_draw_arc(layer, &dsc);
}

inline void drawText(lv_layer_t* layer, const char* txt, float x, float y,
                     const lv_font_t* font, lv_color_t color, lv_opa_t opa) {
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.text = txt;
  dsc.font = font;
  dsc.color = color;
  dsc.opa = opa;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  lv_area_t a;
  a.x1 = (int32_t)(x - 70);
  a.x2 = (int32_t)(x + 70);
  a.y1 = (int32_t)y;
  a.y2 = (int32_t)(y + 20);
  lv_draw_label(layer, &dsc, &a);
}

// ---- aircraft silhouettes (triangle lists, local coords, nose = -y) ----
struct P {
  float x, y;
};

// GA single (172-ish): slim fuselage, long straight wing, small tail.
const P kShapeProp[] = {
    {-1.2, -7}, {1.2, -7},  {1.2, 6},   {-1.2, -7}, {1.2, 6},   {-1.2, 6},
    {-10, -2},  {10, -2},   {10, 0.5},  {-10, -2},  {10, 0.5},  {-10, 0.5},
    {-4, 4.5},  {4, 4.5},   {4, 6},     {-4, 4.5},  {4, 6},     {-4, 6},
};
// Narrowbody jet (737/A320): slim fuselage, moderate sweep, engine pods
// tucked close to the body.
const P kShapeJet[] = {
    // nose cone + fuselage + tail cone
    {-1.6, -8},  {0, -12},     {1.6, -8},
    {-1.6, -8},  {1.6, -8},    {1.6, 9},
    {-1.6, -8},  {1.6, 9},     {-1.6, 9},
    {-1.6, 9},   {1.6, 9},     {0, 11},
    // wings (swept trapezoids)
    {-1.6, -2},  {-10.5, 3.8}, {-10.5, 5.6},
    {-1.6, -2},  {-10.5, 5.6}, {-1.6, 2.6},
    {1.6, -2},   {10.5, 3.8},  {10.5, 5.6},
    {1.6, -2},   {10.5, 5.6},  {1.6, 2.6},
    // engine pods (one per side, close in, poking ahead of the wing)
    {-5.4, -1.2}, {-3.8, -1.2}, {-3.8, 2.2},
    {-5.4, -1.2}, {-3.8, 2.2},  {-5.4, 2.2},
    {3.8, -1.2},  {5.4, -1.2},  {5.4, 2.2},
    {3.8, -1.2},  {5.4, 2.2},   {3.8, 2.2},
    // swept tailplane
    {-1.4, 6.8}, {-5.4, 9.4},  {-5.4, 10.6},
    {-1.4, 6.8}, {-5.4, 10.6}, {-1.4, 8.4},
    {1.4, 6.8},  {5.4, 9.4},   {5.4, 10.6},
    {1.4, 6.8},  {5.4, 10.6},  {1.4, 8.4},
};
// Widebody (777/787/A350): visibly wider + longer fuselage, deep wing sweep,
// big engine pods well outboard, heavier tailplane.
const P kShapeHeavy[] = {
    // nose cone + fuselage + tail cone
    {-2.2, -10}, {0, -14.5},   {2.2, -10},
    {-2.2, -10}, {2.2, -10},   {2.2, 11},
    {-2.2, -10}, {2.2, 11},    {-2.2, 11},
    {-2.2, 11},  {2.2, 11},    {0, 13.5},
    // wings — deeper chord and stronger sweep than the narrowbody
    {-2.2, -3.2}, {-14.5, 6.2}, {-14.5, 8.0},
    {-2.2, -3.2}, {-14.5, 8.0}, {-2.2, 3.4},
    {2.2, -3.2},  {14.5, 6.2},  {14.5, 8.0},
    {2.2, -3.2},  {14.5, 8.0},  {2.2, 3.4},
    // big engine pods, clearly outboard of the fuselage
    {-7.6, -0.6}, {-5.6, -0.6}, {-5.6, 3.6},
    {-7.6, -0.6}, {-5.6, 3.6},  {-7.6, 3.6},
    {5.6, -0.6},  {7.6, -0.6},  {7.6, 3.6},
    {5.6, -0.6},  {7.6, 3.6},   {5.6, 3.6},
    // tailplane
    {-2.0, 8.8}, {-7.6, 12.2}, {-7.6, 13.6},
    {-2.0, 8.8}, {-7.6, 13.6}, {-2.0, 10.6},
    {2.0, 8.8},  {7.6, 12.2},  {7.6, 13.6},
    {2.0, 8.8},  {7.6, 13.6},  {2.0, 10.6},
};

void drawPlaneMarker(lv_layer_t* layer, float x, float y, float trackDeg,
                     IconClass cls, lv_color_t color, bool selected) {
  float a = trackDeg * geo::kDeg2Rad;
  float ca = cosf(a), sa = sinf(a);
  float scale = 1.0f;
  const P* tris = kShapeJet;
  int nPts = sizeof(kShapeJet) / sizeof(P);
  switch (cls) {
    case IconClass::Prop:
      tris = kShapeProp;
      nPts = sizeof(kShapeProp) / sizeof(P);
      scale = 0.95f;
      break;
    case IconClass::Heavy:
      tris = kShapeHeavy;
      nPts = sizeof(kShapeHeavy) / sizeof(P);
      scale = 1.1f;  // shape is inherently larger; just a nudge
      break;
    case IconClass::Heli:
      nPts = 0;  // drawn procedurally below
      break;
    default:
      break;
  }

  auto rx = [&](float lx, float ly) {
    return x + (lx * ca - ly * sa) * scale;
  };
  auto ry = [&](float lx, float ly) {
    return y + (lx * sa + ly * ca) * scale;
  };

  for (int i = 0; i + 2 < nPts; i += 3) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.p[0].x = rx(tris[i].x, tris[i].y);
    dsc.p[0].y = ry(tris[i].x, tris[i].y);
    dsc.p[1].x = rx(tris[i + 1].x, tris[i + 1].y);
    dsc.p[1].y = ry(tris[i + 1].x, tris[i + 1].y);
    dsc.p[2].x = rx(tris[i + 2].x, tris[i + 2].y);
    dsc.p[2].y = ry(tris[i + 2].x, tris[i + 2].y);
    dsc.color = color;
    dsc.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &dsc);
  }

  if (cls == IconClass::Heli) {
    // Cabin dot + tail boom + rotor X.
    drawCircle(layer, x, y, 3, color, 4, LV_OPA_COVER);
    drawLine(layer, x, y, rx(0, 8), ry(0, 8), color, 2, LV_OPA_COVER);
    drawLine(layer, rx(-6, -6), ry(-6, -6), rx(6, 6), ry(6, 6), color, 2,
             LV_OPA_COVER);
    drawLine(layer, rx(6, -6), ry(6, -6), rx(-6, 6), ry(-6, 6), color, 2,
             LV_OPA_COVER);
  }

  if (selected) drawCircle(layer, x, y, 17, kColSel, 2, LV_OPA_COVER);
}

// Per-frame cached center in world pixels (set at the top of the draw cb).
double s_cwx = 0, s_cwy = 0;
uint8_t s_zCur = 10;
float s_kCur = 1.0f;  // oversample factor (widest level is capped at 50 mi)

// lat/lon -> screen px via web mercator (pixel-exact over the map tiles).
bool toScreen(float lat, float lon, float& x, float& y) {
  double wx, wy;
  geo::latLonToWorldPx(lat, lon, s_zCur, wx, wy);
  x = (float)((wx - s_cwx) / s_kCur) + kCx;
  y = (float)((wy - s_cwy) / s_kCur) + kCy;
  return (x > -120 && x < cfg::kScreenW + 120 && y > -120 &&
          y < cfg::kScreenH + 120);
}

const Aircraft* selectedAircraft() {
  if (!s_selHex[0]) return nullptr;
  for (int i = 0; i < s_snap.count; i++) {
    if (strncmp(s_snap.ac[i].hex, s_selHex, sizeof(s_selHex)) == 0)
      return &s_snap.ac[i];
  }
  return nullptr;
}

// Re-style the static labels after a text-size change (draw-callback text
// picks the fonts up automatically on the next frame).
void applyTextSize() {
  lv_obj_set_style_text_font(s_topLabel, fMed(), 0);
  lv_obj_set_style_text_font(s_subLabel, fBody(), 0);
  lv_obj_set_style_text_font(s_rangeLabel, fMed(), 0);
  lv_obj_set_style_text_font(s_panelL1, fMed(), 0);
  lv_obj_set_style_text_font(s_panelL2, fMed(), 0);
  lv_obj_set_style_text_font(s_panelL3, fBody(), 0);
  lv_obj_set_style_text_font(s_acRoute, fMed(), 0);
  // Bigger reading text on the aircraft page (photo removal freed room).
  lv_obj_set_style_text_font(s_acInfo, fMed(), 0);
  lv_obj_set_style_text_font(s_acLive, fMed(), 0);
}

// ---------- tutorial ----------

void tutorialShowStep() {
  const TutStep& st = kTutSteps[s_tutIdx];
  lv_label_set_text(s_tutTitle, st.title);
  lv_label_set_text(s_tutBody, st.body);
  lv_label_set_text_fmt(s_tutStep, "%d / %d  •  tap: next  •  dial: back",
                        (int)(s_tutIdx + 1), (int)kTutCount);
}

void openTutorial() {
  s_tutOpen = true;
  s_tutIdx = 0;
  tutorialShowStep();
  lv_obj_clear_flag(s_tutPage, LV_OBJ_FLAG_HIDDEN);
}

void closeTutorial() {
  s_tutOpen = false;
  lv_obj_add_flag(s_tutPage, LV_OBJ_FLAG_HIDDEN);
  if (!s_tutSeen) {
    s_tutSeen = true;
    Preferences p;
    p.begin("ui", false);
    p.putBool("tut", true);
    p.end();
  }
  lv_obj_invalidate(s_radar);
}

void tutorialNext() {
  if (s_tutIdx + 1 >= kTutCount) {
    closeTutorial();
  } else {
    s_tutIdx++;
    tutorialShowStep();
  }
}

void tutorialPrev() {
  if (s_tutIdx > 0) {
    s_tutIdx--;
    tutorialShowStep();
  }
}

// ---------- settings page ----------

void showSetPage() {
  for (int i = 0; i < 2; i++) {
    if (!s_setPg[i]) continue;
    if (i == s_setPgIdx) lv_obj_clear_flag(s_setPg[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_setPg[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (s_setTitle)
    lv_label_set_text_fmt(s_setTitle, "SETTINGS  %d/2", s_setPgIdx + 1);
}

void refreshSettingsLabels() {
  char flt[5];
  if (gApp.getCallsignFilter(flt)) {
    lv_label_set_text_fmt(s_setFltLbl, "Planes: %s only", flt);
  } else {
    lv_label_set_text(s_setFltLbl, "Planes: all");
  }
  lv_label_set_text_fmt(s_setTxtLbl, "Text size: %s",
                        gApp.textLarge.load() ? "large" : "normal");
  static const char* kUnitNames[3] = {"ft & kt", "ft & mph", "m & km/h"};
  uint8_t preset = gApp.altUnit.load() == 1 ? 2
                   : gApp.spdUnit.load() == 1 ? 1
                                              : 0;
  lv_label_set_text_fmt(s_setUnitLbl, "Units: %s", kUnitNames[preset]);
  lv_label_set_text(s_setWifiLbl, "Reset WiFi");
  char routeLine[48];
  if (routes::aeroActive()) {
    snprintf(routeLine, sizeof(routeLine), "routes: FlightAware (%lu/mo)",
             (unsigned long)routes::aeroUsedThisMonth());
  } else {
    snprintf(routeLine, sizeof(routeLine), "routes: basic — upgrade on web");
  }
  char pass[9];
  devicePassword(pass);
  char info[208];
  snprintf(info, sizeof(info),
           "v%s  •  %s\nloc: %s  •  %.4f, %.4f\n%s\nweb password: %s",
           cfg::kFwVersion,
           gApp.wifiUp.load() ? WiFi.localIP().toString().c_str() : "offline",
           locSourceName(gApp.locSource.load()), gApp.centerLat.load(),
           gApp.centerLon.load(), routeLine, pass);
  lv_label_set_text(s_setInfoLbl, info);
}

void openSettings() {
  s_setOpen = true;
  s_wifiConfirmMs = 0;
  s_setPgIdx = 0;
  showSetPage();
  refreshSettingsLabels();
  lv_obj_clear_flag(s_setPage, LV_OBJ_FLAG_HIDDEN);
}

void closeSettings() {
  s_setOpen = false;
  lv_obj_add_flag(s_setPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(s_radar);
}

// ---------- nearby list (custom drawn) ----------

void openNearby() {
  s_nearOpen = true;
  lv_obj_clear_flag(s_nearPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(s_nearPage);
}

void closeNearby() {
  s_nearOpen = false;
  lv_obj_add_flag(s_nearPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(s_radar);
}

void nearbyDrawCb(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);

  drawText(layer, "NEARBY", kCx, 16, fMed(), kColText, LV_OPA_COVER);

  // Pick the kNearRows nearest aircraft (selection sort on a copy of idx).
  int idx[cfg::kMaxAircraft];
  int n = 0;
  for (int i = 0; i < s_snap.count; i++) idx[n++] = i;
  for (int r = 0; r < kNearRows && r < n; r++) {
    int best = r;
    for (int j = r + 1; j < n; j++) {
      if (s_snap.ac[idx[j]].distNm < s_snap.ac[idx[best]].distNm) best = j;
    }
    int t = idx[r];
    idx[r] = idx[best];
    idx[best] = t;
  }

  float cLat = gApp.centerLat.load();
  float cLon = gApp.centerLon.load();
  static const char* kCardinals[8] = {"N",  "NE", "E",  "SE",
                                      "S",  "SW", "W",  "NW"};

  for (int r = 0; r < kNearRows; r++) {
    s_nearRowIdx[r] = (r < n) ? idx[r] : -1;
    if (r >= n) continue;
    const Aircraft& a = s_snap.ac[idx[r]];
    int y = kNearTopY + r * kNearRowH;
    // Rows on a circle: indent the outermost rows a touch.
    float rowMid = y + kNearRowH / 2.0f;
    float half = sqrtf(max(1.0f, (float)cfg::kScreenR * cfg::kScreenR -
                                     (rowMid - kCy) * (rowMid - kCy)));
    float left = kCx - half + 26;
    float right = kCx + half - 30;

    // Direction arrow (bearing from the user, north-up).
    float brg = geo::bearingDeg(cLat, cLon, a.lat, a.lon);
    float ang = brg * geo::kDeg2Rad;
    float ax = left + 10, ay = rowMid - 4;
    float ca = cosf(ang), sa = sinf(ang);
    lv_draw_triangle_dsc_t td;
    lv_draw_triangle_dsc_init(&td);
    auto rx = [&](float lx, float ly) { return ax + lx * ca - ly * sa; };
    auto ry = [&](float lx, float ly) { return ay + lx * sa + ly * ca; };
    td.p[0].x = rx(0, -9);
    td.p[0].y = ry(0, -9);
    td.p[1].x = rx(-5.5f, 6);
    td.p[1].y = ry(-5.5f, 6);
    td.p[2].x = rx(5.5f, 6);
    td.p[2].y = ry(5.5f, 6);
    td.color = (gApp.airlineColors.load() && a.isCommercial())
                   ? lv_color_hex(kPlaneColors[gApp.colComm.load()].hex)
                   : lv_color_hex(kPlaneColors[gApp.colPriv.load()].hex);
    td.opa = LV_OPA_COVER;
    lv_draw_triangle(layer, &td);
    int card = ((int)((brg + 22.5f) / 45.0f)) & 7;
    drawText(layer, kCardinals[card], ax, ay + 9, fSm(), kColTextDim,
             LV_OPA_COVER);

    // Line 1: route when known, else callsign — colored like the map blip.
    char name[16];
    bool hasRoute = a.routeState == RouteState::Found;
    if (hasRoute) {
      snprintf(name, sizeof(name), "%s > %s", a.route.origIata,
               a.route.destIata);
    } else {
      strlcpy(name, a.callsign[0] ? a.callsign : a.hex, sizeof(name));
    }
    lv_color_t rowCol =
        a.isEmergency()
            ? kColEmerg
            : (gApp.airlineColors.load() && a.isCommercial())
                  ? lv_color_hex(kPlaneColors[gApp.colComm.load()].hex)
                  : lv_color_hex(kPlaneColors[gApp.colPriv.load()].hex);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.text = name;
    ld.font = fMed();
    ld.color = rowCol;
    lv_area_t na = {(int32_t)(left + 28), (int32_t)(rowMid - 19),
                    (int32_t)(right - 62), (int32_t)(rowMid + 1)};
    lv_draw_label(layer, &ld, &na);

    // Line 2: callsign (when line 1 is the route) + type + altitude.
    char detail[36] = {0};
    int dn = 0;
    if (hasRoute && a.callsign[0]) {
      dn = snprintf(detail, sizeof(detail), "%s", a.callsign);
    }
    if (a.type[0]) {
      dn += snprintf(detail + dn, sizeof(detail) - dn, "%s%s",
                     dn ? "  •  " : "", a.type);
    }
    if (a.altFt >= 0) {
      char alt[14];
      fmtAlt(a.altFt, alt, sizeof(alt));
      dn += snprintf(detail + dn, sizeof(detail) - dn, "%s%s",
                     dn ? "  •  " : "", alt);
    } else if (dn < (int)sizeof(detail) - 12) {
      dn += snprintf(detail + dn, sizeof(detail) - dn, "%sground",
                     dn ? "  •  " : "");
    }
    lv_draw_label_dsc_t l2;
    lv_draw_label_dsc_init(&l2);
    l2.text = detail;
    l2.font = fSm();
    l2.color = kColTextDim;
    lv_area_t d2 = {(int32_t)(left + 28), (int32_t)(rowMid + 3),
                    (int32_t)(right - 54), (int32_t)(rowMid + 19)};
    lv_draw_label(layer, &l2, &d2);

    // Right column: distance over speed.
    char dist[12];
    snprintf(dist, sizeof(dist), "%.1f mi", a.distNm * 1.15078f);
    lv_draw_label_dsc_t dd;
    lv_draw_label_dsc_init(&dd);
    dd.text = dist;
    dd.font = fBody();
    dd.color = kColText;
    dd.align = LV_TEXT_ALIGN_RIGHT;
    lv_area_t da = {(int32_t)(right - 70), (int32_t)(rowMid - 17),
                    (int32_t)right, (int32_t)(rowMid + 1)};
    lv_draw_label(layer, &dd, &da);
    if (a.gsKt > 0) {
      char spd[14];
      fmtSpeed(a.gsKt, spd, sizeof(spd));
      lv_draw_label_dsc_t sd;
      lv_draw_label_dsc_init(&sd);
      sd.text = spd;
      sd.font = fSm();
      sd.color = kColTextDim;
      sd.align = LV_TEXT_ALIGN_RIGHT;
      lv_area_t sa = {(int32_t)(right - 70), (int32_t)(rowMid + 3),
                      (int32_t)right, (int32_t)(rowMid + 19)};
      lv_draw_label(layer, &sd, &sa);
    }

    if (r > 0) {
      drawLine(layer, left, (float)y, right, (float)y, kColGrid, 1, LV_OPA_50);
    }
  }
  if (n == 0) {
    drawText(layer, "no aircraft", kCx, kCy - 8, &lv_font_montserrat_14,
             kColTextDim, LV_OPA_COVER);
  }
}

void nearbyClickCb(lv_event_t*) {
  if (s_swiped) return;  // this release was a swipe, not a tap
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  int r = (p.y - kNearTopY) / kNearRowH;
  if (r >= 0 && r < kNearRows && s_nearRowIdx[r] >= 0 &&
      s_nearRowIdx[r] < s_snap.count) {
    strlcpy(s_selHex, s_snap.ac[s_nearRowIdx[r]].hex, sizeof(s_selHex));
    gAircraft.setPriorityHex(s_selHex);
    closeNearby();
    updateDetailPanel();
  }
}

// ---------- main draw callback ----------

void radarDrawCb(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);

  // Cache projection center for this frame.
  s_zCur = gApp.zoomZ();
  s_kCur = gApp.zoomK();
  geo::latLonToWorldPx(gApp.centerLat.load(), gApp.centerLon.load(), s_zCur,
                       s_cwx, s_cwy);

  // Street-map underlay (slightly dimmed so the overlay pops).
  const uint16_t* map = maplayer::canvas();
  if (map) {
    static lv_image_dsc_t imgDsc;
    imgDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    imgDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    imgDsc.header.w = cfg::kScreenW;
    imgDsc.header.h = cfg::kScreenH;
    imgDsc.header.stride = cfg::kScreenW * 2;
    imgDsc.data = (const uint8_t*)map;
    imgDsc.data_size = cfg::kScreenW * cfg::kScreenH * 2;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &imgDsc;
    d.opa = LV_OPA_COVER;  // tiles are pre-dimmed at fetch — opaque = fast copy
    lv_area_t a = {0, 0, cfg::kScreenW - 1, cfg::kScreenH - 1};
    lv_draw_image(layer, &d, &a);
  }

  // Range rings + cross hairs.
  drawCircle(layer, kCx, kCy, cfg::kScreenR - 2, kColGridBright, 2,
             LV_OPA_COVER);
  drawCircle(layer, kCx, kCy, (int16_t)(cfg::kScreenR * 2 / 3), kColGrid, 1,
             map ? LV_OPA_40 : LV_OPA_COVER);
  drawCircle(layer, kCx, kCy, (int16_t)(cfg::kScreenR / 3), kColGrid, 1,
             map ? LV_OPA_40 : LV_OPA_COVER);
  if (!map) {
    drawLine(layer, kCx - cfg::kScreenR, kCy, kCx + cfg::kScreenR, kCy,
             kColGrid, 1, LV_OPA_60);
    drawLine(layer, kCx, kCy - cfg::kScreenR, kCx, kCy + cfg::kScreenR,
             kColGrid, 1, LV_OPA_60);
  }
  // Center dot ("you are here").
  drawCircle(layer, kCx, kCy, 3, kColSweep, 3, LV_OPA_COVER);

  // Airport callouts, revealed by zoom: majors always; regionals from 15 mi
  // (codes at 7); small/GA fields from 7 mi (codes at 3).
  {
    uint8_t zi = gApp.zoomIdx.load();
    uint8_t minSz = (zi >= 2) ? 0 : (zi >= 1) ? 1 : 2;
    float cLat = gApp.centerLat.load();
    float cLon = gApp.centerLon.load();
    float rMi = gApp.radiusMi();
    float latSpan = rMi / 69.0f * 1.35f;
    float lonSpan = rMi / (69.0f * cosf(cLat * geo::kDeg2Rad)) * 1.35f;
    for (int i = 0; i < kAirportCount; i++) {
      const AirportRec& ap = kAirports[i];
      if (ap.sz < minSz) continue;
      if (fabsf(ap.lat - cLat) > latSpan) continue;
      float dLon = fabsf(ap.lon - cLon);
      if (dLon > 180) dLon = 360 - dLon;
      if (dLon > lonSpan) continue;
      float x, y;
      if (!toScreen(ap.lat, ap.lon, x, y)) continue;
      bool code = (ap.sz == 2) || (ap.sz == 1 && zi >= 2) || (zi >= 3);
      lv_opa_t opa = ap.sz == 2 ? LV_OPA_80 : ap.sz == 1 ? LV_OPA_60
                                                         : LV_OPA_40;
      drawCircle(layer, x, y, ap.sz == 2 ? 4 : 2, kColAirport, 2, opa);
      if (code) {
        drawText(layer, ap.code, x, y + (ap.sz == 2 ? 6 : 5), fSm(),
                 kColAirport, opa);
      }
    }
  }

  const Aircraft* sel = selectedAircraft();

  // Route lines for the selected aircraft: bright dashes ahead to the
  // destination, dim dashes back to the origin (the leg already flown).
  if (sel && sel->routeState == RouteState::Found) {
    struct Leg {
      float toLat, toLon;
      lv_opa_t opa;
    } legs[2] = {
        {sel->route.destLat, sel->route.destLon, LV_OPA_COVER},
        {sel->route.origLat, sel->route.origLon, LV_OPA_40},
    };
    for (const Leg& leg : legs) {
      if (leg.toLat == 0 && leg.toLon == 0) continue;
      float px, py;
      toScreen(sel->lat, sel->lon, px, py);
      float prevX = px, prevY = py;
      const int kSegs = 30;
      for (int s = 1; s <= kSegs; s++) {
        float la, lo, x, y;
        geo::greatCirclePoint(sel->lat, sel->lon, leg.toLat, leg.toLon,
                              (float)s / kSegs, la, lo);
        toScreen(la, lo, x, y);
        if (s % 2) {  // dashed: draw odd segments only
          drawLine(layer, prevX, prevY, x, y, kColRoute, 2, leg.opa);
        }
        prevX = x;
        prevY = y;
        // Stop once well offscreen — no point projecting to the far airport.
        if (x < -400 || x > cfg::kScreenW + 400 || y < -400 ||
            y > cfg::kScreenH + 400)
          break;
      }
    }
  }

  // Aircraft: trails, markers, labels.
  for (int i = 0; i < s_snap.count; i++) {
    const Aircraft& a = s_snap.ac[i];
    float x, y;
    s_onScreen[i] = toScreen(a.lat, a.lon, x, y);
    s_px[i] = x;
    s_py[i] = y;
    if (!s_onScreen[i]) continue;

    // Trail: opacity from segment age — fresh is bright, gone at 30 s.
    if (a.trailCount > 1) {
      uint32_t now = millis();
      float px = 0, py = 0;
      bool have = false;
      for (int t = 0; t < a.trailCount; t++) {
        uint8_t idx = (a.trailHead + cfg::kTrailLen - a.trailCount + t) %
                      cfg::kTrailLen;
        float tx, ty;
        toScreen(a.trail[idx].lat, a.trail[idx].lon, tx, ty);
        if (have) {
          uint32_t age = now - a.trail[idx].ms;  // newer end of the segment
          if (age < cfg::kTrailFadeMs) {
            lv_opa_t opa = (lv_opa_t)(20 + 140 * (cfg::kTrailFadeMs - age) /
                                               cfg::kTrailFadeMs);
            drawLine(layer, px, py, tx, ty, kColTrail, 2, opa);
          }
        }
        px = tx;
        py = ty;
        have = true;
      }
    }

    bool isSel = sel == &a;
    bool onGround = a.altFt < 0;
    lv_color_t privCol = lv_color_hex(kPlaneColors[gApp.colPriv.load()].hex);
    lv_color_t commCol = lv_color_hex(kPlaneColors[gApp.colComm.load()].hex);
    lv_color_t col = a.isEmergency() ? kColEmerg
                     : isSel         ? kColSel
                     : onGround      ? kColPlaneGnd
                     : (gApp.airlineColors.load() && a.isCommercial())
                         ? commCol
                         : privCol;

    // Velocity vector: where the plane will be in ~60 s (ATC-style).
    if (!onGround && a.gsKt > 40) {
      float nmPerPx = (gApp.radiusMi() / 1.15078f) / (float)cfg::kScreenR;
      float lenPx = (a.gsKt / 60.0f) / nmPerPx;
      lenPx = constrain(lenPx, 12.0f, 42.0f);
      float ta = a.trackDeg * geo::kDeg2Rad;
      drawLine(layer, x, y, x + lenPx * sinf(ta), y - lenPx * cosf(ta), col, 1,
               LV_OPA_50);
    }

    drawPlaneMarker(layer, x, y, a.trackDeg, a.iconClass, col, isSel);
    if (a.isEmergency()) {
      // Squawking 7500/7600/7700 — ring it so it can't be missed.
      drawCircle(layer, x, y, 20, kColEmerg, 2, LV_OPA_COVER);
    }

    // Label: route ("DAL123" -> "ATL>DFW") when known, else callsign.
    char label[16] = {0};
    if (a.routeState == RouteState::Found) {
      snprintf(label, sizeof(label), "%s>%s", a.route.origIata,
               a.route.destIata);
    } else if (a.callsign[0]) {
      strlcpy(label, a.callsign, sizeof(label));
    }
    float labelY = y + 14;
    if (label[0]) {
      drawText(layer, label, x, labelY, fBody(), isSel ? kColSel : kColText,
               LV_OPA_COVER);
      labelY += gApp.textLarge.load() ? 16 : 14;
    }

    // Altitude line ("34k ft" / "10.4k m" per unit setting). At the two
    // closest zooms there's room for speed too.
    if (!onGround) {
      char altTxt[28];
      fmtAlt(a.altFt, altTxt, sizeof(altTxt));
      if (gApp.zoomIdx.load() >= 2 && a.gsKt > 0) {
        size_t len = strlen(altTxt);
        altTxt[len++] = ' ';
        fmtSpeed(a.gsKt, altTxt + len, sizeof(altTxt) - len);
      }
      drawText(layer, altTxt, x, labelY, fSm(), kColTextDim, LV_OPA_COVER);
    }
  }
}

// ---------- interaction ----------

void radarClickCb(lv_event_t* e) {
  if (s_swiped) return;  // this release was a swipe, not a tap
  lv_indev_t* indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  // Nearest on-screen aircraft within 36 px.
  float bestD = 36 * 36;
  int best = -1;
  for (int i = 0; i < s_snap.count; i++) {
    if (!s_onScreen[i]) continue;
    float dx = s_px[i] - p.x, dy = s_py[i] - p.y;
    float d = dx * dx + dy * dy;
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  if (best >= 0) {
    strlcpy(s_selHex, s_snap.ac[best].hex, sizeof(s_selHex));
  } else {
    s_selHex[0] = '\0';  // tap on empty space deselects
  }
  gAircraft.setPriorityHex(s_selHex);  // selected plane's route jumps the queue
  updateDetailPanel();
  lv_obj_invalidate(s_radar);
}

void updateDetailPanel() {
  const Aircraft* sel = selectedAircraft();
  if (!sel) {
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

  // Airline logo chip (cached; appears on the next refresh after fetch).
  bool logoShown = false;
  if (sel->routeState == RouteState::Found && sel->route.airlineIata[0]) {
    logos::Logo lg;
    if (logos::get(sel->route.airlineIata, &lg)) {
      memset(&s_panelLogoDsc, 0, sizeof(s_panelLogoDsc));
      s_panelLogoDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
      s_panelLogoDsc.header.cf = LV_COLOR_FORMAT_RGB565;
      s_panelLogoDsc.header.w = lg.w;
      s_panelLogoDsc.header.h = lg.h;
      s_panelLogoDsc.header.stride = lg.w * 2;
      s_panelLogoDsc.data = (const uint8_t*)lg.buf;
      s_panelLogoDsc.data_size = (uint32_t)lg.w * lg.h * 2;
      lv_image_set_src(s_panelLogo, &s_panelLogoDsc);
      lv_obj_clear_flag(s_panelLogo, LV_OBJ_FLAG_HIDDEN);
      logoShown = true;
    }
  }
  if (!logoShown) lv_obj_add_flag(s_panelLogo, LV_OBJ_FLAG_HIDDEN);

  char l1[48];
  snprintf(l1, sizeof(l1), "%s  %s", sel->callsign[0] ? sel->callsign : "----",
           sel->type[0] ? sel->type : "");
  lv_label_set_text(s_panelL1, l1);

  char l2[64];
  if (sel->routeState == RouteState::Found) {
    snprintf(l2, sizeof(l2), "%s > %s", sel->route.origIata,
             sel->route.destIata);
  } else if (sel->routeState == RouteState::None) {
    strlcpy(l2, "no route info", sizeof(l2));
  } else {
    strlcpy(l2, "route...", sizeof(l2));
  }
  lv_label_set_text(s_panelL2, l2);

  char l3[80];
  char spd[16];
  fmtSpeed(sel->gsKt, spd, sizeof(spd));
  if (sel->altFt < 0) {
    snprintf(l3, sizeof(l3), "on ground  •  %s  •  %.1f mi", spd,
             sel->distNm * 1.15078f);
  } else {
    char alt[16];
    fmtAlt(sel->altFt, alt, sizeof(alt));
    const char* vr = sel->vertRateFpm > 300    ? " ^"
                     : sel->vertRateFpm < -300 ? " v"
                                               : "";
    snprintf(l3, sizeof(l3), "%s%s  •  %s  •  %.1f mi", alt, vr, spd,
             sel->distNm * 1.15078f);
  }
  lv_label_set_text(s_panelL3, l3);
}

void updateStatusText() {
  NetPhase phase = gApp.netPhase.load();
  if (phase != NetPhase::Running) {
    lv_obj_clear_flag(s_bootLabel, LV_OBJ_FLAG_HIDDEN);
    switch (phase) {
      case NetPhase::Portal: {
        char pass[9];
        devicePassword(pass);
        lv_label_set_text_fmt(
            s_bootLabel,
            "Setup: join WiFi\n\"%s\"\npassword %s\nthen open 192.168.4.1",
            cfg::kSetupApName, pass);
        break;
      }
      case NetPhase::Connecting:
        lv_label_set_text(s_bootLabel, "Connecting\nWiFi...");
        break;
      case NetPhase::Locating:
        lv_label_set_text(s_bootLabel, "Finding\nlocation...");
        break;
      default:
        lv_label_set_text(s_bootLabel, "Starting...");
    }
  } else {
    lv_obj_add_flag(s_bootLabel, LV_OBJ_FLAG_HIDDEN);
  }

  lv_label_set_text_fmt(s_topLabel, "%d aircraft", (int)s_snap.count);
  uint32_t ageS = s_snap.lastUpdateMs
                      ? (millis() - s_snap.lastUpdateMs) / 1000
                      : 999;
  // Surface API trouble instead of silently thinning out the sky.
  char apiNote[24] = "";
  if (phase == NetPhase::Running && (!s_snap.apiOk || ageS > 15)) {
    int code = gApp.adsbLastCode.load();
    if (code == 429) {
      strlcpy(apiNote, "  •  rate limited", sizeof(apiNote));
    } else if (code > 0 && code != 200) {
      snprintf(apiNote, sizeof(apiNote), "  •  api err %d", code);
    } else {
      snprintf(apiNote, sizeof(apiNote), "  •  no data %lus",
               (unsigned long)min(ageS, (uint32_t)999));
    }
  }
  char flt[5];
  char fltNote[16] = "";
  if (gApp.getCallsignFilter(flt)) {
    snprintf(fltNote, sizeof(fltNote), "%s only  •  ", flt);
  }
  lv_label_set_text_fmt(s_subLabel, "%sloc: %s%s", fltNote,
                        locSourceName(gApp.locSource.load()), apiNote);
  // LVGL's built-in printf has no float support — format with libc.
  char rangeTxt[16];
  snprintf(rangeTxt, sizeof(rangeTxt), "%.0f mi", gApp.radiusMi());
  lv_label_set_text(s_rangeLabel, rangeTxt);
}

}  // namespace

void init() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColBg, 0);

  s_radar = lv_obj_create(scr);
  lv_obj_remove_style_all(s_radar);
  lv_obj_set_size(s_radar, cfg::kScreenW, cfg::kScreenH);
  lv_obj_set_pos(s_radar, 0, 0);
  lv_obj_add_flag(s_radar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_radar, radarDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(s_radar, radarClickCb, LV_EVENT_CLICKED, nullptr);

  s_topLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(s_topLabel, kColText, 0);
  lv_obj_set_style_text_font(s_topLabel, &lv_font_montserrat_16, 0);
  lv_obj_align(s_topLabel, LV_ALIGN_TOP_MID, 0, 34);
  lv_label_set_text(s_topLabel, "");

  s_subLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(s_subLabel, kColTextDim, 0);
  lv_obj_set_style_text_font(s_subLabel, &lv_font_montserrat_12, 0);
  lv_obj_align(s_subLabel, LV_ALIGN_TOP_MID, 0, 56);
  lv_label_set_text(s_subLabel, "");

  s_rangeLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(s_rangeLabel, kColText, 0);
  lv_obj_set_style_text_font(s_rangeLabel, &lv_font_montserrat_16, 0);
  lv_obj_align(s_rangeLabel, LV_ALIGN_BOTTOM_MID, 0, -30);
  char rangeTxt[16];
  snprintf(rangeTxt, sizeof(rangeTxt), "%.0f mi", gApp.radiusMi());
  lv_label_set_text(s_rangeLabel, rangeTxt);

  s_bootLabel = lv_label_create(scr);
  lv_obj_set_style_text_color(s_bootLabel, kColSweep, 0);
  lv_obj_set_style_text_font(s_bootLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(s_bootLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_bootLabel, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(s_bootLabel, "Starting...");

  // Detail panel (hidden until a plane is tapped).
  s_panel = lv_obj_create(scr);
  lv_obj_remove_style_all(s_panel);
  lv_obj_set_size(s_panel, 260, 96);
  lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, -52);
  lv_obj_set_style_bg_color(s_panel, kColPanelBg, 0);
  lv_obj_set_style_bg_opa(s_panel, LV_OPA_80, 0);
  lv_obj_set_style_radius(s_panel, 14, 0);
  lv_obj_set_style_border_color(s_panel, kColGridBright, 0);
  lv_obj_set_style_border_width(s_panel, 1, 0);
  lv_obj_set_style_pad_all(s_panel, 10, 0);
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

  s_panelLogo = lv_image_create(s_panel);
  lv_obj_align(s_panelLogo, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(s_panelLogo, LV_OBJ_FLAG_HIDDEN);

  s_panelL1 = lv_label_create(s_panel);
  lv_obj_set_style_text_color(s_panelL1, kColSel, 0);
  lv_obj_set_style_text_font(s_panelL1, &lv_font_montserrat_16, 0);
  lv_obj_align(s_panelL1, LV_ALIGN_TOP_MID, 0, 0);

  s_panelL2 = lv_label_create(s_panel);
  lv_obj_set_style_text_color(s_panelL2, kColText, 0);
  lv_obj_set_style_text_font(s_panelL2, &lv_font_montserrat_16, 0);
  lv_obj_align(s_panelL2, LV_ALIGN_TOP_MID, 0, 24);

  s_panelL3 = lv_label_create(s_panel);
  lv_obj_set_style_text_color(s_panelL3, kColTextDim, 0);
  lv_obj_set_style_text_font(s_panelL3, &lv_font_montserrat_12, 0);
  lv_obj_align(s_panelL3, LV_ALIGN_TOP_MID, 0, 50);

  // Tap the panel -> full aircraft page.
  lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      s_panel, [](lv_event_t*) { openAircraftPage(); }, LV_EVENT_CLICKED,
      nullptr);

  // Full-screen aircraft page (photo + registration + live data).
  s_acPage = lv_obj_create(scr);
  lv_obj_remove_style_all(s_acPage);
  lv_obj_set_size(s_acPage, cfg::kScreenW, cfg::kScreenH);
  lv_obj_set_pos(s_acPage, 0, 0);
  lv_obj_set_style_bg_color(s_acPage, kColBg, 0);
  lv_obj_set_style_bg_opa(s_acPage, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_acPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_acPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      s_acPage, [](lv_event_t*) { closeAircraftPage(); }, LV_EVENT_CLICKED,
      nullptr);

  s_acTitle = lv_label_create(s_acPage);
  lv_obj_set_style_text_color(s_acTitle, kColSel, 0);
  lv_obj_set_style_text_font(s_acTitle, &lv_font_montserrat_20, 0);
  lv_obj_align(s_acTitle, LV_ALIGN_TOP_MID, 0, 8);

  s_acRoute = lv_label_create(s_acPage);
  lv_obj_set_style_text_color(s_acRoute, kColText, 0);
  lv_obj_set_style_text_font(s_acRoute, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(s_acRoute, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_acRoute, LV_ALIGN_TOP_MID, 0, 36);

  s_acImg = lv_image_create(s_acPage);
  lv_obj_align(s_acImg, LV_ALIGN_TOP_MID, 0, 64);
  lv_image_set_pivot(s_acImg, 0, 0);
  lv_obj_add_flag(s_acImg, LV_OBJ_FLAG_HIDDEN);

  // Monogram roundel — the offline-safe "logo" (airline code / callsign).
  s_acMono = lv_obj_create(s_acPage);
  lv_obj_remove_style_all(s_acMono);
  lv_obj_set_size(s_acMono, 76, 76);
  lv_obj_align(s_acMono, LV_ALIGN_TOP_MID, 0, 64);
  lv_obj_set_style_radius(s_acMono, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_acMono, kColCardBg, 0);
  lv_obj_set_style_bg_opa(s_acMono, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_acMono, 3, 0);
  lv_obj_set_style_border_color(s_acMono, kColGridBright, 0);
  lv_obj_add_flag(s_acMono, LV_OBJ_FLAG_HIDDEN);
  s_acMonoLbl = lv_label_create(s_acMono);
  lv_obj_set_style_text_color(s_acMonoLbl, kColText, 0);
  lv_obj_set_style_text_font(s_acMonoLbl, &lv_font_montserrat_24, 0);
  lv_obj_center(s_acMonoLbl);

  auto makeDivider = [&]() -> lv_obj_t* {
    lv_obj_t* d = lv_obj_create(s_acPage);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 240, 1);
    lv_obj_set_style_bg_color(d, kColGridBright, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
  };
  auto makeSectionHdr = [&](const char* txt) -> lv_obj_t* {
    lv_obj_t* l = lv_label_create(s_acPage);
    lv_obj_set_style_text_color(l, kColAirport, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_10, 0);
    lv_label_set_text(l, txt);
    return l;
  };
  s_acDiv1 = makeDivider();
  s_acSec1 = makeSectionHdr("AIRCRAFT");
  s_acDiv2 = makeDivider();
  s_acSec2 = makeSectionHdr("LIVE");

  s_acInfo = lv_label_create(s_acPage);
  lv_obj_set_style_text_color(s_acInfo, kColTextDim, 0);
  lv_obj_set_style_text_font(s_acInfo, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(s_acInfo, LV_TEXT_ALIGN_CENTER, 0);

  s_acLive = lv_label_create(s_acPage);
  lv_obj_set_style_text_color(s_acLive, kColText, 0);
  lv_obj_set_style_text_font(s_acLive, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(s_acLive, LV_TEXT_ALIGN_CENTER, 0);

  // ----- settings page (swipe up from the radar) -----
  s_setPage = lv_obj_create(scr);
  lv_obj_remove_style_all(s_setPage);
  lv_obj_set_size(s_setPage, cfg::kScreenW, cfg::kScreenH);
  lv_obj_set_pos(s_setPage, 0, 0);
  lv_obj_set_style_bg_color(s_setPage, kColBg, 0);
  lv_obj_set_style_bg_opa(s_setPage, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_setPage, LV_OBJ_FLAG_HIDDEN);

  s_setTitle = lv_label_create(s_setPage);
  lv_obj_set_style_text_color(s_setTitle, kColText, 0);
  lv_obj_set_style_text_font(s_setTitle, &lv_font_montserrat_16, 0);
  lv_obj_align(s_setTitle, LV_ALIGN_TOP_MID, 0, 14);
  lv_label_set_text(s_setTitle, "SETTINGS  1/2");

  for (int i = 0; i < 2; i++) {
    s_setPg[i] = lv_obj_create(s_setPage);
    lv_obj_remove_style_all(s_setPg[i]);
    lv_obj_set_size(s_setPg[i], cfg::kScreenW, cfg::kScreenH);
    lv_obj_set_pos(s_setPg[i], 0, 0);
    lv_obj_add_flag(s_setPg[i], LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  auto makeRow = [&](lv_obj_t* pg, int y, lv_event_cb_t cb,
                     int w = 260) -> lv_obj_t* {
    lv_obj_t* row = lv_obj_create(pg);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, 48);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, kColCardBg, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_color(row, kColGridBright, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    // Bubble press/release to the page so swipes over rows still register.
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(row);
    lv_obj_set_style_text_color(lbl, kColText, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
    return lbl;
  };

  // Seven rows at h36/gap6; the bottom one narrows to fit the circle.
  // "Find location again" lives on the knob long-press + web page.
  // ----- page 1: planes filter, text size, units -----
  s_setFltLbl = makeRow(s_setPg[0], 52, [](lv_event_t*) {
    if (s_swiped) return;
    char cur[5];
    Preferences p;
    p.begin("ui", false);
    if (gApp.getCallsignFilter(cur)) {
      gApp.setCallsignFilter("");
      p.putString("flt", "");
    } else {
      String last = p.getString("fltL", DEFAULT_CALLSIGN_FILTER[0]
                                            ? DEFAULT_CALLSIGN_FILTER
                                            : "SWA");
      gApp.setCallsignFilter(last.c_str());
      p.putString("flt", last);
    }
    p.end();
    refreshSettingsLabels();
  });

  s_setTxtLbl = makeRow(s_setPg[0], 108, [](lv_event_t*) {
    if (s_swiped) return;
    gApp.textLarge = !gApp.textLarge.load();
    savePrefs();
    refreshSettingsLabels();  // applyTextSize runs from the tick watcher
  });
  s_setUnitLbl = makeRow(s_setPg[0], 164, [](lv_event_t*) {
    if (s_swiped) return;
    // Cycle presets: ft&kt -> ft&mph -> m&km/h.
    uint8_t preset = gApp.altUnit.load() == 1 ? 2
                     : gApp.spdUnit.load() == 1 ? 1
                                                : 0;
    preset = (preset + 1) % 3;
    gApp.altUnit = (preset == 2) ? 1 : 0;
    gApp.spdUnit = (preset == 2) ? 2 : preset;
    savePrefs();
    refreshSettingsLabels();
  });
  // ----- page 2: help, wifi, device info -----
  lv_obj_t* tutLbl = makeRow(s_setPg[1], 52, [](lv_event_t*) {
    if (s_swiped) return;
    closeSettings();
    openTutorial();
  });
  lv_label_set_text(tutLbl, "How to use");
  s_setWifiLbl = makeRow(s_setPg[1], 108, [](lv_event_t*) {
    if (s_swiped) return;
    // Two-tap confirm: erasing WiFi credentials forces re-setup.
    if (millis() - s_wifiConfirmMs < 3000) {
      WiFi.disconnect(true /*wifi off*/, true /*erase creds*/);
      delay(300);
      ESP.restart();
    } else {
      s_wifiConfirmMs = millis();
      lv_label_set_text(s_setWifiLbl, "Tap again to confirm");
    }
  });

  // Swipe down anywhere on the settings page closes it.
  lv_obj_add_flag(s_setPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_setPage, pressCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      s_setPage,
      [](lv_event_t*) {
        if (releaseSwipeDir() > 0) {
          s_swiped = true;
          closeSettings();
        }
      },
      LV_EVENT_RELEASED, nullptr);

  s_setInfoLbl = lv_label_create(s_setPg[1]);
  lv_obj_set_style_text_color(s_setInfoLbl, kColTextDim, 0);
  lv_obj_set_style_text_font(s_setInfoLbl, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_align(s_setInfoLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_setInfoLbl, LV_ALIGN_TOP_MID, 0, 178);

  lv_obj_t* setHint = lv_label_create(s_setPage);
  lv_obj_set_style_text_color(setHint, kColHint, 0);
  lv_obj_set_style_text_font(setHint, &lv_font_montserrat_10, 0);
  lv_obj_align(setHint, LV_ALIGN_BOTTOM_MID, 0, -22);
  lv_label_set_text(setHint, "dial: page 1/2  \xE2\x80\xA2  swipe down: close");

  // ----- nearby list (swipe down from the radar) -----
  s_nearPage = lv_obj_create(scr);
  lv_obj_remove_style_all(s_nearPage);
  lv_obj_set_size(s_nearPage, cfg::kScreenW, cfg::kScreenH);
  lv_obj_set_pos(s_nearPage, 0, 0);
  lv_obj_set_style_bg_color(s_nearPage, kColBg, 0);
  lv_obj_set_style_bg_opa(s_nearPage, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_nearPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_nearPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_nearPage, nearbyDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(s_nearPage, nearbyClickCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_nearPage, pressCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      s_nearPage,
      [](lv_event_t*) {
        if (releaseSwipeDir() < 0) {
          s_swiped = true;
          closeNearby();
        }
      },
      LV_EVENT_RELEASED, nullptr);

  // ----- first-boot tutorial (tap-through) -----
  s_tutPage = lv_obj_create(scr);
  lv_obj_remove_style_all(s_tutPage);
  lv_obj_set_size(s_tutPage, cfg::kScreenW, cfg::kScreenH);
  lv_obj_set_pos(s_tutPage, 0, 0);
  lv_obj_set_style_bg_color(s_tutPage, kColBg, 0);
  lv_obj_set_style_bg_opa(s_tutPage, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_tutPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_tutPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(
      s_tutPage, [](lv_event_t*) { tutorialNext(); }, LV_EVENT_CLICKED,
      nullptr);

  s_tutTitle = lv_label_create(s_tutPage);
  lv_obj_set_style_text_color(s_tutTitle, kColSweep, 0);
  lv_obj_set_style_text_font(s_tutTitle, &lv_font_montserrat_24, 0);
  lv_obj_align(s_tutTitle, LV_ALIGN_TOP_MID, 0, 64);

  s_tutBody = lv_label_create(s_tutPage);
  lv_obj_set_style_text_color(s_tutBody, kColText, 0);
  lv_obj_set_style_text_font(s_tutBody, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(s_tutBody, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_tutBody, LV_ALIGN_CENTER, 0, 6);

  s_tutStep = lv_label_create(s_tutPage);
  lv_obj_set_style_text_color(s_tutStep, kColTextDim, 0);
  lv_obj_set_style_text_font(s_tutStep, &lv_font_montserrat_12, 0);
  lv_obj_align(s_tutStep, LV_ALIGN_BOTTOM_MID, 0, -44);

  // ----- swipes on the radar: up = settings, down = nearby list -----
  lv_obj_add_event_cb(s_radar, pressCb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(
      s_radar,
      [](lv_event_t*) {
        int dir = releaseSwipeDir();
        if (dir == 0) return;
        s_swiped = true;  // suppress the CLICKED that follows
        if (dir < 0) openSettings();
        else openNearby();
      },
      LV_EVENT_RELEASED, nullptr);

  loadPrefs();
}

void tick() {
  uint32_t now = millis();
  bool redraw = false;

  // First boot: show the tutorial once setup finishes.
  static bool s_wasRunning = false;
  if (!s_wasRunning && gApp.netPhase.load() == NetPhase::Running) {
    s_wasRunning = true;
    if (!s_tutSeen) openTutorial();
  }

  // Requests from the web settings page (display bus belongs to this task).
  int briReq = gApp.briReq.exchange(-1);
  if (briReq >= 0 && briReq <= 3) {
    s_briIdx = (uint8_t)briReq;
    hal::setBrightness(kBriLevels[s_briIdx]);
    gApp.briIdx = s_briIdx;
    savePrefs();
    if (s_setOpen) refreshSettingsLabels();
  }
  if (gApp.tutReq.exchange(false) && !s_tutOpen) openTutorial();

  // Text-size changes (device row or web page) restyle the static labels.
  static bool s_lastTextLarge = false;
  if (s_lastTextLarge != gApp.textLarge.load()) {
    s_lastTextLarge = gApp.textLarge.load();
    applyTextSize();
    redraw = true;
  }

  // Overlay pages swallow input while open.
  if (s_tutOpen) {
    // Dial steps through pages — back turn rescues an accidental tap.
    int steps = hal::encoderTakeSteps();
    if (steps < 0) tutorialPrev();
    else if (steps > 0) tutorialNext();
    if (hal::encoderTakeClick()) tutorialNext();
    hal::encoderTakeLongPress();
    return;
  }
  if (s_setOpen) {
    if (hal::encoderTakeSteps() != 0) {
      s_setPgIdx ^= 1;
      showSetPage();
    }
    if (hal::encoderTakeClick()) closeSettings();
    hal::encoderTakeLongPress();
    // Reset the two-tap confirms if they timed out.
    if (s_wifiConfirmMs && millis() - s_wifiConfirmMs > 3000) {
      s_wifiConfirmMs = 0;
      lv_label_set_text(s_setWifiLbl, "Reset WiFi");
    }
    return;
  }
  if (s_nearOpen) {
    hal::encoderTakeSteps();
    if (hal::encoderTakeClick()) closeNearby();
    hal::encoderTakeLongPress();
    if (now - s_lastSnapMs >= 500) {
      s_lastSnapMs = now;
      gAircraft.snapshot(s_snap);
      lv_obj_invalidate(s_nearPage);
    }
    return;
  }
  // Aircraft page swallows input while open.
  if (s_acPageOpen) {
    hal::encoderTakeSteps();
    if (hal::encoderTakeClick()) closeAircraftPage();
    hal::encoderTakeLongPress();
    if (now - s_lastTextMs >= 500) {
      s_lastTextMs = now;
      if (now - s_lastSnapMs >= 250) {
        s_lastSnapMs = now;
        gAircraft.snapshot(s_snap);
      }
      updateAircraftPage();
    }
    return;
  }

  // Encoder: zoom. Presets are mercator z-levels: index up = zoom in.
  int steps = hal::encoderTakeSteps();
  if (steps != 0) {
    int z = (int)gApp.zoomIdx.load() + (steps > 0 ? 1 : -1);
    z = constrain(z, 0, (int)cfg::kZoomCount - 1);
    if (z != (int)gApp.zoomIdx.load()) {
      gApp.zoomIdx = (uint8_t)z;
      // No immediate poll needed: traffic is always fetched at max radius,
      // so every zoom level draws from the same fully-populated store.
      redraw = true;
      updateStatusText();  // update range readout immediately
    }
  }
  if (hal::encoderTakeClick()) {
    if (s_selHex[0]) {
      s_selHex[0] = '\0';  // knob click clears the selection
      redraw = true;
    }
  }
  if (hal::encoderTakeLongPress()) {
    gApp.relocateReq = true;  // re-run geolocation
  }

  // Fresh aircraft snapshot at ~4 Hz, but only re-render when the data
  // actually changed (a poll landed). Redrawing identical frames at 4 Hz
  // kept the CPU inside 100ms+ renders most of the time and starved input.
  if (now - s_lastSnapMs >= 250) {
    s_lastSnapMs = now;
    uint32_t prevUpdate = s_snap.lastUpdateMs;
    uint8_t prevCount = s_snap.count;
    gAircraft.snapshot(s_snap);
    if (s_snap.lastUpdateMs != prevUpdate || s_snap.count != prevCount) {
      redraw = true;
    }
  }

  // Status text at 2 Hz.
  if (now - s_lastTextMs >= 500) {
    s_lastTextMs = now;
    updateStatusText();
    updateDetailPanel();
  }

  if (maplayer::takeDirty()) redraw = true;

  // No sweep animation anymore — only redraw when something changed.
  if (redraw) lv_obj_invalidate(s_radar);
}

}  // namespace ui
