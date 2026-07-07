#include "web_settings.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "aircraft_store.h"
#include "app_state.h"
#include "config.h"
#include "geolocate.h"
#include "route_client.h"

namespace websrv {

// Web palette (themed per flavor at compile time).
#ifdef SWA_THEME
// Strict corporate palette: Bold Blue, Warm Red, Sunrise Yellow, Summit
// Silver + neutral black/white only. Structure comes from blue borders.
#define W_BG "#000000"
#define W_CARD "#000000"
#define W_FLD "#000000"
#define W_TXT "#FFFFFF"
#define W_DIM "#CCCCCC"
#define W_BRD "#304CB2"
#define W_ACC "#FFBF27"
#define W_DGB "#D5152E"
#define W_DGT "#D5152E"
#else
#define W_BG "#04140a"
#define W_CARD "#07200f"
#define W_FLD "#0b2a16"
#define W_TXT "#9df5bd"
#define W_DIM "#4d8f66"
#define W_BRD "#1c5c31"
#define W_ACC "#27f06c"
#define W_DGB "#7a2222"
#define W_DGT "#ff8484"
#endif


namespace {

WebServer s_server(80);
bool s_started = false;
char s_geoNotice[160] = {0};

// ---- session auth ----
// The device password (shown on the on-device settings page) gates the web
// UI. Successful login sets a per-boot random session cookie; all pages and
// every mutation require it. Guest networks are hostile LANs.
char s_token[17] = {0};
uint8_t s_loginFails = 0;
uint32_t s_lockUntil = 0;

bool authed() {
  if (!s_token[0] || !s_server.hasHeader("Cookie")) return false;
  return s_server.header("Cookie").indexOf(s_token) >= 0;
}

String loginPage(const char* msg) {
  String h;
  h.reserve(1024);
  h += F(
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>FlightRadar</title><style>"
      "body{background:" W_BG ";color:" W_TXT ";font-family:system-ui,sans-serif;"
      "max-width:430px;margin:40px auto;padding:16px;text-align:center}"
      "h1{color:" W_ACC "}input,button{background:" W_FLD ";color:" W_TXT ";"
      "border:1px solid " W_BRD ";border-radius:8px;padding:10px 14px;"
      "font-size:1.1em;margin:4px}small{color:" W_DIM "}"
      ".err{color:" W_DGT "}</style></head><body><h1>FlightRadar</h1>"
      "<p>Enter the password shown on the device<br><small>(swipe up on the "
      "radar &rarr; bottom of the settings page)</small></p>"
      "<form method=post action=/login><input name=pin maxlength=8 "
      "autocapitalize=characters autocomplete=off placeholder='ABCD2345'>"
      "<button>Unlock</button></form>");
  if (msg) {
    h += "<p class=err>";
    h += msg;
    h += "</p>";
  }
  h += F("</body></html>");
  return h;
}

void handleLogin() {
  if (millis() < s_lockUntil) {
    s_server.send(429, "text/html",
                  loginPage("Too many attempts — wait a minute."));
    return;
  }
  char pass[9];
  devicePassword(pass);
  String pin = s_server.arg("pin");
  pin.trim();
  pin.toUpperCase();
  if (s_server.method() == HTTP_POST && pin == pass) {
    s_loginFails = 0;
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "s=%s; HttpOnly; SameSite=Strict",
             s_token);
    s_server.sendHeader("Set-Cookie", cookie);
    s_server.sendHeader("Location", "/");
    s_server.send(303, "text/plain", "");
    return;
  }
  if (++s_loginFails >= 5) {
    s_loginFails = 0;
    s_lockUntil = millis() + 60000;
  }
  s_server.send(403, "text/html", loginPage("Wrong password."));
}

// Mutations require a valid session AND a POST (GET mutations are CSRF bait).
bool mutationAllowed() {
  return authed() && s_server.method() == HTTP_POST;
}

void urlEncodeTo(const char* in, char* out, size_t outLen) {
  size_t o = 0;
  for (const char* c = in; *c && o + 4 < outLen; c++) {
    if (isalnum((unsigned char)*c) || *c == '-' || *c == '.') {
      out[o++] = *c;
    } else {
      o += snprintf(&out[o], outLen - o, "%%%02X", (unsigned char)*c);
    }
  }
  out[o] = '\0';
}

// Address -> lat/lon via Nominatim (OSM). One-shot at setup time — well
// within their fair-use policy. Coordinates are stored; the address isn't.
bool geocode(const char* query, float* lat, float* lon, char* place,
             size_t placeLen) {
  char enc[256];
  urlEncodeTo(query, enc, sizeof(enc));
  char url[360];
  snprintf(url, sizeof(url),
           "https://nominatim.openstreetmap.org/search?q=%s&format=jsonv2"
           "&limit=1",
           enc);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setUserAgent(cfg::kUserAgent);
  http.setTimeout(cfg::kHttpTimeoutMs);
  http.useHTTP10(true);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[web] nominatim HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
  JsonObject hit = doc[0];
  if (hit.isNull()) return false;
  // Nominatim returns lat/lon as strings.
  *lat = atof(hit["lat"] | "0");
  *lon = atof(hit["lon"] | "0");
  if (*lat == 0 && *lon == 0) return false;
  strlcpy(place, hit["display_name"] | "", placeLen);
  return true;
}

String page(const char* notice = nullptr) {
  uint8_t bri = gApp.briIdx.load();
  String h;
  h.reserve(6144);
  h += F(
      "<!doctype html><html><head><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>FlightRadar</title><style>"
      "body{background:" W_BG ";color:" W_TXT ";font-family:system-ui,sans-serif;"
      "max-width:430px;margin:0 auto;padding:16px 16px 32px}"
      "h1{color:" W_ACC ";font-size:1.4em;margin-bottom:4px}"
      "h2{font-size:.78em;color:" W_DIM ";margin:20px 0 6px;"
      "text-transform:uppercase;letter-spacing:.08em}"
      ".card{background:" W_CARD ";border:1px solid " W_BRD ";border-radius:12px;"
      "padding:14px;margin-bottom:4px}"
      ".row{margin:10px 0 0}.row:first-child{margin-top:0}"
      ".lbl{display:inline-block;min-width:78px;color:" W_DIM ";font-size:.9em}"
      "input,button{background:" W_FLD ";color:" W_TXT ";border:1px solid " W_BRD ";"
      "border-radius:8px;padding:8px 12px;font-size:1em;margin:2px 6px 2px 0}"
      "button{cursor:pointer}button.on{border-color:" W_ACC "}"
      ".danger{border-color:" W_DGB ";color:" W_DGT "}"
      ".note{background:" W_FLD ";border:1px solid " W_ACC ";border-radius:8px;"
      "padding:10px;margin-bottom:12px}"
      "small{color:" W_DIM ";line-height:1.4}"
      ".sw{width:34px;height:26px;padding:0}"
      "</style></head><body><h1>FlightRadar</h1>");

  char buf[240];

  if (notice) {
    h += "<div class=note>";
    h += notice;
    h += "</div>";
  }

  // ---- Status ----
  snprintf(buf, sizeof(buf),
           "<div class=card>v%s &bull; <b>%d aircraft</b> tracked<br>"
           "<small>location %.5f, %.5f (%s) &bull; http://%s.local &bull; "
           "%s &bull; free heap %u KB</small></div>",
           cfg::kFwVersion, gAircraft.count(), gApp.centerLat.load(),
           gApp.centerLon.load(), locSourceName(gApp.locSource.load()),
           cfg::kHostname, WiFi.localIP().toString().c_str(),
           (unsigned)(ESP.getFreeHeap() / 1024));
  h += buf;

  // ---- Display ----
  h += F("<h2>Display</h2><div class=card><form method=post action=/set>"
         "<div class=row><span class=lbl>Brightness</span>");
  for (int i = 0; i < 4; i++) {
    snprintf(buf, sizeof(buf), "<button name=bri value=%d%s>%d%%</button>", i,
             i == bri ? " class=on" : "", (i + 1) * 25);
    h += buf;
  }
  h += F("</div><div class=row><span class=lbl>Text size</span>");
  snprintf(buf, sizeof(buf),
           "<button name=txtl value=0%s>normal</button>"
           "<button name=txtl value=1%s>large</button>",
           gApp.textLarge.load() ? "" : " class=on",
           gApp.textLarge.load() ? " class=on" : "");
  h += buf;
  h += F("</div><div class=row><span class=lbl>Altitude</span>");
  for (int i = 0; i < 2; i++) {
    snprintf(buf, sizeof(buf), "<button name=altu value=%d%s>%s</button>", i,
             i == gApp.altUnit.load() ? " class=on" : "", i == 0 ? "ft" : "m");
    h += buf;
  }
  h += F("</div><div class=row><span class=lbl>Speed</span>");
  static const char* kSpd[3] = {"kt", "mph", "km/h"};
  for (int i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), "<button name=spdu value=%d%s>%s</button>", i,
             i == gApp.spdUnit.load() ? " class=on" : "", kSpd[i]);
    h += buf;
  }
  h += F("</div></form></div>");

  // ---- Planes ----
  h += F("<h2>Planes</h2><div class=card><form method=post action=/set>");
  snprintf(buf, sizeof(buf),
           "<div class=row><span class=lbl>Airline color</span>"
           "<button name=acol value=%d%s>on</button>"
           "<button name=acol value=%d%s>off</button></div>",
           1, gApp.airlineColors.load() ? " class=on" : "", 0,
           gApp.airlineColors.load() ? "" : " class=on");
  h += buf;
  for (int row = 0; row < 2; row++) {
    const char* arg = row == 0 ? "ccol" : "pcol";
    uint8_t cur = row == 0 ? gApp.colComm.load() : gApp.colPriv.load();
    snprintf(buf, sizeof(buf), "<div class=row><span class=lbl>%s</span>",
             row == 0 ? "Airlines" : "Small planes");
    h += buf;
    for (int i = 0; i < kPlaneColorCount; i++) {
      snprintf(buf, sizeof(buf),
               "<button class=sw name=%s value=%d title='%s' "
               "style='background:#%06lX%s'></button>",
               arg, i, kPlaneColors[i].name,
               (unsigned long)kPlaneColors[i].hex,
               i == cur ? ";border:2px solid " W_ACC "" : "");
      h += buf;
    }
    h += F("</div>");
  }
  {
    char flt[5];
    gApp.getCallsignFilter(flt);
    snprintf(buf, sizeof(buf),
             "<div class=row><span class=lbl>Only airline</span>"
             "<input name=flt size=5 maxlength=4 value='%s' "
             "placeholder='SWA'><button>Save</button></div>",
             flt);
    h += buf;
  }
  h += F("</form><small>Colors: selection stays yellow, emergencies red "
         "(picking red for a category makes emergencies stand out less). "
         "Airline filter: a callsign prefix like SWA, AAL or UAL tracks one "
         "carrier only; blank tracks everything. Filtered planes clear "
         "within ~30 s.</small></div>");

  // ---- Route data ----
  h += F("<h2>Route data</h2><div class=card>");
  if (routes::aeroActive()) {
    snprintf(buf, sizeof(buf),
             "<b>Enhanced</b> &bull; FlightAware AeroAPI active &bull; "
             "%lu of %lu lookups used this month<br>"
             "<small>Missing routes are filled from real filed flight "
             "plans.</small>"
             "<form method=post action=/set class=row><button name=aeroclr value=1 "
             "class=danger>Remove key</button></form>",
             (unsigned long)routes::aeroUsedThisMonth(),
             (unsigned long)cfg::kAeroMonthlyCap);
    h += buf;
  } else {
    h += F(
        "<b>Basic</b> &bull; free community database<br>"
        "<small>Some routes are missing or hidden as unreliable &mdash; "
        "especially multi-leg airlines like Southwest, where one flight "
        "number covers several city pairs per day.</small><br><br>"
        "<b>Free upgrade:</b> a FlightAware AeroAPI key fills the gaps with "
        "real filed flight plans &mdash; the correct leg for Southwest "
        "through-flights, plus charters and internationals. The personal "
        "tier includes $5/month of credit; on-device caching keeps typical "
        "home use inside it, and a built-in monthly cap prevents overage."
        "<br><small>Create a free account and key at "
        "<b>flightaware.com/aeroapi</b>, then paste it here:</small>"
        "<form method=post action=/set class=row><input name=aerokey size=30 "
        "placeholder='AeroAPI key'><button>Save</button></form>");
  }
  h += F("</div>");

  // ---- Location ----
  h += F(
      "<h2>Location</h2><div class=card>"
      "<form method=post action=/set class=row><span class=lbl>Address</span>"
      "<input name=addr size=22 placeholder='1234 Main St, Fort Worth TX'>"
      "<button>Set</button></form>"
      "<form method=post action=/set class=row><span class=lbl>or lat, lon</span>"
      "<input name=loc size=14 placeholder='32.95, -97.26'>"
      "<button>Set</button></form>"
      "<form method=post action=/set class=row><span class=lbl></span>"
      "<button name=relocate value=1>Re-detect via WiFi</button></form>"
      "<small>Addresses are looked up once (OpenStreetMap) and only the "
      "coordinates are stored. A manual location stays until you set a new "
      "one, re-detect, or factory reset.</small></div>");

  // ---- Device ----
  h += F(
      "<h2>Device</h2><div class=card>"
      "<form method=post action=/set style='display:inline'><button name=tutorial "
      "value=1>Show tutorial</button></form>"
      "<form method=post action=/reboot style='display:inline'><button>Reboot</button>"
      "</form>"
      "<form method=post action=/factory style='display:inline'><button "
      "class=danger>Factory reset&hellip;</button></form></div>"
      "<small>FlightRadar &bull; traffic: adsb.lol / adsb.fi &bull; routes: "
      "adsbdb.com &bull; logos: avs.io &bull; map &copy; CARTO / "
      "OpenStreetMap &bull; geocoding: Nominatim</small>"
      "</body></html>");
  return h;
}

