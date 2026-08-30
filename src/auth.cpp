#include "auth.h"
#include <Preferences.h>
#include <esp_system.h>
#include "mbedtls/md.h"
#include "config.h"
#include "net.h"
#include "secrets.h"

// secrets.h is gitignored, so a fresh clone may not carry this key. The default
// is deliberately obvious and the UI nags until it is changed - the same bargain
// a consumer router makes.
#ifndef UI_PASSWORD
#define UI_PASSWORD "GreenAdmin"
#endif

#define UI_SALT_LEN 8
#define UI_HASH_LEN 32
#define UI_PASS_MIN 8

// Four concurrent logins - a phone and a laptop, with room to spare. Sessions
// live here and nowhere else: a reboot ends all of them, which costs an operator
// one login and denies an attacker anything durable to steal.
#define SESSION_SLOTS 4
#define SESSION_TOKEN_LEN 32              // 16 random bytes as hex
#define SESSION_IDLE_TIMEOUT 1800000UL    // 30 min without a request

// Failed logins are throttled by REFUSING, never by sleeping. A delay() in a
// handler stalls the 1 Hz heater-zone read and safetyTick() with it - the same
// reason a blocking WiFi scan is banned.
#define AUTH_MAX_FAILURES 5
#define AUTH_LOCKOUT_MS 60000UL

#define COOKIE_NAME "gh"

struct Session {
  char token[SESSION_TOKEN_LEN + 1];
  unsigned long lastSeen;
  bool used;
};

static Session sessions[SESSION_SLOTS];
static Preferences prefs;
static const char* NVS_NAMESPACE = "greenhouse";

static bool passwordIsDefault = true;

static uint8_t failures = 0;
static unsigned long lockStart = 0;
static bool lockActive = false;

// ----------------------------------------------------------------------------
// PRIMITIVES
// ----------------------------------------------------------------------------
// Length-independent so a mismatched length cannot be distinguished from a
// mismatched byte. Wi-Fi jitter swamps this signal in practice; it costs one
// line to not have to reason about that.
static bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// mbedtls_md() rather than the mbedtls_sha256_* family: the latter renamed its
// functions between mbedtls 2.x and 3.x, and this wrapper did not. SHA-256 is
// hardware-accelerated on this chip either way.
static void hashPassword(const uint8_t* salt, const char* password, uint8_t* out) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, salt, UI_SALT_LEN);
  mbedtls_md_update(&ctx, (const uint8_t*)password, strlen(password));
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

static void randomBytes(uint8_t* out, size_t len) {
  // esp_random() is a true hardware RNG whenever the radio is running, and the
  // radio is always running here - the access point is never taken down.
  for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(esp_random() & 0xFF);
}

static void storePassword(const char* password, bool isDefault) {
  uint8_t salt[UI_SALT_LEN];
  uint8_t hash[UI_HASH_LEN];
  randomBytes(salt, UI_SALT_LEN);
  hashPassword(salt, password, hash);

  prefs.begin(NVS_NAMESPACE, false);
  prefs.putBytes("ui_salt", salt, UI_SALT_LEN);
  prefs.putBytes("ui_hash", hash, UI_HASH_LEN);
  prefs.putBool("ui_def", isDefault);
  prefs.end();

  passwordIsDefault = isDefault;
}

static bool passwordMatches(const char* candidate) {
  uint8_t salt[UI_SALT_LEN];
  uint8_t stored[UI_HASH_LEN];

  prefs.begin(NVS_NAMESPACE, true);
  const size_t gotSalt = prefs.getBytes("ui_salt", salt, UI_SALT_LEN);
  const size_t gotHash = prefs.getBytes("ui_hash", stored, UI_HASH_LEN);
  prefs.end();

  // Nothing stored is not "let anyone in". authBegin() seeds these on first
  // boot, so the only way to arrive here empty is a half-written NVS.
  if (gotSalt != UI_SALT_LEN || gotHash != UI_HASH_LEN) return false;

  uint8_t computed[UI_HASH_LEN];
  hashPassword(salt, candidate, computed);
  return constantTimeEqual(computed, stored, UI_HASH_LEN);
}

