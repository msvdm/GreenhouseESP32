#pragma once
#include <WebServer.h>

// ============================================================================
// WEB INTERFACE
// ============================================================================
// Every page is a static shell served straight from flash with send_P: no
// String building, no heap churn per request. All dynamic values are fetched
// by the client from /status, which is the single source of truth - the UI
// polled it anyway, so rendering the same values into the HTML server-side
// was pure duplicate work.

void webBegin();
void webLoop();

// Exposed so the OTA module can register its own routes on the same server.
WebServer& webServer();
