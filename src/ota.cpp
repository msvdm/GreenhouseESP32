#include "ota.h"
#include "net.h"
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "control.h"
#include "display.h"
#include "secrets.h"

static bool updating = false;

// Operator-set password for the BROWSER upload path. Empty means unguarded,
// which is the default and also what every existing board will come up with.
static char updatePass[UPDATE_PASS_LEN] = { 0 };

// Set when an upload is refused, so the write path stays shut for the rest of
// the request and the result handler can answer 401 instead of 500.
static bool uploadRejected = false;

bool otaInProgress() { return updating; }

// Checked on UPLOAD_FILE_START, the last moment before Update.begin() opens the
// flash partition. Headers are fully parsed by then, whereas a form field in
// the body would not arrive until after the image had already been written.
// That ordering is the entire reason this is carried in a header.
static bool updateAuthorised(WebServer& server) {
  if (updatePass[0] == 0) return true;                 // no password set
  return server.header(F("X-Update-Auth")) == updatePass;
}

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
<title>Firmware Update</title><link rel='stylesheet' href='/style.css?v=621'>
</head><body><div class='container'>
<h1>Firmware Update</h1>
<div class='card'>
<p style='color:#FF6B6B;'><b>The heater is switched off and the fans run for the
whole update.</b> Do not remove power until the board reboots. If the upload
fails or is interrupted, the current firmware keeps running.</p>

<input type='file' id='file' accept='.bin' required
       style='width:100%;margin:10px 0;color:inherit;'>

<label class='field'>Update password</label>
<input type='password' id='pass' maxlength='63' placeholder='none set'>
<p class='note' id='passNote'></p>

<button class='btn' style='width:100%;margin-top:10px;' onclick='upload()'>
Upload firmware.bin</button>
<div id='msg' style='margin-top:15px;color:#aaa;'></div>
</div>

<div class='card'><h2>Change update password</h2>
<label class='field'>New password &mdash; blank clears it</label>
<input type='password' id='newpass' maxlength='63' placeholder='no password'>
<p class='note'>If a password is already set, enter it above first. Minimum 8
characters. This is separate from the compiled-in password used by
<code>pio run -e ota</code>; changing it here cannot lock you out of that.</p>
<div class='actions'><button class='btn' onclick='savePass()'>Save password</button></div>
</div>

<button class='btn' style='width:100%;' onclick='location.href="/"'>Back to Control</button>
</div>
<script>
const $=id=>document.getElementById(id);
let isSet=false;

async function refresh(){
  try{
    const d=await(await fetch('/update/status')).json();
    isSet=d.set==1;
    $('pass').placeholder=isSet?'required':'none set';
    $('passNote').textContent=isSet
      ?'Required to upload, and to change the password.'
      :'No password is set. Anyone who can reach this board can replace the '+
       'firmware that drives the 2200 W contactor - setting one is a good idea.';
    $('passNote').style.color=isSet?'#888':'#FF6B6B';
  }catch(e){console.error(e);}
}

async function upload(){
  const f=$('file').files[0];
  if(!f){alert('Choose a firmware.bin first.');return;}
  if(isSet&&!$('pass').value){alert('This board needs the update password.');return;}
  $('msg').textContent='Uploading... the board reboots automatically when it finishes.';
  const fd=new FormData();
  fd.append('firmware',f,f.name);
  try{
    const r=await fetch('/update',{method:'POST',
      headers:{'X-Update-Auth':$('pass').value},body:fd});
    $('msg').textContent=await r.text();
    if(!r.ok)$('msg').style.color='#FF6B6B';
  }catch(e){
    // A successful update reboots part-way through its own response, so a
    // dropped connection here is the expected ending rather than a failure.
    $('msg').textContent='Connection closed - if the board reboots, it worked.';
  }
}

async function savePass(){
  const np=$('newpass').value;
  if(np.length>0&&np.length<8){
    alert('Use at least 8 characters, or leave it blank to clear.');return;}
  if(!np&&!confirm('Clear the update password? Anyone who can reach this board '+
    'will then be able to replace its firmware.'))return;
  const r=await fetch('/update/pass?new='+encodeURIComponent(np),
    {method:'POST',headers:{'X-Update-Auth':$('pass').value}});
  if(r.status==401){alert('Wrong current password.');return;}
  if(!r.ok){alert('Could not save the password.');return;}
  $('newpass').value='';$('pass').value=np;
  alert(np?'Password saved.':'Password cleared.');
  refresh();
}

window.onload=refresh;
</script></body></html>)HTML";

static void handleUpdatePage(WebServer& server) {
  server.send_P(200, PSTR("text/html"), UPDATE_HTML);
}

static void handleUpdateUpload(WebServer& server) {
  HTTPUpload& upload = server.upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
      // Checked before otaPrepare(), so a refused upload does not shed the
      // heater, release the watchdog or open the flash partition. An
      // unauthenticated request must cost the running system nothing.
      uploadRejected = !updateAuthorised(server);
      if (uploadRejected) {
        Serial.println(F("[OTA] Web upload REFUSED - wrong or missing password"));
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
  if (uploadRejected) {
    uploadRejected = false;
    server.sendHeader(F("Connection"), F("close"));
    server.send(401, F("text/plain"), F("Wrong update password - nothing was written"));
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

static void handleUpdateStatus(WebServer& server) {
  // Only whether a password exists, never the password itself.
  server.send(200, F("application/json"),
              updatePass[0] ? F("{\"set\":1}") : F("{\"set\":0}"));
}

static void handleUpdatePassword(WebServer& server) {
  // Changing the password requires the current one, so someone who wanders
  // onto an already-protected board cannot simply set their own.
  if (!updateAuthorised(server)) {
    server.send(401, F("application/json"), F("{\"error\":\"wrong password\"}"));
    return;
  }

  const String next = server.arg(F("new"));

  // Either unguarded or long enough to be worth something. Eight characters
  // matches the floor the WiFi passwords already use on the settings page.
  if (next.length() > 0 && (next.length() < 8 || next.length() >= UPDATE_PASS_LEN)) {
    server.send(400, F("application/json"), F("{\"error\":\"8-63 characters, or blank\"}"));
    return;
  }

  strlcpy(updatePass, next.c_str(), UPDATE_PASS_LEN);
  saveUpdatePassword(updatePass);
  server.send(200, F("application/json"), F("{\"ok\":1}"));
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

  loadUpdatePassword(updatePass, UPDATE_PASS_LEN);

  // WebServer discards every header it was not told to keep, and the upload
  // handler needs this one before the body arrives.
  static const char* UPDATE_HEADERS[] = { "X-Update-Auth" };
  server.collectHeaders(UPDATE_HEADERS, 1);

  server.on("/update", HTTP_GET,  [&server]() { handleUpdatePage(server); });
  server.on("/update", HTTP_POST, [&server]() { handleUpdateResult(server); },
                                  [&server]() { handleUpdateUpload(server); });
  server.on("/update/status", HTTP_GET,  [&server]() { handleUpdateStatus(server); });
  server.on("/update/pass",   HTTP_POST, [&server]() { handleUpdatePassword(server); });

  Serial.printf("OTA ready (ArduinoOTA + POST /update, web password %s)\n",
                updatePass[0] ? "SET" : "not set");
}

void otaLoop() {
  ArduinoOTA.handle();
}
