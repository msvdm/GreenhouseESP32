#pragma once
#include <Arduino.h>
#include <WebServer.h>

// ============================================================================
// WEB UI PASSWORD AND SESSIONS
// ============================================================================
// One password (salted SHA-256 in NVS, same format as firmware 7.x), and a
// random session cookie (HttpOnly, SameSite=Strict) after logging in.

void authBegin(WebServer& server);
bool authIsDefault();

bool authLoggedIn(WebServer& server);
// Logged in, or answers 401 and returns false.
bool authRequire(WebServer& server);

void authHandleLogin(WebServer& server);
void authHandleLogout(WebServer& server);
void authHandleChangePassword(WebServer& server);

void authFactoryReset();   // password back to UI_PASSWORD, all sessions ended
