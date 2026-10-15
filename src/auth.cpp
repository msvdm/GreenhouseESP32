#include "auth.h"
#include <Preferences.h>
#include <esp_system.h>
#include "mbedtls/md.h"
#include "secrets.h"

#ifndef UI_PASSWORD
#define UI_PASSWORD "GreenAdmin"
#endif

#define SALT_LEN 8
#define HASH_LEN 32
#define TOKEN_LEN 32                    // 16 random bytes as hex
#define SESSIONS 4
#define SESSION_IDLE 43200000UL         // 12 h without a request
#define MAX_FAILS 5
#define LOCKOUT_MS 60000UL

struct Session {
  char token[TOKEN_LEN + 1];
  unsigned long lastSeen;
  bool used;
};

static Session sessions[SESSIONS];
static Preferences prefs;
static bool isDefault = true;
static uint8_t fails = 0;
static unsigned long lockedAt = 0;

static void sha256(const uint8_t* salt, const char* pw, uint8_t* out) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, salt, SALT_LEN);
  mbedtls_md_update(&ctx, (const uint8_t*)pw, strlen(pw));
  mbedtls_md_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

static bool sameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

static void storePassword(const char* pw, bool dflt) {
  uint8_t salt[SALT_LEN], hash[HASH_LEN];
  esp_fill_random(salt, SALT_LEN);
  sha256(salt, pw, hash);
  prefs.begin("greenhouse", false);
  prefs.putBytes("ui_salt", salt, SALT_LEN);
  prefs.putBytes("ui_hash", hash, HASH_LEN);
  prefs.putBool("ui_def", dflt);
  prefs.end();
  isDefault = dflt;
}

static bool passwordOk(const char* pw) {
  uint8_t salt[SALT_LEN], stored[HASH_LEN], calc[HASH_LEN];
  prefs.begin("greenhouse", true);
  const bool ok = prefs.getBytes("ui_salt", salt, SALT_LEN) == SALT_LEN &&
                  prefs.getBytes("ui_hash", stored, HASH_LEN) == HASH_LEN;
  prefs.end();
  if (!ok) return false;
  sha256(salt, pw, calc);
  return sameBytes(calc, stored, HASH_LEN);
}

static void endAllSessions() {
  for (Session& s : sessions) s.used = false;
}

// Extract our "gh=<token>" from the Cookie header.
static bool cookieToken(WebServer& server, char* out) {
  const String c = server.header("Cookie");
  int at = 0;
  while ((at = c.indexOf("gh=", at)) >= 0) {
    if (at == 0 || c[at - 1] == ' ' || c[at - 1] == ';') {
      const int from = at + 3;
      if ((int)c.length() - from < TOKEN_LEN) return false;
      c.substring(from, from + TOKEN_LEN).toCharArray(out, TOKEN_LEN + 1);
      return true;
    }
    at += 3;
  }
  return false;
}

static Session* findSession(WebServer& server) {
  char token[TOKEN_LEN + 1];
  if (!cookieToken(server, token)) return nullptr;
  const unsigned long now = millis();
  for (Session& s : sessions) {
    if (!s.used) continue;
    if (now - s.lastSeen >= SESSION_IDLE) { s.used = false; continue; }
    if (sameBytes((const uint8_t*)s.token, (const uint8_t*)token, TOKEN_LEN)) return &s;
  }
  return nullptr;
}

// ----------------------------------------------------------------------------
void authBegin(WebServer& server) {
  endAllSessions();
  prefs.begin("greenhouse", true);
  const bool have = prefs.isKey("ui_hash");
  isDefault = prefs.getBool("ui_def", true);
  prefs.end();
  if (!have) storePassword(UI_PASSWORD, true);

  static const char* headers[] = { "Cookie" };
  server.collectHeaders(headers, 1);
}

bool authIsDefault() { return isDefault; }

bool authLoggedIn(WebServer& server) {
  Session* s = findSession(server);
  if (!s) return false;
  s->lastSeen = millis();
  return true;
}

bool authRequire(WebServer& server) {
  if (authLoggedIn(server)) return true;
  server.send(401, "application/json", "{\"error\":\"login required\"}");
  return false;
}

void authHandleLogin(WebServer& server) {
  const unsigned long now = millis();
  if (fails >= MAX_FAILS) {
    if (now - lockedAt < LOCKOUT_MS) {
      server.send(429, "application/json", "{\"error\":\"Too many attempts - wait a minute\"}");
      return;
    }
    fails = 0;
  }
  if (!passwordOk(server.arg("pass").c_str())) {
    if (++fails >= MAX_FAILS) lockedAt = now;
    server.send(401, "application/json", "{\"error\":\"Wrong password\"}");
    return;
  }
  fails = 0;

  // Reuse a free slot, else evict the least recently used one.
  Session* slot = &sessions[0];
  for (Session& s : sessions) {
    if (!s.used) { slot = &s; break; }
    if (now - s.lastSeen > now - slot->lastSeen) slot = &s;
  }
  uint8_t raw[TOKEN_LEN / 2];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) snprintf(&slot->token[i * 2], 3, "%02x", raw[i]);
  slot->used = true;
  slot->lastSeen = now;

  server.sendHeader("Set-Cookie", String("gh=") + slot->token + "; Path=/; HttpOnly; SameSite=Strict");
  server.send(200, "application/json", "{\"ok\":1}");
}

void authHandleLogout(WebServer& server) {
  Session* s = findSession(server);
  if (s) s->used = false;
  server.sendHeader("Set-Cookie", "gh=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  server.send(200, "application/json", "{\"ok\":1}");
}

void authHandleChangePassword(WebServer& server) {
  if (!authRequire(server)) return;
  const String next = server.arg("next");
  if (!passwordOk(server.arg("cur").c_str())) {
    server.send(400, "application/json", "{\"error\":\"Current password is wrong\"}");
    return;
  }
  if (next.length() < 8) {
    server.send(400, "application/json", "{\"error\":\"New password needs at least 8 characters\"}");
    return;
  }
  storePassword(next.c_str(), false);
  endAllSessions();
  server.sendHeader("Set-Cookie", "gh=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  server.send(200, "application/json", "{\"ok\":1}");
}

void authFactoryReset() {
  storePassword(UI_PASSWORD, true);
  endAllSessions();
  fails = 0;
}