// ----------------------------------------------------------------------------
// SESSIONS
// ----------------------------------------------------------------------------
static void dropAllSessions() {
  for (Session& s : sessions) s.used = false;
}

// Elapsed difference, never (now - TIMEOUT): the latter underflows for the
// first half hour after boot and again at the millis() wrap.
static bool sessionExpired(const Session& s, unsigned long now) {
  return (now - s.lastSeen) >= SESSION_IDLE_TIMEOUT;
}

static Session* findSession(const char* token, unsigned long now) {
  for (Session& s : sessions) {
    if (!s.used) continue;
    if (sessionExpired(s, now)) { s.used = false; continue; }
    if (constantTimeEqual((const uint8_t*)s.token, (const uint8_t*)token, SESSION_TOKEN_LEN)) {
      return &s;
    }
  }
  return nullptr;
}

// Reuses the oldest slot when all four are taken, so a fifth login evicts a
// stale phone rather than being refused.
static Session* claimSlot(unsigned long now) {
  Session* oldest = &sessions[0];
  for (Session& s : sessions) {
    if (!s.used) return &s;
    if ((now - s.lastSeen) > (now - oldest->lastSeen)) oldest = &s;
  }
  return oldest;
}

// The Cookie header carries every cookie for the host, so a plain indexOf would
// also match "notgh=..." - the name has to start the header or follow a
// separator.
static bool cookieToken(WebServer& server, char* out) {
  const String cookies = server.header(F("Cookie"));
  const String name = F(COOKIE_NAME "=");

  int at = cookies.indexOf(name);
  while (at >= 0) {
    const bool atStart = (at == 0);
    const char before = atStart ? '\0' : cookies.charAt(at - 1);
    if (atStart || before == ';' || before == ' ') {
      const int from = at + name.length();
      if ((int)cookies.length() - from < SESSION_TOKEN_LEN) return false;
      cookies.substring(from, from + SESSION_TOKEN_LEN).toCharArray(out, SESSION_TOKEN_LEN + 1);
      return true;
    }
    at = cookies.indexOf(name, at + 1);
  }
  return false;
}

static void issueSession(WebServer& server, unsigned long now) {
  uint8_t raw[SESSION_TOKEN_LEN / 2];
  randomBytes(raw, sizeof(raw));

  Session* s = claimSlot(now);
  for (size_t i = 0; i < sizeof(raw); i++) {
    snprintf(&s->token[i * 2], 3, "%02x", raw[i]);
  }
  s->lastSeen = now;
  s->used = true;

  // SameSite=Strict is the CSRF defence and the reason no per-request token is
  // needed: a request originating from any other site simply does not carry
  // this cookie. HttpOnly keeps it away from any script that gets injected.
  //
  // Deliberately a session cookie with no Max-Age. SESSION_IDLE_TIMEOUT above
  // is the real control and it SLIDES, so an expiry baked into the cookie would
  // only log out an operator who was still using the page.
  String cookie = F(COOKIE_NAME "=");
  cookie += s->token;
  cookie += F("; Path=/; HttpOnly; SameSite=Strict");
  server.sendHeader(F("Set-Cookie"), cookie);
}

// ----------------------------------------------------------------------------
// HOST ALLOWLIST
// ----------------------------------------------------------------------------
// Answers "was this request addressed to one of MY names?". A browser tricked
// into rebinding an attacker domain onto this board sends that domain in Host,
// which matches nothing here.
static bool hostAllowed(WebServer& server) {
  String host = server.hostHeader();
  if (host.length() == 0) return false;          // HTTP/1.1 requires it

  const int colon = host.indexOf(':');           // strip any :port
  if (colon >= 0) host = host.substring(0, colon);
  host.toLowerCase();

  if (host == netApAddress()) return true;

  const String sta = netStaAddress();
  if (sta.length() > 0 && host == sta) return true;

  const char* name = netHostname();
  if (name[0]) {
    String h(name);
    h.toLowerCase();
    if (host == h || host == h + F(".local")) return true;
  }
  return false;
}

