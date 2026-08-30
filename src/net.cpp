#include "net.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include "display.h"

// committed - what is in NVS, and what the board will boot on.
// live      - what the radio is running right now.
// During an AP trial the two differ, and only netConfirm() closes the gap.
static WifiConfig committed;
static WifiConfig live;

static bool trialRunning = false;
static unsigned long trialStart = 0;

static bool applyPending = false;
static unsigned long applyRequestedAt = 0;
static WifiConfig candidate;

// Scan results go stale, and a network list from ten minutes ago is worse than
// no list because it looks current. Dropped after this long.
#define SCAN_RESULT_TTL 60000UL
static unsigned long scanFinishedAt = 0;

// Edge detection for the station link, so mDNS can be re-announced once the
// interface actually has an address to announce.
static bool staWasConnected = false;

// ----------------------------------------------------------------------------
// mDNS
// ----------------------------------------------------------------------------
// net.cpp owns mDNS outright, and ota.cpp is told not to start its own.
// ArduinoOTA::begin() calls MDNS.begin() internally with its own hostname, so
// without that hand-off the two would race over one responder and the hostname
// configured on the settings page would sometimes lose.
//
// MDNS.enableArduino() is what puts the OTA service back on the wire, and it is
// not optional: dropping it removes espota discovery from a board that has no
// USB port and therefore no other recovery path. The pinned upload in
// platformio.ini targets a literal address and would still work, but breaking
// discovery as a side effect of a UI change is not a trade worth making.
#define OTA_PORT 3232

static void applyMdns() {
  MDNS.end();
  if (!live.mdnsEnabled || live.hostname[0] == 0) return;

  if (!MDNS.begin(live.hostname)) {
    Serial.println(F("[mDNS] responder failed to start"));
    return;
  }
  MDNS.enableArduino(OTA_PORT, true);      // true: an OTA password is set
  MDNS.addService("http", "tcp", 80);
  Serial.printf("[mDNS] announcing %s.local\n", live.hostname);
}

const char* netHostname() {
  return live.mdnsEnabled ? live.hostname : "";
}

// ----------------------------------------------------------------------------
// RADIO
// ----------------------------------------------------------------------------
// Split in two deliberately. The old single applyRadio() tore the access point
// down on every change, which meant joining a home network cost the operator
// their AP and a 60-second confirmation for no reason at all.

static void applyApRadio(const WifiConfig& cfg) {
  // Tear the AP down and bring it back so clients are forced to reassociate
  // with the new credentials. That is not a side effect, it IS the test: if
  // the operator cannot get back on, the trial should fail.
  WiFi.softAPdisconnect(false);
  delay(100);

  IPAddress ip;
  if (ip.fromString(cfg.apIp)) {
    // Gateway is the board itself; a /24 keeps the DHCP pool on the same net.
    WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  }
  WiFi.softAP(cfg.apSsid, cfg.apPass[0] ? cfg.apPass : nullptr);

  Serial.printf("[WiFi] AP %s at %s\n", cfg.apSsid,
                WiFi.softAPIP().toString().c_str());
  displayNetwork(WiFi.softAPIP().toString());
}

static void applyStaRadio(const WifiConfig& cfg) {
  // Never touches softAP(). Changing the mode to AP_STA and back leaves the
  // access point up, which is the whole reason the station side needs no trial.
  WiFi.mode(cfg.staEnabled ? WIFI_AP_STA : WIFI_AP);

  if (cfg.staEnabled) {
    WiFi.begin(cfg.staSsid, cfg.staPass);
    Serial.printf("[WiFi] STA %s connecting\n", cfg.staSsid);
  } else {
    WiFi.disconnect(false, true);
    Serial.println(F("[WiFi] STA off"));
  }
  staWasConnected = false;
}

// Everything, in the order the radio wants it: mode, then AP, then station.
static void applyAll(const WifiConfig& cfg) {
  live = cfg;
  applyApRadio(cfg);
  applyStaRadio(cfg);
  applyMdns();
}

// ----------------------------------------------------------------------------
void netBegin() {
  Serial.println(F("\n[WiFi] Starting network"));
  loadWifiConfig(committed);
  applyAll(committed);
}

void netLoop(unsigned long now) {
  // A queued AP change waits briefly so the browser gets its acknowledgement
  // before the access point drops it.
  if (applyPending && (now - applyRequestedAt) >= WIFI_TRIAL_APPLY_DELAY) {
    applyPending = false;
    trialRunning = true;
    trialStart = now;
    Serial.printf("[WiFi] AP trial started - %lus to confirm\n",
                  WIFI_TRIAL_WINDOW / 1000);
    applyAll(candidate);
    return;
  }

  // Re-announce once the station link actually comes up: the responder has no
  // address to publish on that interface until it does, so a name registered
  // at boot covers the access point only.
  const bool staUp = netStaConnected();
  if (staUp != staWasConnected) {
    staWasConnected = staUp;
    if (staUp) {
      Serial.printf("[WiFi] STA connected at %s\n", WiFi.localIP().toString().c_str());
      applyMdns();
    }
  }

  // Stale scan results are worse than none, because they look current.
  if (scanFinishedAt != 0 && (now - scanFinishedAt) >= SCAN_RESULT_TTL) {
    scanFinishedAt = 0;
    WiFi.scanDelete();
  }

  if (!trialRunning) return;

  // Elapsed difference, never (now - WINDOW): the latter underflows for the
  // first minute after boot and at the millis() wrap.
  if ((now - trialStart) < WIFI_TRIAL_WINDOW) return;

  trialRunning = false;
  Serial.println(F("[WiFi] Not confirmed in time - reverting to previous settings"));
  applyAll(committed);
}

