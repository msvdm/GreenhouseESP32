#include "webui.h"
#include <ArduinoJson.h>
#include "config.h"
#include "control.h"
#include "sensors.h"

static WebServer server(80);

WebServer& webServer() { return server; }

// ============================================================================
// WEB SERVER - SHARED PAGE CHROME
// ============================================================================
// Pages are static shells served straight from flash with send_P (no heap, no
// String building). Every dynamic value is fetched from /status by the client,
// which the UI already polled for anyway.
static const char PAGE_CSS[] PROGMEM = R"CSS(
body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#1a1a1a;color:#fff;}
.container{max-width:800px;margin:0 auto;}
h1{color:#4CAF50;text-align:center;margin-bottom:5px;}
h2{color:#4CAF50;border-bottom:2px solid #4CAF50;padding-bottom:5px;margin-top:20px;}
.version{text-align:center;color:#888;font-size:12px;margin-bottom:20px;}
.card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.3);}
.temp{font-size:48px;font-weight:bold;color:#FFA500;text-align:center;margin:10px 0;}
.status{display:inline-block;padding:10px 20px;border-radius:20px;margin:5px;font-weight:bold;font-size:14px;}
.status.on{background:#4CAF50;color:#000;}
.status.off{background:#666;color:#ccc;}
.status.warn{background:#f44336;color:#fff;animation:blink 1s infinite;}
@keyframes blink{50%{opacity:.5;}}
.control{display:flex;justify-content:space-between;align-items:center;margin:10px 0;padding:10px;background:#333;border-radius:5px;}
.btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;font-size:16px;font-weight:bold;border-radius:5px;cursor:pointer;margin:0 2px;}
.btn:hover{background:#45a049;}
.btn.danger{background:#dc3545;color:#fff;}
.value{font-size:18px;font-weight:bold;color:#4CAF50;}
.sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(80px,1fr));gap:10px;text-align:center;}
.sensor{background:#333;padding:10px;border-radius:5px;}
.sensor-label{color:#aaa;font-size:12px;}
.sensor-value{color:#87CEEB;font-weight:bold;font-size:16px;margin-top:5px;}
.sensor-value.err{color:#f44336;}
.info-row,.stat{display:flex;justify-content:space-between;margin:5px 0;}
.stat{padding:10px;background:#333;border-radius:5px;margin:10px 0;}
.info-label,.label{color:#aaa;}
.info-value{color:#fff;font-weight:bold;}
.stat .value{color:#4CAF50;}
.foot{text-align:center;margin-top:20px;color:#666;font-size:12px;}
)CSS";

// ============================================================================
// WEB SERVER - ROOT PAGE
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Greenhouse Control</title><link rel='stylesheet' href='/style.css'>
</head><body><div class='container'>
<h1>Greenhouse Controller</h1>
<div class='version'>v<span id='fw'>-</span> | Safety-Critical System</div>

<div class='card'><h2>System Status</h2>
<div class='status off' id='heaterState'>Heater: --</div>
<div class='status off' id='fansState'>Fans: --</div>
<div class='status off' id='modeState'>Mode: --</div>
<div class='info-row'><span class='info-label'>Total Heating Cycles:</span><span class='info-value' id='cycles'>-</span></div>
<div class='info-row'><span class='info-label'>Total Runtime:</span><span class='info-value' id='runtime'>-</span></div>
</div>

<div class='card'><h2>Temperatures</h2>
<div class='temp' id='avgTemp'>--</div>
<p style='text-align:center;color:#aaa;font-size:14px;'>Average Air Temperature</p>
<p style='text-align:center;margin-top:15px;'>Heater Zone:
<span id='heatTemp' style='color:#FF6B6B;font-weight:bold;font-size:20px;'>--</span></p>
</div>

<div class='card'><h2>Sensor Array</h2><div class='sensor-grid' id='sensors'></div></div>

<div class='card'><h2>Temperature Control</h2>
<div class='control'><span>Heating Starts:</span><span class='value' id='temp_min'>--</span>
<div><button class='btn' onclick='adj("tempmin","down")'>-</button>
<button class='btn' onclick='adj("tempmin","up")'>+</button></div></div>
<div class='control'><span>Cooling Starts:</span><span class='value' id='temp_max'>--</span>
<div><button class='btn' onclick='adj("tempmax","down")'>-</button>
<button class='btn' onclick='adj("tempmax","up")'>+</button></div></div>
</div>

<div class='card'><h2>Safety Limits</h2>
<div class='control'><span>Heater Safety Limit:</span><span class='value' id='heater_max'>--</span>
<div><button class='btn' onclick='adj("heatmax","down")'>-</button>
<button class='btn' onclick='adj("heatmax","up")'>+</button></div></div>
<div class='info-row'><span class='info-label'>Critical Shutdown:</span><span class='info-value' id='heater_crit'>--</span></div>
<p style='color:#888;font-size:12px;margin:10px 0 0 0;'>Max continuous runtime: 30 min | Min off-time: 5 min | Max cycles/hour: 6</p>
</div>

<div class='card'><h2>Quick Links</h2><div style='text-align:center;'>
<button class='btn' onclick='location.href="/stats"'>Statistics</button>
<button class='btn' onclick='location.href="/update"'>Firmware</button>
</div></div>

<div class='foot'>Greenhouse Controller v<span id='fw2'>-</span><br>
ESP32-WROOM-32E | 2200W Heater | DS18B20 Sensors | 220V AC Fan</div>
</div>
<script src='/ui.js'></script></body></html>)HTML";

static const char UI_JS[] PROGMEM = R"JS(
const $=id=>document.getElementById(id);
const C=v=>(v>-100)?v.toFixed(1)+'C':'ERROR';
function hms(s){return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}
function badge(el,on,label){el.textContent=label+': '+(on?'ON':'OFF');el.className='status '+(on?'on':'off');}
function cells(arr,tag){return arr.map(function(v,i){
  return "<div class='sensor'><div class='sensor-label'>"+tag+" "+i+"</div>"+
    "<div class='sensor-value"+(v>-100?"":" err")+"'>"+
    (v>-100?v.toFixed(1)+'C':'ERROR')+"</div></div>";}).join('');}
async function update(){
  try{
    const d=await(await fetch('/status')).json();
    $('avgTemp').textContent=C(d.avg);
    $('heatTemp').textContent=C(d.heat_temp);
    badge($('heaterState'),d.heater==1,'Heater');
    badge($('fansState'),d.fans==1,'Fans');
    $('modeState').textContent='Mode: '+d.mode+(d.cooldown==1?' [CD]':'');
    $('cycles').textContent=d.cycles;
    $('runtime').textContent=hms(d.runtime);
    $('sensors').innerHTML=cells(d.left,'LEFT')+cells(d.right,'RIGHT');
    ['temp_min','temp_max','heater_max','heater_crit'].forEach(function(k){
      $(k).textContent=d[k].toFixed(1)+'C';});
    $('fw').textContent=d.fw; $('fw2').textContent=d.fw;
  }catch(e){console.error(e);}
}
async function adj(p,a){await fetch('/adjust?param='+p+'&action='+a);setTimeout(update,200);}
setInterval(update,2000); window.onload=update;
)JS";

static void handleRoot() {
  server.send_P(200, PSTR("text/html"), INDEX_HTML);
}

static void handleStyleCSS() {
  server.sendHeader(F("Cache-Control"), F("max-age=86400"));
  server.send_P(200, PSTR("text/css"), PAGE_CSS);
}

static void handleUiJS() {
  server.sendHeader(F("Cache-Control"), F("max-age=86400"));
  server.send_P(200, PSTR("application/javascript"), UI_JS);
}

// ============================================================================
// WEB SERVER - STATISTICS PAGE
// ============================================================================
static const char STATS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Statistics</title><link rel='stylesheet' href='/style.css'>
</head><body><div class='container'>
<h1>System Statistics</h1>
<div class='card'>
<div class='stat'><span class='label'>Total Heating Cycles:</span><span class='value' id='cycles'>-</span></div>
<div class='stat'><span class='label'>Total Cooling Cycles:</span><span class='value' id='cool_cycles'>-</span></div>
<div class='stat'><span class='label'>Total Runtime:</span><span class='value' id='runtime'>-</span></div>
<div class='stat'><span class='label'>Uptime:</span><span class='value' id='up'>-</span></div>
<div class='stat'><span class='label'>Min Temperature:</span><span class='value' id='t_min_rec'>-</span></div>
<div class='stat'><span class='label'>Max Temperature:</span><span class='value' id='t_max_rec'>-</span></div>
<div class='stat'><span class='label'>Safety Shutdowns:</span><span class='value' id='shutdowns'>-</span></div>
</div>
<div class='card'><h2 style='color:#FF6B6B;'>Safety Events Breakdown</h2>
<div class='stat'><span class='label'>Heater Sensor Failures:</span><span class='value' id='h_sens_fail'>-</span></div>
<div class='stat'><span class='label'>Heater Critical Events:</span><span class='value' id='h_critical'>-</span></div>
<div class='stat'><span class='label'>Heater Over-Temp Events:</span><span class='value' id='h_safety'>-</span></div>
<div class='stat'><span class='label'>Air Sensor Failures:</span><span class='value' id='a_sens_fail'>-</span></div>
<div class='stat'><span class='label'>Invalid Reading Events:</span><span class='value' id='inv_read'>-</span></div>
</div>
<button class='btn' style='width:100%;' onclick='location.href="/"'>Back to Control</button>
<button class='btn danger' style='width:100%;margin-top:10px;' onclick='reset()'>Reset Statistics</button>
</div>
<script>
const $=id=>document.getElementById(id);
function hms(s){return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}
async function update(){
  try{
    const d=await(await fetch('/status')).json();
    ['cycles','cool_cycles','shutdowns','h_sens_fail','h_critical',
     'h_safety','a_sens_fail','inv_read'].forEach(function(k){$(k).textContent=d[k];});
    $('runtime').textContent=hms(d.runtime);
    $('up').textContent=hms(d.up);
    $('t_min_rec').textContent=d.t_min_rec.toFixed(1)+'C';
    $('t_max_rec').textContent=d.t_max_rec.toFixed(1)+'C';
  }catch(e){console.error(e);}
}
function reset(){
  if(confirm('Reset all statistics? This cannot be undone.'))
    fetch('/resetstats',{method:'POST'}).then(update);
}
setInterval(update,2000); window.onload=update;
</script></body></html>)HTML";

static void handleStats() {
  server.send_P(200, PSTR("text/html"), STATS_HTML);
}

// ============================================================================
// STATUS JSON - the single source of truth for every dynamic value in the UI
// ============================================================================
static void handleStatusJSON() {
  StaticJsonDocument<768> doc;

  doc["fw"] = FIRMWARE_VERSION;
  doc["up"] = millis() / 1000;

  doc["avg"] = sensorData.averageTemp;
  doc["heat_temp"] = sensorData.heaterTemp;
  doc["heater"] = heaterOn ? 1 : 0;
  doc["fans"] = fansOn ? 1 : 0;
  doc["cooldown"] = (currentMode == MODE_COOLDOWN) ? 1 : 0;
  doc["mode"] = modeName(currentMode);
  doc["fault"] = (int)activeFault;

  doc["temp_min"] = settings.tempMin;
  doc["temp_max"] = settings.tempMax;
  doc["heater_max"] = settings.heaterMax;
  doc["heater_crit"] = settings.heaterCritical;

  JsonArray left = doc.createNestedArray("left");
  for (int i = 0; i < sensorData.numLeft; i++) left.add(sensorData.left[i]);

  JsonArray right = doc.createNestedArray("right");
  for (int i = 0; i < sensorData.numRight; i++) right.add(sensorData.right[i]);

  doc["cycles"] = stats.totalHeatingCycles;
  doc["cool_cycles"] = stats.totalCoolingCycles;
  doc["runtime"] = stats.totalHeaterRuntime / 1000;
  doc["t_min_rec"] = stats.minTempRecorded;
  doc["t_max_rec"] = stats.maxTempRecorded;
  doc["shutdowns"] = stats.safetyShutdownCount;
  doc["h_sens_fail"] = stats.heaterSensorFailures;
  doc["h_critical"] = stats.heaterCriticalEvents;
  doc["h_safety"] = stats.heaterSafetyEvents;
  doc["a_sens_fail"] = stats.airSensorFailures;
  doc["inv_read"] = stats.invalidReadingEvents;

  String output;
  output.reserve(measureJson(doc) + 1);
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}

// ============================================================================
// SETPOINT ADJUSTMENT
// ============================================================================
// Editable setpoints as data. Each carries its own absolute range; the
// relationships between them are enforced by clampSetpoints(), so no sequence
// of individually-legal edits can produce an illegal pair.
struct Setpoint {
  const char* name;
  float Settings::* field;
};

static const Setpoint SETPOINTS[] = {
  { "tempmin", &Settings::tempMin   },
  { "tempmax", &Settings::tempMax   },
  { "heatmax", &Settings::heaterMax },
};

static void handleAdjust() {
  if (!server.hasArg(F("param")) || !server.hasArg(F("action"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing param/action\"}"));
    return;
  }

  const String param = server.arg(F("param"));
  const float delta = (server.arg(F("action")) == F("up")) ? 0.5f : -0.5f;

  for (const Setpoint& sp : SETPOINTS) {
    if (param != sp.name) continue;

    settings.*(sp.field) += delta;
    clampSetpoints();
    settingsChangedTime = millis();   // debounced write, handled by loop()
    Serial.printf("%s -> %.1fC\n", sp.name, settings.*(sp.field));

    server.send(200, F("application/json"), F("{\"ok\":1}"));
    return;
  }

  server.send(404, F("application/json"), F("{\"error\":\"unknown param\"}"));
}

static void handleResetStats() {
  resetStats();
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// ============================================================================
void webBegin() {
  server.on("/", handleRoot);
  server.on("/style.css", handleStyleCSS);
  server.on("/ui.js", handleUiJS);
  server.on("/status", handleStatusJSON);
  server.on("/adjust", handleAdjust);
  server.on("/stats", handleStats);
  server.on("/resetstats", handleResetStats);
  server.begin();
  Serial.println(F("Web server started"));
}

void webLoop() {
  server.handleClient();
}
