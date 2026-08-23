#include "ota.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "control.h"
#include "display.h"
#include "secrets.h"

static bool updating = false;

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
static const char UPDATE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Firmware Update</title><link rel='stylesheet' href='/style.css'>
</head><body><div class='container'>
<h1>Firmware Update</h1>
<div class='card'>
<p style='color:#FF6B6B;'><b>The heater is switched off and the fans run for the
whole update.</b> Do not remove power until the board reboots. If the upload
fails or is interrupted, the current firmware keeps running.</p>
<form method='POST' action='/update' enctype='multipart/form-data' id='f'>
<input type='file' name='firmware' accept='.bin' required
       style='width:100%;margin:10px 0;color:#fff;'>
<button class='btn' type='submit' style='width:100%;'>Upload firmware.bin</button>
</form>
<div id='msg' style='margin-top:15px;color:#aaa;'></div>
</div>
<button class='btn' style='width:100%;' onclick='location.href="/"'>Back to Control</button>
</div>
<script>
document.getElementById('f').addEventListener('submit',function(){
  document.getElementById('msg').textContent=
    'Uploading... the board reboots automatically when it finishes.';
});
</script></body></html>)HTML";

static void handleUpdatePage(WebServer& server) {
  server.send_P(200, PSTR("text/html"), UPDATE_HTML);
}

static void handleUpdateUpload(WebServer& server) {
  HTTPUpload& upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      otaPrepare("web upload");
      Serial.printf("[OTA] Receiving %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
        otaFinish(false, "no space for image");
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (Update.isRunning() && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
      displayOtaProgress(upload.totalSize, 0);   // total unknown for uploads
      break;

    case UPLOAD_FILE_END:
      if (Update.end(true)) {
        otaFinish(true, "rebooting");
      } else {
        Update.printError(Serial);
        otaFinish(false, "image rejected");
      }
      break;

    case UPLOAD_FILE_ABORTED:
      Update.abort();
      otaFinish(false, "upload aborted");
      break;

    default:
      break;
  }
}

static void handleUpdateResult(WebServer& server) {
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
  ArduinoOTA.setHostname("greenhouse");
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

  server.on("/update", HTTP_GET,  [&server]() { handleUpdatePage(server); });
  server.on("/update", HTTP_POST, [&server]() { handleUpdateResult(server); },
                                  [&server]() { handleUpdateUpload(server); });

  Serial.println(F("OTA ready (ArduinoOTA + POST /update)"));
}

void otaLoop() {
  ArduinoOTA.handle();
}