// ----------------------------------------------------------------------------
// PUBLIC INTERFACE
// ----------------------------------------------------------------------------
void authBegin(WebServer& server) {
  dropAllSessions();

  prefs.begin(NVS_NAMESPACE, true);
  const bool haveHash = prefs.isKey("ui_hash");
  passwordIsDefault = prefs.getBool("ui_def", true);
  prefs.end();

  // First boot, or NVS erased. Seed the default so every later check is a plain
  // hash comparison with no "nothing stored yet" special case to get wrong.
  if (!haveHash) {
    storePassword(UI_PASSWORD, true);
    Serial.println(F("[auth] No password stored - seeded with the default"));
  }

  // WebServer discards every header it was not told to keep, and the gate needs
  // this one. Note this REPLACES any previous list, so it must be the only
  // collectHeaders() call in the firmware.
  static const char* AUTH_HEADERS[] = { "Cookie" };
  server.collectHeaders(AUTH_HEADERS, 1);

  Serial.printf("[auth] Web UI password %s\n",
                passwordIsDefault ? "is STILL THE DEFAULT" : "set");
}

bool authPasswordIsDefault() { return passwordIsDefault; }

bool authCheck(WebServer& server, bool needSession) {
  if (!hostAllowed(server)) return false;
  if (!needSession) return true;

  const unsigned long now = millis();
  char token[SESSION_TOKEN_LEN + 1];
  if (!cookieToken(server, token)) return false;

  Session* s = findSession(token, now);
  if (!s) return false;

  s->lastSeen = now;                     // idle timeout is sliding, not absolute
  return true;
}

bool requireAuth(WebServer& server, bool needSession) {
  if (!hostAllowed(server)) {
    server.send(403, F("application/json"), F("{\"error\":\"bad host\"}"));
    return false;
  }
  if (authCheck(server, needSession)) return true;

  server.send(401, F("application/json"), F("{\"error\":\"login required\"}"));
  return false;
}

bool authLogin(WebServer& server) {
  if (!hostAllowed(server)) {
    server.send(403, F("application/json"), F("{\"error\":\"bad host\"}"));
    return false;
  }

  const unsigned long now = millis();

  if (lockActive) {
    if ((now - lockStart) < AUTH_LOCKOUT_MS) {
      server.send(429, F("application/json"), F("{\"error\":\"too many attempts\"}"));
      return false;
    }
    lockActive = false;
    failures = 0;
  }

  if (!passwordMatches(server.arg(F("pass")).c_str())) {
    if (++failures >= AUTH_MAX_FAILURES) {
      lockActive = true;
      lockStart = now;
      failures = 0;
      Serial.println(F("[auth] Too many failed logins - refusing for 60 s"));
    }
    server.send(401, F("application/json"), F("{\"error\":\"wrong password\"}"));
    return false;
  }

  failures = 0;
  issueSession(server, now);
  server.send(200, F("application/json"), F("{\"ok\":1}"));
  return true;
}

void authLogout(WebServer& server) {
  const unsigned long now = millis();
  char token[SESSION_TOKEN_LEN + 1];
  if (cookieToken(server, token)) {
    Session* s = findSession(token, now);
    if (s) s->used = false;
  }
  server.sendHeader(F("Set-Cookie"),
                    F(COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"));
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

bool authChangePassword(WebServer& server) {
  const String next = server.arg(F("next"));

  if (!passwordMatches(server.arg(F("cur")).c_str())) {
    server.send(401, F("application/json"), F("{\"error\":\"wrong current password\"}"));
    return false;
  }
  if (next.length() < UI_PASS_MIN) {
    server.send(400, F("application/json"), F("{\"error\":\"at least 8 characters\"}"));
    return false;
  }

  storePassword(next.c_str(), false);

  // Every other browser holding a session was authorised under the old
  // password. Changing it is how an operator responds to thinking someone else
  // has it, so those sessions have to go - including, deliberately, this one.
  dropAllSessions();
  server.sendHeader(F("Set-Cookie"),
                    F(COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"));
  server.send(200, F("application/json"), F("{\"ok\":1}"));
  Serial.println(F("[auth] Web UI password changed - all sessions ended"));
  return true;
}

const char* authDefaultPassword() { return UI_PASSWORD; }

void authFactoryReset() {
  storePassword(UI_PASSWORD, true);
  dropAllSessions();
  failures = 0;
  lockActive = false;
  Serial.println(F("[auth] Password reset to default, all sessions ended"));
}
