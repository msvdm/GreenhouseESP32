#include "web.h"
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "auth.h"
#include "control.h"
#include "display.h"
#include "led.h"
#include "net.h"
#include "sensors.h"
#include "settings.h"
#include "web_pages.h"

static WebServer server(80);

static bool restartPending = false;
static unsigned long restartRequested = 0;

static bool uploadStarted = false;
static bool uploadRejected = false;

static void scheduleRestart() {
  restartPending = true;
  restartRequested = millis();
}

static void sendOk() { server.send(200, "application/json", "{\"ok\":1}"); }

static void sendError(int code, const char* msg) {
  String body = "{\"error\":\"";
  body += msg;
  body += "\"}";
  server.send(code, "application/json", body);
}

// Serve a static page; HTML pages need a login, except the login page itself.
static void sendPage(const char* html, bool needLogin) {
  if (needLogin && !authLoggedIn(server)) {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", html);
}

static void sendStatic(const char* type, const char* body) {
  server.sendHeader("Cache-Control", "max-age=86400");   // URL carries the version
  server.send_P(200, type, body);
}

// ----------------------------------------------------------------------------
// JSON helpers - small and flat, so no JSON library.
// ----------------------------------------------------------------------------
static void jsonTemp(String& out, float t) {
  if (isnan(t)) { out += "null"; return; }
  char b[12];
  snprintf(b, sizeof(b), "%.2f", t);
  out += b;
}

static void jsonStr(String& out, const char* s) {
  out += '"';
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') out += '\\';
    if ((uint8_t)*s >= 0x20) out += *s;
  }
  out += '"';
}