void handleRoot() {
  if (!authed()) {
    s_server.send(200, "text/html", loginPage(nullptr));
    return;
  }
  s_server.send(200, "text/html", page());
}

void savePrefKey(const char* key, uint8_t v) {
  Preferences p;
  p.begin("ui", false);
  p.putUChar(key, v);
  p.end();
}

void handleSet() {
  if (!mutationAllowed()) {
    s_server.send(403, "text/html", loginPage("Session expired — log in."));
    return;
  }
  const char* notice = "Saved.";
  if (s_server.hasArg("bri")) {
    int v = s_server.arg("bri").toInt();
    if (v >= 0 && v <= 3) gApp.briReq = v;
  }
  if (s_server.hasArg("altu")) {
    int v = s_server.arg("altu").toInt();
    if (v >= 0 && v <= 1) {
      gApp.altUnit = (uint8_t)v;
      savePrefKey("altu", (uint8_t)v);
    }
  }
  if (s_server.hasArg("spdu")) {
    int v = s_server.arg("spdu").toInt();
    if (v >= 0 && v <= 2) {
      gApp.spdUnit = (uint8_t)v;
      savePrefKey("spdu", (uint8_t)v);
    }
  }
  if (s_server.hasArg("acol")) {
    bool on = s_server.arg("acol").toInt() != 0;
    gApp.airlineColors = on;
    Preferences p;
    p.begin("ui", false);
    p.putBool("acol", on);
    p.end();
  }
  if (s_server.hasArg("aerokey") && s_server.arg("aerokey").length() > 0) {
    String k = s_server.arg("aerokey");
    k.trim();
    routes::setAeroKey(k.c_str());
    notice = "AeroAPI key saved — filed flight plans active.";
  }
  if (s_server.hasArg("aeroclr")) {
    routes::setAeroKey("");
    notice = "AeroAPI key removed — back to the free database.";
  }
  if (s_server.hasArg("flt")) {
    char flt[5] = {0};
    const String& v = s_server.arg("flt");
    int o = 0;
    for (size_t i = 0; i < v.length() && o < 4; i++) {
      if (isalpha((unsigned char)v[i])) flt[o++] = toupper(v[i]);
    }
    gApp.setCallsignFilter(flt);
    Preferences p;
    p.begin("ui", false);
    p.putString("flt", flt);
    if (flt[0]) p.putString("fltL", flt);  // device toggle restores this
    p.end();
    notice = flt[0] ? "Airline filter set." : "Airline filter cleared.";
  }
  if (s_server.hasArg("ccol")) {
    int v = s_server.arg("ccol").toInt();
    if (v >= 0 && v < kPlaneColorCount) {
      gApp.colComm = (uint8_t)v;
      savePrefKey("ccol", (uint8_t)v);
    }
  }
  if (s_server.hasArg("pcol")) {
    int v = s_server.arg("pcol").toInt();
    if (v >= 0 && v < kPlaneColorCount) {
      gApp.colPriv = (uint8_t)v;
      savePrefKey("pcol", (uint8_t)v);
    }
  }
  if (s_server.hasArg("txtl")) {
    bool on = s_server.arg("txtl").toInt() != 0;
    gApp.textLarge = on;  // UI tick watcher restyles the labels
    Preferences p;
    p.begin("ui", false);
    p.putBool("txtl", on);
    p.end();
  }
  if (s_server.hasArg("addr") && s_server.arg("addr").length() > 0) {
    float lat, lon;
    char place[110];
    if (geocode(s_server.arg("addr").c_str(), &lat, &lon, place,
                sizeof(place))) {
      char coords[40];
      snprintf(coords, sizeof(coords), "%.5f, %.5f", lat, lon);
      geolocate::saveManual(coords);
      snprintf(s_geoNotice, sizeof(s_geoNotice),
               "Radar centered on: %s (%.4f, %.4f)", place, lat, lon);
      notice = s_geoNotice;
    } else {
      notice = "Address not found — try adding city/state, or use lat, lon.";
    }
  }
  if (s_server.hasArg("loc") && s_server.arg("loc").length() > 0) {
    if (!geolocate::saveManual(s_server.arg("loc").c_str())) {
      notice = "Could not parse location — use \"lat, lon\" in decimal "
               "degrees.";
    }
  }
  if (s_server.hasArg("relocate")) {
    gApp.relocateReq = true;
    notice = "Re-detecting location...";
  }
  if (s_server.hasArg("tutorial")) {
    gApp.tutReq = true;
    notice = "Tutorial opened on the device.";
  }
  s_server.send(200, "text/html", page(notice));
}

