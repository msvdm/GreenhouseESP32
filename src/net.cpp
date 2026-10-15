#include "net.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include "control.h"
#include "display.h"
#include "led.h"
#include "secrets.h"

#ifndef OTA_PASSWORD
#error "Define OTA_PASSWORD in src/secrets.h (see secrets.h.example)"
#endif

static WifiSettings cfg;
static bool staWasUp = false;
static bool updating = false;

bool otaActive() { return updating; }
void otaSetActive(bool active) { updating = active; }

static void startMdns() {
  MDNS.end();
  if (!MDNS.begin(cfg.hostname)) {
    Serial.println(F("[net] mDNS failed"));
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.enableArduino(3232, true);   // espota discovery
}

static void otaBegin() {
  // net.cpp owns mDNS; ArduinoOTA would otherwise start its own responder.
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.setHostname(cfg.hostname);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    updating = true;
    controlAllOff();
    controlSaveRuntime();
    displayOta("Receiving firmware", -1);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    esp_task_wdt_reset();   // the whole transfer runs inside ArduinoOTA.handle()
    ledUpdate(millis());
    displayOta("Receiving firmware", total ? (int)(done * 100UL / total) : -1);
  });
  ArduinoOTA.onEnd([]() { displayOta("Update OK - rebooting", 100); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[ota] error %u\n", e);
    displayOta("Update FAILED", -1);
    updating = false;
  });
  ArduinoOTA.begin();
}

void netBegin() {
  wifiLoad(cfg);

  WiFi.persistent(false);   // our own NVS keys are the source of truth
  WiFi.mode(cfg.staEnabled ? WIFI_AP_STA : WIFI_AP);
  WiFi.setHostname(cfg.hostname);
  WiFi.softAP(cfg.apSsid, cfg.apPass[0] ? cfg.apPass : nullptr);
  Serial.printf("[net] AP '%s' at %s\n", cfg.apSsid, WiFi.softAPIP().toString().c_str());

  if (cfg.staEnabled) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.staSsid, cfg.staPass);
    Serial.printf("[net] joining '%s'\n", cfg.staSsid);
  }

  startMdns();
  otaBegin();
}

void netLoop() {
  ArduinoOTA.handle();

  // Re-announce mDNS once the home-network link has an address.
  const bool up = netStaConnected();
  if (up != staWasUp) {
    staWasUp = up;
    if (up) {
      Serial.printf("[net] connected, IP %s\n", WiFi.localIP().toString().c_str());
      startMdns();
    } else {
      Serial.println(F("[net] home network lost"));
    }
  }
}

const WifiSettings& netConfig() { return cfg; }

void netSave(const WifiSettings& w) {
  cfg = w;
  wifiSave(cfg);
}

bool netStaConnected() { return cfg.staEnabled && WiFi.status() == WL_CONNECTED; }
String netStaIp() { return netStaConnected() ? WiFi.localIP().toString() : String(); }
String netApIp() { return WiFi.softAPIP().toString(); }
String netDisplayAddress() { return netStaConnected() ? netStaIp() : netApIp(); }
