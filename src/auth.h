#pragma once
#include <WebServer.h>

// ============================================================================
// WEB UI AUTHENTICATION
// ============================================================================
// One password guards the whole browser surface: the control endpoints, the
// settings page and the firmware upload. It is NOT the WiFi password, and it is
// NOT OTA_PASSWORD - those are a link-layer secret and a compiled-in break-glass
// credential respectively, and each has a different lifetime.
//
// This device is a heater switch first. Authentication is a gate on the way in,
// not a subsystem: it lives entirely in this file, and every handler that needs
// it calls exactly one function.
//
// STORED AS A SALTED HASH. The plaintext never reaches NVS, so a flash dump
// yields nothing reusable. Sessions live in RAM only, which means a reboot logs
// everyone out - that is deliberate, and it is free.
void authBegin(WebServer& server);

// True while the password is still the shipped default. Drives the nag banner
// on the control page; nothing is blocked by it. Blocking would be one more way
// to be locked out of a board with no USB port.
bool authPasswordIsDefault();

// ----------------------------------------------------------------------------
// THE GATE
// ----------------------------------------------------------------------------
// Called at the top of every handler that is not a static asset. Returns true
// if the request may proceed. On false it has ALREADY sent the response, so the
// caller must return immediately without touching anything.
//
// Two checks, in order:
//
//   1. HOST ALLOWLIST - always applied. Only the board's own addresses are
//      accepted. Without this a DNS rebinding attack lets a page on the public
//      internet talk to this board as though it were same-origin, reading the
//      replies as well as sending the requests.
//   2. SESSION - applied when needSession, which is everything except the few
//      routes that are deliberately public.
//
// needSession=false is for /status and for switching an output OFF. Turning
// something off must never require credentials: no expired session, forgotten
// password or bug in this file may leave 2200 W stranded on.
bool requireAuth(WebServer& server, bool needSession = true);

// The same two checks, answering nothing. The firmware upload handler needs
// this: it runs as the request body streams in, where there is no way to send a
// reply yet, so it records the refusal and lets the result handler send the
// 401 once the body has been consumed.
bool authCheck(WebServer& server, bool needSession = true);

// ----------------------------------------------------------------------------
// SESSION LIFECYCLE
// ----------------------------------------------------------------------------
// Verifies the password and, on success, issues a session cookie. Returns false
// on a wrong password or while the failure lockout is running; it has already
// answered the request in both cases.
bool authLogin(WebServer& server);
void authLogout(WebServer& server);

// Requires the current password, so a browser someone walked away from cannot
// be used to take the board over. Returns false and answers the request itself
// if the current password is wrong or the new one is shorter than 8 characters.
bool authChangePassword(WebServer& server);

// Back to the shipped default, every session dropped. Called by the IO0 button
// and by nothing else - there is no network path to this.
void authFactoryReset();

// The compiled-in default, for the reset confirmation on the TFT. Reading it
// from here rather than repeating the literal means the screen cannot end up
// announcing a password that secrets.h no longer sets.
const char* authDefaultPassword();