void handleReboot() {
  if (!mutationAllowed()) {
    s_server.send(403, "text/html", loginPage("Session expired — log in."));
    return;
  }
  if (!s_server.hasArg("confirm")) {
    s_server.send(200, "text/html",
                  page("<form method=post action=/reboot><button name=confirm "
                       "value=1>Confirm reboot</button></form>"));
    return;
  }
  s_server.send(200, "text/html", "<body>Rebooting...</body>");
  delay(400);
  ESP.restart();
}

void handleFactory() {
  if (!mutationAllowed()) {
    s_server.send(403, "text/html", loginPage("Session expired — log in."));
    return;
  }
  if (!s_server.hasArg("confirm")) {
    s_server.send(
        200, "text/html",
        page("<b>Factory reset erases WiFi, location and settings.</b>"
             "<form method=post action=/factory><button name=confirm value=1 "
             "class=danger>Confirm factory reset</button></form>"));
    return;
  }
  s_server.send(200, "text/html",
                "<body>Resetting. Reconnect to the FlightRadar-Setup WiFi to "
                "set up again.</body>");
  delay(400);
  Preferences p;
  p.begin("geo", false);
  p.clear();
  p.end();
  p.begin("ui", false);
  p.clear();
  p.end();
  WiFi.disconnect(true /*wifi off*/, true /*erase creds*/);
  delay(300);
  ESP.restart();
}

}  // namespace

void start() {
  if (s_started) return;
  snprintf(s_token, sizeof(s_token), "%08lx%08lx",
           (unsigned long)esp_random(), (unsigned long)esp_random());
  static const char* kHdrs[] = {"Cookie"};
  s_server.collectHeaders(kHdrs, 1);
  if (MDNS.begin(cfg::kHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
  s_server.on("/", handleRoot);
  s_server.on("/login", handleLogin);
  s_server.on("/set", handleSet);
  s_server.on("/reboot", handleReboot);
  s_server.on("/factory", handleFactory);
  s_server.onNotFound(handleRoot);
  s_server.begin();
  s_started = true;
  Serial.printf("[web] settings at http://%s.local/\n", cfg::kHostname);
}

void service() {
  if (s_started) s_server.handleClient();
}

}  // namespace websrv
