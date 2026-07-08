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

// Web palette, selected at request time so it matches the device: strict
// Southwest corporate colors while in SWA-only mode, radar green otherwise.
struct WebPal {
  const char *bg, *card, *fld, *txt, *dim, *brd, *acc, *dgb, *dgt;
};

WebPal webPal() {
  static const WebPal grn = {"#04140a", "#07200f", "#0b2a16",
                             "#9df5bd", "#4d8f66", "#1c5c31",
                             "#27f06c", "#7a2222", "#ff8484"};
#ifdef SWA_THEME
  // Bold Blue / Warm Red / Sunrise Yellow / Summit Silver on black.
  static const WebPal swa = {"#000000", "#000000", "#000000",
                             "#FFFFFF", "#CCCCCC", "#304CB2",
                             "#FFBF27", "#D5152E", "#D5152E"};
  char f[5];
  gApp.getCallsignFilter(f);
  if (strcmp(f, "SWA") == 0) return swa;
#endif
  return grn;
}


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
  WebPal w = webPal();
  String h;
  h.reserve(1024);
  char css[420];
  snprintf(css, sizeof(css),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>FlightRadar</title><style>"
           "body{background:%s;color:%s;font-family:system-ui,sans-serif;"
           "max-width:430px;margin:40px auto;padding:16px;text-align:center}"
           "h1{color:%s}input,button{background:%s;color:%s;"
           "border:1px solid %s;border-radius:8px;padding:10px 14px;"
           "font-size:1.1em;margin:4px}small{color:%s}"
           ".err{color:%s}</style></head><body><h1>FlightRadar</h1>",
           w.bg, w.txt, w.acc, w.fld, w.txt, w.brd, w.dim, w.dgt);
  h += css;
  h += F(
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
  WebPal w = webPal();
  String h;
  h.reserve(6144);
  char buf[520];
  snprintf(buf, sizeof(buf),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>FlightRadar</title><style>"
           "body{background:%s;color:%s;font-family:system-ui,sans-serif;"
           "max-width:430px;margin:0 auto;padding:16px 16px 32px}"
           "h1{color:%s;font-size:1.4em;margin-bottom:4px}"
           "h2{font-size:.78em;color:%s;margin:20px 0 6px;"
           "text-transform:uppercase;letter-spacing:.08em}"
           ".card{background:%s;border:1px solid %s;border-radius:12px;"
           "padding:14px;margin-bottom:4px}"
           ".row{margin:10px 0 0}.row:first-child{margin-top:0}"
           ".lbl{display:inline-block;min-width:78px;color:%s;font-size:.9em}"
           "input,button{background:%s;color:%s;border:1px solid %s;"
           "border-radius:8px;padding:8px 12px;font-size:1em;margin:2px 6px 2px 0}"
           "button{cursor:pointer}button.on{border-color:%s}"
           ".danger{border-color:%s;color:%s}"
           ".note{background:%s;border:1px solid %s;border-radius:8px;"
           "padding:10px;margin-bottom:12px}"
           "small{color:%s;line-height:1.4}"
           ".sw{width:34px;height:26px;padding:0}"
           "</style></head><body><h1>FlightRadar</h1>",
           w.bg, w.txt, w.acc, w.dim, w.card, w.brd, w.dim, w.fld, w.txt, w.brd,
           w.acc, w.dgb, w.dgt, w.fld, w.acc, w.dim);
  h += buf;

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
      char sel[40] = "";
      if (i == cur) snprintf(sel, sizeof(sel), ";border:2px solid %s", w.acc);
      snprintf(buf, sizeof(buf),
               "<button class=sw name=%s value=%d title='%s' "
               "style='background:#%06lX%s'></button>",
               arg, i, kPlaneColors[i].name,
               (unsigned long)kPlaneColors[i].hex, sel);
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
  // Two independent optional enhanced sources — either, both, or neither.
  // adsbdb (free) is always tried first; AeroAPI's filed plans are tried
  // before AirLabs when both are configured (best for through-flights).
  bool anyEnhanced = routes::aeroActive() || routes::airlabsActive();
  h += F("<h2>Route data</h2><div class=card>");
  snprintf(buf, sizeof(buf), "<b>%s</b> &bull; free community database%s",
           anyEnhanced ? "Enhanced" : "Basic",
           anyEnhanced ? " (always tried first)" : "");
  h += buf;
  if (!anyEnhanced) {
    h += F(
        "<br><small>Some routes are missing or hidden as unreliable "
        "&mdash; especially multi-leg airlines like Southwest, where one "
        "flight number covers several city pairs per day.</small>");
  }
  h += F("<br><br>");

  // -- FlightAware AeroAPI --
  if (routes::aeroActive()) {
    snprintf(buf, sizeof(buf),
             "<b>FlightAware AeroAPI:</b> active &bull; %lu of %lu lookups "
             "this month<br><small>Fills gaps with real filed flight "
             "plans &mdash; the correct leg for Southwest through-flights.</small>"
             "<form method=post action=/set class=row><button name=aeroclr "
             "value=1 class=danger>Remove key</button></form>",
             (unsigned long)routes::aeroUsedThisMonth(),
             (unsigned long)cfg::kAeroMonthlyCap);
    h += buf;
  } else {
    h += F(
        "<b>FlightAware AeroAPI</b> (free upgrade): real filed flight "
        "plans. The personal tier includes $5/month of credit; on-device "
        "caching keeps typical home use inside it, with a built-in monthly "
        "cap to prevent overage.<br><small>Free account &amp; key at "
        "<b>flightaware.com/aeroapi</b>:</small>"
        "<form method=post action=/set class=row><input name=aerokey "
        "size=28 placeholder='AeroAPI key'><button>Save</button></form>");
  }
  h += F("<br>");

  // -- AirLabs --
  if (routes::airlabsActive()) {
    snprintf(buf, sizeof(buf),
             "<b>AirLabs:</b> active &bull; %lu of %lu lookups this month"
             "<br><small>An independent maintained schedule database, "
             "tried when AeroAPI is off or also misses.</small>"
             "<form method=post action=/set class=row><button name=airlabsclr "
             "value=1 class=danger>Remove key</button></form>",
             (unsigned long)routes::airlabsUsedThisMonth(),
             (unsigned long)cfg::kAirlabsMonthlyCap);
    h += buf;
  } else {
    h += F(
        "<b>AirLabs</b> (free upgrade): an alternative maintained schedule "
        "database &mdash; a second chance at routes the free community DB "
        "doesn't have. AirLabs offers a free tier for light use; check "
        "current limits when you sign up, and this device caps its own "
        "usage as a safety net.<br><small>Free account &amp; key at "
        "<b>airlabs.co</b>:</small>"
        "<form method=post action=/set class=row><input name=airlabskey "
        "size=28 placeholder='AirLabs key'><button>Save</button></form>");
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
  if (s_server.hasArg("airlabskey") && s_server.arg("airlabskey").length() > 0) {
    String k = s_server.arg("airlabskey");
    k.trim();
    routes::setAirlabsKey(k.c_str());
    notice = "AirLabs key saved.";
  }
  if (s_server.hasArg("airlabsclr")) {
    routes::setAirlabsKey("");
    notice = "AirLabs key removed.";
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
    if (flt[0]) gAircraft.purgeNonCallsign(flt);  // drop non-matching now
    gApp.pollNowReq = true;
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
