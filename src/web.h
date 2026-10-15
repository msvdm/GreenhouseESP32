#pragma once

// ============================================================================
// WEB UI - static pages, JSON API, browser firmware upload
// ============================================================================
// Pages are static (PROGMEM); everything live comes from /api/status.

void webBegin();
void webLoop();
