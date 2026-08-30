#include "ota.h"
#include "net.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "auth.h"
#include "config.h"
#include "control.h"
#include "display.h"
#include "secrets.h"

static bool updating = false;

// Set when an upload is refused, so the write path stays shut for the rest of
// the request and the result handler can answer 401 instead of 500.
static bool uploadRejected = false;

// Whether a multipart file part arrived at all.
//
// Without it, a POST that reaches the result handler carrying no image is
// indistinguishable from one that wrote an image perfectly: the upload callback
// never fired, so nothing set uploadRejected and nothing set an error, and
// Update.hasError() is false when no update was ever attempted. The handler
// would then answer "Update OK" and reboot the board - unauthenticated, because
// the session check lives in the upload callback that never ran.
//
// This is defensive rather than a fix for an observed exploit: in practice a
// malformed POST to this route is usually swallowed earlier, by the library
// problem recorded under Known-unresolved in CLAUDE.md. Both need to hold.
static bool uploadStarted = false;

bool otaInProgress() { return updating; }

// ----------------------------------------------------------------------------
// Shared safety preamble. Called before any firmware write begins.
// ----------------------------------------------------------------------------
// Fans are left RUNNING rather than shut off: the element may have been
// energised moments ago, and for the next ~20 seconds nothing will be
// monitoring its temperature. Ventilating is the safe default.
static void otaPrepare(const char* via) {
  updating = true;

  forceSafeState(/*ventilate=*/true);
  saveSettings();          // statistics would otherwise be lost on reboot

  // The control loop stops feeding the watchdog during an update.
  esp_task_wdt_delete(NULL);

  Serial.printf("\n[OTA] Update starting via %s\n", via);
  Serial.println(F("[OTA] Heater OFF, fans RUNNING, watchdog released"));
  displayOtaBegin();
}

static void otaFinish(bool ok, const char* message) {
  updating = false;
  Serial.printf("[OTA] %s: %s\n", ok ? "SUCCESS" : "FAILED", message);
  displayOtaResult(ok, message);

  if (!ok) {
    // Re-arm the watchdog and hand control back; the running image is intact.
    esp_task_wdt_add(NULL);
  }
}

// ----------------------------------------------------------------------------
// Browser upload: GET /update serves the form, POST /update takes the image.
// ----------------------------------------------------------------------------
// No password field: the browser upload is guarded by the web UI session, the
// same one that guards the control endpoints. Reaching this page at all means
// already being logged in.
static const char UPDATE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Firmware Update</title><link rel='stylesheet' href='/style.css?v=700'>
</head><body><div class='container'>
<h1>Firmware Update</h1>
<div class='card'>
<p style='color:#FF6B6B;'><b>The heater is switched off and the fans run for the
whole update.</b> Do not remove power until the board reboots. If the upload
fails or is interrupted, the current firmware keeps running.</p>

<input type='file' id='file' accept='.bin' required
       style='width:100%;margin:10px 0;color:inherit;'>

<button class='btn' style='width:100%;margin-top:10px;' onclick='upload()'>
Upload firmware.bin</button>
<div id='msg' style='margin-top:15px;color:#aaa;'></div>
</div>

<button class='btn' style='width:100%;' onclick='location.href="/"'>Back to Control</button>
</div>
<script>
const $=id=>document.getElementById(id);

async function upload(){
  const f=$('file').files[0];
  if(!f){alert('Choose a firmware.bin first.');return;}
  $('msg').textContent='Uploading... the board reboots automatically when it finishes.';
  const fd=new FormData();
  fd.append('firmware',f,f.name);
  try{
    const r=await fetch('/update',{method:'POST',body:fd});
    if(r.status==401){
      $('msg').style.color='#FF6B6B';
      $('msg').textContent='Session expired - log in again.';
      setTimeout(()=>location.href='/login',1500);
      return;
    }
    $('msg').textContent=await r.text();
    if(!r.ok)$('msg').style.color='#FF6B6B';
  }catch(e){
    // A successful update reboots part-way through its own response, so a
    // dropped connection here is the expected ending rather than a failure.
    $('msg').textContent='Connection closed - if the board reboots, it worked.';
  }
}
</script></body></html>)HTML";

static void handleUpdatePage(WebServer& server) {
  if (!requireAuth(server)) return;
  server.send_P(200, PSTR("text/html"), UPDATE_HTML);
}