static void handleStatus() {
  if (!authRequire(server)) return;

  String j;
  j.reserve(700);
  j += "{\"fw\":\"" FW_VERSION "\",\"up\":";
  j += millis() / 1000;
  j += ",\"avg\":";
  jsonTemp(j, sensorsAverage());
  j += ",\"lines\":[";
  for (int i = 0; i < 2; i++) {
    const SensorLine& l = sensorLines[i];
    if (i) j += ',';
    j += "{\"avg\":";
    jsonTemp(j, l.avg);
    j += ",\"n\":";
    j += l.count;
    j += ",\"t\":[";
    for (int k = 0; k < l.count; k++) {
      if (k) j += ',';
      jsonTemp(j, l.temps[k]);
    }
    j += "]}";
  }
  j += "],\"heater\":";      j += ctrl.heater ? 1 : 0;
  j += ",\"fan\":";          j += ctrl.fan ? 1 : 0;
  j += ",\"heaterMode\":\""; j += relayModeName(ctrl.heaterMode);
  j += "\",\"fanMode\":\"";  j += relayModeName(ctrl.fanMode);
  j += "\",\"fanWhy\":\"";   j += fanReason();
  j += "\",\"room\":";       j += settings.roomHeating ? 1 : 0;
  j += ",\"heatOn\":";       jsonTemp(j, settings.heatOn);
  j += ",\"coolOn\":";       jsonTemp(j, settings.coolOn);
  j += ",\"hyst\":";         jsonTemp(j, settings.hyst);
  j += ",\"heaterSec\":";    j += ctrl.heaterSec;
  j += ",\"fanSec\":";       j += ctrl.fanSec;
  j += ",\"staIp\":\"";      j += netStaIp();
  j += "\",\"rssi\":";       j += netStaConnected() ? WiFi.RSSI() : 0;
  j += ",\"pwDefault\":";    j += authIsDefault() ? 1 : 0;
  j += '}';

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// ----------------------------------------------------------------------------
// CONTROL API
// ----------------------------------------------------------------------------
static void handleRelay() {
  if (!authRequire(server)) return;
  RelayMode mode;
  if (!relayModeParse(server.arg("mode"), mode)) return sendError(400, "mode must be auto, on or off");

  const String target = server.arg("target");
  if (target == "heater")   ctrl.heaterMode = mode;
  else if (target == "fan") ctrl.fanMode = mode;
  else return sendError(400, "target must be heater or fan");

  controlUpdate(millis());   // apply at once, not on the next 2 s tick
  sendOk();
}

static void handleRoom() {
  if (!authRequire(server)) return;
  settings.roomHeating = server.arg("on") == "1";
  settingsSave();
  controlUpdate(millis());
  sendOk();
}

// NAN unless the whole argument is a number (String::toFloat turns junk into 0).
static float numberArg(const char* name) {
  const String s = server.arg(name);
  char* end = nullptr;
  const float v = strtof(s.c_str(), &end);
  return (s.length() && end && *end == 0) ? v : NAN;
}

static void handleSetpoints() {
  if (!authRequire(server)) return;
  const float heatOn = numberArg("heatOn");
  const float coolOn = numberArg("coolOn");
  const float hyst   = numberArg("hyst");

  const char* err = settingsValidate(heatOn, coolOn, hyst);
  if (err) return sendError(400, err);

  settings.heatOn = heatOn;
  settings.coolOn = coolOn;
  settings.hyst = hyst;
  settingsSave();
  controlUpdate(millis());
  sendOk();
}

// ----------------------------------------------------------------------------
// SETTINGS API
// ----------------------------------------------------------------------------
static void handleWifiGet() {
  if (!authRequire(server)) return;
  const WifiSettings& w = netConfig();
  String j = "{\"sta\":";
  j += w.staEnabled ? 1 : 0;
  j += ",\"ssid\":";   jsonStr(j, w.staSsid);
  j += ",\"apSsid\":"; jsonStr(j, w.apSsid);
  j += ",\"host\":";   jsonStr(j, w.hostname);
  j += '}';
  server.send(200, "application/json", j);
}

static bool hostnameOk(String& h) {
  h.toLowerCase();
  if (h.length() < 1 || h.length() > 32 || h[0] == '-' || h[h.length() - 1] == '-') return false;
  for (char c : h) {
    if (!isalnum((unsigned char)c) && c != '-') return false;
  }
  return true;
}

static void handleWifiPost() {
  if (!authRequire(server)) return;
  WifiSettings w = netConfig();

  const String ssid = server.arg("ssid"), pass = server.arg("pass");
  const String apSsid = server.arg("apSsid"), apPass = server.arg("apPass");
  String host = server.arg("host");

  w.staEnabled = server.arg("sta") == "1";
  if (w.staEnabled && (ssid.length() < 1 || ssid.length() > 32)) return sendError(400, "Network name must be 1-32 characters");
  if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) return sendError(400, "WiFi password must be 8-63 characters");
  if (apSsid.length() < 1 || apSsid.length() > 32) return sendError(400, "Access point name must be 1-32 characters");
  if (apPass.length() > 0 && (apPass.length() < 8 || apPass.length() > 63)) return sendError(400, "Access point password must be 8-63 characters");
  if (!hostnameOk(host)) return sendError(400, "Hostname: letters, digits and '-' only");

  strlcpy(w.staSsid, ssid.c_str(), sizeof(w.staSsid));
  if (pass.length()) strlcpy(w.staPass, pass.c_str(), sizeof(w.staPass));
  strlcpy(w.apSsid, apSsid.c_str(), sizeof(w.apSsid));
  if (apPass.length()) strlcpy(w.apPass, apPass.c_str(), sizeof(w.apPass));
  strlcpy(w.hostname, host.c_str(), sizeof(w.hostname));

  netSave(w);
  sendOk();
  scheduleRestart();
}

static void handleReboot() {
  if (!authRequire(server)) return;
  sendOk();
  scheduleRestart();
}