// ----------------------------------------------------------------------------
// APPLYING CHANGES
// ----------------------------------------------------------------------------
bool netApplyAp(const WifiConfig& c) {
  // Start from what is running now and take only the AP fields, so an AP edit
  // can never disturb a station connection as a side effect.
  WifiConfig cfg = live;
  strlcpy(cfg.apSsid, c.apSsid, WIFI_SSID_LEN);
  strlcpy(cfg.apPass, c.apPass, WIFI_PASS_LEN);
  strlcpy(cfg.apIp, c.apIp, WIFI_ADDR_LEN);
  strlcpy(cfg.hostname, c.hostname, WIFI_HOST_LEN);
  cfg.mdnsEnabled = c.mdnsEnabled;

  // Untrusted input from a form field. A configuration that cannot be made
  // valid is refused rather than quietly corrected into something else.
  if (!validateWifiConfig(cfg)) return false;

  candidate = cfg;
  applyRequestedAt = millis();
  applyPending = true;
  return true;
}

bool netApplySta(const WifiConfig& c) {
  WifiConfig cfg = live;
  cfg.staEnabled = c.staEnabled;
  strlcpy(cfg.staSsid, c.staSsid, WIFI_SSID_LEN);
  strlcpy(cfg.staPass, c.staPass, WIFI_PASS_LEN);

  if (!validateWifiConfig(cfg)) return false;

  // Committed straight away. There is no trial to run: the access point is
  // untouched, so the operator's way back in is exactly where it was.
  live = cfg;
  applyStaRadio(cfg);
  applyMdns();
  committed = cfg;
  saveWifiConfig(committed);

  // A trial in flight covers the AP only, so an unconfirmed AP candidate must
  // not be quietly promoted by a station edit. Copy the station fields into it
  // and let the AP side keep running out its own clock.
  if (trialRunning || applyPending) {
    candidate.staEnabled = cfg.staEnabled;
    strlcpy(candidate.staSsid, cfg.staSsid, WIFI_SSID_LEN);
    strlcpy(candidate.staPass, cfg.staPass, WIFI_PASS_LEN);
  }
  return true;
}

bool netFactoryReset() {
  // Deliberately runs as a trial like any other AP change: a misfired reset is
  // then exactly as recoverable as a mistyped password.
  candidate = factoryWifiConfig();
  if (!validateWifiConfig(candidate)) return false;
  applyRequestedAt = millis();
  applyPending = true;
  return true;
}

bool netConfirm() {
  if (!trialRunning && !applyPending) return false;
  trialRunning = false;
  applyPending = false;
  committed = live;
  saveWifiConfig(committed);
  return true;
}

bool netRevert() {
  if (!trialRunning && !applyPending) return false;
  const bool reachedRadio = trialRunning;
  trialRunning = false;
  applyPending = false;
  Serial.println(F("[WiFi] Reverted by the operator"));
  // A candidate still waiting out its apply delay never reached the radio, so
  // there is nothing to put back.
  if (reachedRadio) applyAll(committed);
  return true;
}

void netFactoryResetNow() {
  // Any trial in flight is abandoned rather than left to fire later against a
  // configuration it was never started for.
  trialRunning = false;
  applyPending = false;

  committed = factoryWifiConfig();
  validateWifiConfig(committed);
  saveWifiConfig(committed);
  applyAll(committed);

  Serial.println(F("[WiFi] Factory settings applied and committed"));
}

unsigned long netTrialSecondsLeft() {
  if (applyPending)  return WIFI_TRIAL_WINDOW / 1000;
  if (!trialRunning) return 0;
  const unsigned long elapsed = millis() - trialStart;
  if (elapsed >= WIFI_TRIAL_WINDOW) return 0;
  return (WIFI_TRIAL_WINDOW - elapsed) / 1000;
}

const WifiConfig& netLiveConfig() { return live; }

bool netStaConnected() { return live.staEnabled && WiFi.status() == WL_CONNECTED; }

String netStaAddress() {
  return netStaConnected() ? WiFi.localIP().toString() : String("");
}

String netApAddress() { return WiFi.softAPIP().toString(); }

// ----------------------------------------------------------------------------
// SSID SCAN
// ----------------------------------------------------------------------------
void netStartScan() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  WiFi.scanDelete();
  scanFinishedAt = 0;
  // async = true. See the note in net.h: a blocking scan here would stall the
  // heater-zone read for seconds at a time.
  WiFi.scanNetworks(true, false);
}

int netScanState() {
  const int n = WiFi.scanComplete();
  if (n >= 0 && scanFinishedAt == 0) scanFinishedAt = millis();
  return n;
}

// ----------------------------------------------------------------------------
// ASSOCIATED CLIENTS
// ----------------------------------------------------------------------------
int netApClients(ApClient* out, int max) {
  wifi_sta_list_t wifiList;
  esp_netif_sta_list_t netifList;

  if (esp_wifi_ap_get_sta_list(&wifiList) != ESP_OK) return 0;
  if (esp_netif_get_sta_list(&wifiList, &netifList) != ESP_OK) return 0;

  int written = 0;
  for (int i = 0; i < netifList.num && written < max; i++) {
    const esp_netif_sta_info_t& sta = netifList.sta[i];
    snprintf(out[written].mac, sizeof(out[written].mac),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             sta.mac[0], sta.mac[1], sta.mac[2], sta.mac[3], sta.mac[4], sta.mac[5]);
    snprintf(out[written].ip, sizeof(out[written].ip), "%s",
             IPAddress(sta.ip.addr).toString().c_str());
    written++;
  }
  return written;
}