static void handleUpdateUpload(WebServer& server) {
  HTTPUpload& upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      // Checked before otaPrepare(), so a refused upload does not shed the
      // heater, release the watchdog or open the flash partition. An
      // unauthenticated request must cost the running system nothing.
      //
      // The session cookie is a HEADER, so it is fully parsed by the time this
      // runs - which is the whole reason the credential travels in one. A form
      // field would not arrive until after the image had already been written.
      // authCheck() rather than requireAuth() because nothing can be sent from
      // here; handleUpdateResult() answers once the body has been consumed.
      // Set before the auth check: a REFUSED upload is still an upload, and the
      // result handler needs to tell it apart from a request that carried no
      // image at all.
      uploadStarted = true;

      uploadRejected = !authCheck(server);
      if (uploadRejected) {
        Serial.println(F("[OTA] Web upload REFUSED - no valid session"));
        break;
      }
      otaPrepare("web upload");
      Serial.printf("[OTA] Receiving %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        otaFinish(false, "no space for image");
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (uploadRejected) break;
      if (Update.isRunning() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
      displayOtaProgress(upload.totalSize, 0);   // total unknown for uploads
      break;

    case UPLOAD_FILE_END:
      if (uploadRejected) break;
      if (Update.end(true)) {
        otaFinish(true, "rebooting");
      } else {
        Update.printError(Serial);
        otaFinish(false, "image rejected");
      }
      break;

    case UPLOAD_FILE_ABORTED:
      if (uploadRejected) break;
      Update.abort();
      otaFinish(false, "upload aborted");
      break;

    default:
      break;
  }
}

static void handleUpdateResult(WebServer& server) {
  const bool started = uploadStarted;
  const bool rejected = uploadRejected;
  uploadStarted = false;
  uploadRejected = false;

  // No file part ever arrived, so this was not a firmware upload. It must not
  // reach the ESP.restart() below: Update.hasError() is false when nothing was
  // ever attempted, so "no image" would otherwise be indistinguishable from
  // "wrote it perfectly" and reboot the board on request, unauthenticated.
  if (!started) {
    if (!requireAuth(server)) return;
    server.send(400, F("text/plain"), F("No firmware image in the request"));
    return;
  }

  if (rejected) {
    server.sendHeader(F("Connection"), F("close"));
    server.send(401, F("text/plain"), F("Not logged in - nothing was written"));
    return;
  }

  const bool ok = !Update.hasError();
  server.sendHeader(F("Connection"), F("close"));
  server.send(ok ? 200 : 500, F("text/plain"),
              ok ? F("Update OK - rebooting") : F("Update FAILED - firmware unchanged"));
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

// ----------------------------------------------------------------------------
void otaBegin(WebServer& server) {
  // net.cpp owns the mDNS responder. ArduinoOTA::begin() would otherwise start
  // its own with a hardcoded name, and the two would race over one responder -
  // so the hostname set on the WiFi page would sometimes simply not take.
  // net.cpp calls MDNS.enableArduino() itself, which is what keeps espota
  // discovery working; this board has no USB port, so that is not decorative.
  ArduinoOTA.setMdnsEnabled(false);

  const char* host = netHostname();
  ArduinoOTA.setHostname(host[0] ? host : "greenhouse");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { otaPrepare("ArduinoOTA"); });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    displayOtaProgress(done, total);
  });

  ArduinoOTA.onEnd([]() { otaFinish(true, "rebooting"); });

  ArduinoOTA.onError([](ota_error_t error) {
    const char* msg = "unknown error";
    switch (error) {
      case OTA_AUTH_ERROR:    msg = "auth failed";    break;
      case OTA_BEGIN_ERROR:   msg = "begin failed";   break;
      case OTA_CONNECT_ERROR: msg = "connect failed"; break;
      case OTA_RECEIVE_ERROR: msg = "receive failed"; break;
      case OTA_END_ERROR:     msg = "end failed";     break;
    }
    otaFinish(false, msg);
  });

  ArduinoOTA.begin();

  // The Cookie header the upload path needs is collected by authBegin(), which
  // webBegin() calls first. collectHeaders() REPLACES the list rather than
  // adding to it, so there must be exactly one such call in the firmware and
  // this is deliberately not it.
  server.on("/update", HTTP_GET,  [&server]() { handleUpdatePage(server); });
  server.on("/update", HTTP_POST, [&server]() { handleUpdateResult(server); },
                                  [&server]() { handleUpdateUpload(server); });

  Serial.println(F("OTA ready (ArduinoOTA + POST /update, session-guarded)"));
}

void otaLoop() {
  ArduinoOTA.handle();
}