// ----------------------------------------------------------------------------
// BROWSER FIRMWARE UPLOAD
// ----------------------------------------------------------------------------
// The session cookie is a header, so it is checked on UPLOAD_FILE_START before
// anything is written; nothing can be sent from inside the upload callback, so
// the result handler answers afterwards.
static void handleUploadData() {
  HTTPUpload& up = server.upload();
  static size_t lastShownKb = 0;

  switch (up.status) {
    case UPLOAD_FILE_START:
      uploadStarted = true;
      uploadRejected = !authLoggedIn(server);
      if (uploadRejected) break;
      otaSetActive(true);
      controlAllOff();
      controlSaveRuntime();
      lastShownKb = 0;
      displayOta("Receiving firmware", -1);
      Serial.printf("[ota] browser upload: %s\n", up.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      break;

    case UPLOAD_FILE_WRITE:
      if (uploadRejected) break;
      esp_task_wdt_reset();   // the whole upload runs inside one handleClient()
      ledUpdate(millis());
      if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) {
        Update.printError(Serial);
      }
      if (up.totalSize / 1024 >= lastShownKb + 64) {
        lastShownKb = up.totalSize / 1024;
        char msg[32];
        snprintf(msg, sizeof(msg), "%u KB received", (unsigned)lastShownKb);
        displayOta(msg, -1);
      }
      break;

    case UPLOAD_FILE_END:
      if (uploadRejected) break;
      if (!Update.end(true)) Update.printError(Serial);
      break;

    case UPLOAD_FILE_ABORTED:
      if (uploadRejected) break;
      Update.abort();
      break;

    default:
      break;
  }
}

static void handleUploadDone() {
  const bool started = uploadStarted, rejected = uploadRejected;
  uploadStarted = uploadRejected = false;

  if (!started) {
    if (!authRequire(server)) return;
    server.send(400, "text/plain", "No firmware file in the request");
    return;
  }
  if (rejected) {
    server.send(401, "text/plain", "Not logged in - nothing was written");
    return;
  }

  // Nothing written at all (aborted before Update.begin) also counts as failed.
  const bool ok = !Update.hasError() && Update.isFinished();
  server.sendHeader("Connection", "close");
  if (ok) {
    displayOta("Update OK - rebooting", 100);
    server.send(200, "text/plain", "Update OK - rebooting. This page reloads in 15 s.");
    scheduleRestart();
  } else {
    displayOta("Update FAILED", -1);
    server.send(500, "text/plain", "Update failed - the current firmware keeps running.");
    otaSetActive(false);   // control resumes on the next loop pass
  }
}

// ----------------------------------------------------------------------------
void webBegin() {
  authBegin(server);

  server.on("/", HTTP_GET, []() { sendPage(INDEX_HTML, true); });
  server.on("/settings", HTTP_GET, []() { sendPage(SETTINGS_HTML, true); });
  server.on("/update", HTTP_GET, []() { sendPage(UPDATE_HTML, true); });
  server.on("/login", HTTP_GET, []() { sendPage(LOGIN_HTML, false); });
  server.on("/app.css", HTTP_GET, []() { sendStatic("text/css", APP_CSS); });
  server.on("/common.js", HTTP_GET, []() { sendStatic("application/javascript", JS_COMMON); });

  server.on("/login", HTTP_POST, []() { authHandleLogin(server); });
  server.on("/logout", HTTP_POST, []() { authHandleLogout(server); });

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/relay", HTTP_POST, handleRelay);
  server.on("/api/room", HTTP_POST, handleRoom);
  server.on("/api/setpoints", HTTP_POST, handleSetpoints);
  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/wifi", HTTP_POST, handleWifiPost);
  server.on("/api/password", HTTP_POST, []() { authHandleChangePassword(server); });
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/update", HTTP_POST, handleUploadDone, handleUploadData);

  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void webLoop() {
  server.handleClient();

  // Let the HTTP reply leave before rebooting.
  if (restartPending && millis() - restartRequested >= 1000) {
    controlAllOff();
    controlSaveRuntime();
    ESP.restart();
  }
}
