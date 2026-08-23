#include "webui.h"
#include <ArduinoJson.h>
#include "config.h"
#include "control.h"
#include "net.h"
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
h2{color:#4CAF50;border-bottom:2px solid #4CAF50;padding-bottom:5px;margin-top:20px;
   display:flex;justify-content:space-between;align-items:center;gap:10px;}
.version{text-align:center;color:#888;font-size:12px;margin-bottom:20px;}
.card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,.3);}
.temp{font-size:48px;font-weight:bold;color:#FFA500;text-align:center;margin:10px 0;}
.cap{text-align:center;color:#aaa;font-size:14px;margin:0;}
.status{display:inline-block;padding:10px 20px;border-radius:20px;margin:5px;font-weight:bold;
  font-size:14px;border:2px solid transparent;}
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
.note{color:#888;font-size:12px;margin:10px 0 0 0;}

/* iOS-style toggle. Pure CSS - no images, no animation library. */
.switch{position:relative;display:inline-block;width:51px;height:31px;flex:0 0 auto;}
.switch input{opacity:0;width:0;height:0;}
.slider{position:absolute;top:0;left:0;right:0;bottom:0;background:#555;border-radius:31px;transition:.3s;cursor:pointer;}
.slider:before{content:"";position:absolute;height:27px;width:27px;left:2px;top:2px;background:#fff;
  border-radius:50%;transition:.3s;box-shadow:0 1px 3px rgba(0,0,0,.4);}
.switch input:checked+.slider{background:#34C759;}
.switch input:checked+.slider:before{transform:translateX(20px);}
.mode-label{font-size:14px;color:#aaa;font-weight:normal;display:flex;align-items:center;gap:8px;}

/* In manual mode the EXISTING readouts become the controls. A blue outline
   marks what has become tappable; nothing new appears on the page. */
.tap{cursor:pointer;border-color:#0A84FF;}
.simtap{cursor:pointer;border-bottom:2px dashed #0A84FF;}
.simon{color:#FF9500;}

label.field{color:#aaa;font-size:13px;display:block;margin-top:10px;}
input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;
  border-radius:5px;border:1px solid #555;background:#1f1f1f;color:#fff;font-size:16px;}

/* Manual mode repaints the page light grey. The colour IS the mode indicator. */
body.manual{background:#d9d9d9;color:#111;}
body.manual h1,body.manual h2{color:#2E7D32;}
body.manual .card{background:#f4f4f4;box-shadow:0 2px 4px rgba(0,0,0,.15);}
body.manual .control,body.manual .sensor,body.manual .stat{background:#e6e6e6;}
body.manual .info-value{color:#111;}
body.manual .info-label,body.manual .label,body.manual .mode-label,body.manual .cap{color:#555;}
body.manual .sensor-label{color:#666;}
body.manual .sensor-value{color:#1565C0;}
body.manual .version,body.manual .foot,body.manual .note{color:#777;}
body.manual .status.off{background:#bbb;color:#444;}
body.manual .slider{background:#c0c0c0;}
body.manual input[type=text],body.manual input[type=password]{background:#fff;color:#111;border-color:#aaa;}
)CSS";

// ============================================================================
// WEB SERVER - ROOT PAGE
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Greenhouse Control</title><link rel='stylesheet' href='/style.css?v=611'>
</head><body><div class='container'>
<h1>Greenhouse Controller</h1>
<div class='version'>v<span id='fw'>-</span> | Safety-Critical System</div>

<div class='card'><h2><span>System Status</span>
<span class='mode-label'>Manual
<label class='switch'><input type='checkbox' id='manualSw' onchange='setManual(this.checked)'>
<span class='slider'></span></label></span></h2>
<div class='status off' id='heaterState' onclick='tapOut("heater")'>Heater: --</div>
<div class='status off' id='fansState' onclick='tapOut("fan")'>Fans: --</div>
<div class='status off' id='modeState'>Mode: --</div>
<div class='info-row'><span class='info-label'>Total Heating Cycles:</span><span class='info-value' id='cycles'>-</span></div>
<div class='info-row'><span class='info-label'>Total Runtime:</span><span class='info-value' id='runtime'>-</span></div>
</div>

<div class='card'><h2>Temperatures</h2>
<div class='temp' id='avgTemp' onclick='tapSim("air")'>--</div>
<p class='cap'>Average Air Temperature</p>
<p class='cap' style='margin-top:15px;'>Heater Zone:
<span id='heatTemp' style='color:#FF6B6B;font-weight:bold;font-size:20px;'
onclick='tapSim("heater")'>--</span></p>
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
<p class='note'>Max continuous runtime: 30 min | Min off-time: 5 min | Max cycles/hour: 6</p>
</div>

<div class='card'><h2>Quick Links</h2><div style='text-align:center;'>
<button class='btn' onclick='location.href="/stats"'>Statistics</button>
<button class='btn' onclick='location.href="/wifi"'>WiFi</button>
<button class='btn' onclick='location.href="/update"'>Firmware</button>
</div></div>

<div class='foot'>Greenhouse Controller v<span id='fw2'>-</span><br>
ESP32-WROOM-32E | 2200W Heater | DS18B20 Sensors | 220V AC Fan</div>
</div>
<script src='/ui.js?v=611'></script></body></html>)HTML";

static const char UI_JS[] PROGMEM = R"JS(
const $=id=>document.getElementById(id);
const C=v=>(v>-100)?v.toFixed(1)+'C':'ERROR';
function hms(s){return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}
function cells(arr,tag){return arr.map(function(v,i){
  return "<div class='sensor'><div class='sensor-label'>"+tag+" "+i+"</div>"+
    "<div class='sensor-value"+(v>-100?"":" err")+"'>"+
    (v>-100?v.toFixed(1)+'C':'ERROR')+"</div></div>";}).join('');}

let S={};                 // last /status
let settleUntil=0;        // stop the 2 s poll undoing something just tapped
function touched(){settleUntil=Date.now()+1500;}
function manual(){return S.manual==1;}

function badge(el,on,label,tappable){
  el.textContent=label+': '+(on?'ON':'OFF');
  el.className='status '+(on?'on':'off')+(tappable?' tap':'');
}

async function update(){
  try{
    S=await(await fetch('/status')).json();
    const man=manual();
    document.body.className=man?'manual':'';
    if(Date.now()>=settleUntil)$('manualSw').checked=man;

    // In manual the heater and fan badges ARE the switches. The fan stops
    // being tappable while the heater is on, because that is the interlock.
    badge($('heaterState'),S.heater==1,'Heater',man&&!S.fault);
    badge($('fansState'),S.fans==1,'Fans',man&&S.man_heat!=1);

    let m='Mode: '+S.mode+(S.cooldown==1?' [CD]':'');
    if(S.fault)m='Mode: '+S.fault_name;
    else if(man&&S.hold)m='Mode: MANUAL - '+S.hold;
    $('modeState').textContent=m;
    $('modeState').className='status '+(S.fault?'warn':'off');

    // Temperatures double as the simulated-input controls in manual mode.
    sim($('avgTemp'),S.avg,S.sim_air==1,S.sim_air_v,man);
    sim($('heatTemp'),S.heat_temp,S.sim_heat==1,S.sim_heat_v,man);

    $('cycles').textContent=S.cycles;
    $('runtime').textContent=hms(S.runtime);
    $('sensors').innerHTML=cells(S.left,'LEFT')+cells(S.right,'RIGHT');
    ['temp_min','temp_max','heater_max','heater_crit'].forEach(function(k){
      $(k).textContent=S[k].toFixed(1)+'C';});
    $('fw').textContent=S.fw; $('fw2').textContent=S.fw;
  }catch(e){console.error(e);}
}

function sim(el,real,armed,value,man){
  el.textContent=armed?(value.toFixed(1)+'C SIM'):C(real);
  el.classList.toggle('simon',armed);
  el.classList.toggle('simtap',man);
}

async function adj(p,a){await fetch('/adjust?param='+p+'&action='+a);setTimeout(update,200);}

async function setManual(on){
  touched();
  await fetch('/manual?on='+(on?1:0),{method:'POST'});
  setTimeout(update,200);
}

async function tapOut(dev){
  if(!manual())return;
  if(dev=='heater'&&S.fault){alert(S.fault_name+' - the heater cannot be switched on.');return;}
  if(dev=='fan'&&S.man_heat==1){alert('The fans cannot stop while the heater is on.');return;}
  const on=(dev=='heater')?S.man_heat!=1:S.man_fan!=1;
  if(dev=='heater'&&on&&!confirm('Energise the 2200 W element?'))return;
  touched();
  const r=await fetch('/output?dev='+dev+'&on='+(on?1:0),{method:'POST'});
  if(!r.ok)alert('Refused: not in manual mode.');
  setTimeout(update,200);
}

async function tapSim(target){
  if(!manual())return;
  const armed=(target=='air')?S.sim_air==1:S.sim_heat==1;
  const cur=(target=='air')?S.sim_air_v:S.sim_heat_v;
  const v=prompt((target=='air'?'Air':'Heater zone')+
    ' temperature to simulate.\nLeave blank to use the real sensor.',armed?cur:'');
  if(v===null)return;
  const on=v.trim()!=='';
  touched();
  const r=await fetch('/sim?target='+target+'&on='+(on?1:0)+'&value='+(on?v.trim():0),
    {method:'POST'});
  if(!r.ok)alert('Value must be between -40 and 100 C.');
  setTimeout(update,200);
}

setInterval(update,2000); window.onload=update;
)JS";

static void handleRoot() {
  server.send_P(200, PSTR("text/html"), INDEX_HTML);
}

// The 24-hour cache these two used to carry meant that after an OTA the phone
// kept serving the previous CSS and JS for a day, so a perfectly successful
// update looked like it had changed nothing. no-cache still lets the browser
// keep a copy, it just has to ask first.
static void handleStyleCSS() {
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send_P(200, PSTR("text/css"), PAGE_CSS);
}

static void handleUiJS() {
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send_P(200, PSTR("application/javascript"), UI_JS);
}

// ============================================================================
// WEB SERVER - WIFI SETTINGS PAGE
// ============================================================================
static const char WIFI_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>WiFi Settings</title><link rel='stylesheet' href='/style.css?v=611'>
</head><body><div class='container'>
<h1>WiFi Settings</h1>

<div class='card hidden' id='trialCard'>
<h2><span>Testing new settings</span><span id='left'>60</span>s</h2>
<p class='note'>Confirm now, or the previous settings come back.</p>
<button class='btn' style='width:100%;font-size:18px;padding:14px;' onclick='confirmCfg()'>
Keep these settings</button>
</div>

<div class='card'><h2>Current</h2>
<div class='info-row'><span class='info-label'>Access point:</span><span class='info-value' id='c_ap'>-</span></div>
<div class='info-row'><span class='info-label'>Address:</span><span class='info-value' id='c_ip'>-</span></div>
<div class='info-row'><span class='info-label'>Home network:</span><span class='info-value' id='c_sta'>-</span></div>
</div>

<div class='card'><h2>Access Point</h2>
<label class='field'>Network name (SSID)</label>
<input type='text' id='ap_ssid' maxlength='32' autocapitalize='off' autocorrect='off'>
<label class='field'>Password &mdash; blank leaves it unchanged</label>
<input type='password' id='ap_pass' maxlength='63' placeholder='unchanged'>
</div>

<div class='card'><h2><span>Join Home Network</span>
<label class='switch'><input type='checkbox' id='sta_en'><span class='slider'></span></label></h2>
<p class='note'>Additional, not instead &mdash; the access point stays up either way.</p>
<label class='field'>Network name (SSID)</label>
<input type='text' id='sta_ssid' maxlength='32' autocapitalize='off' autocorrect='off'>
<label class='field'>Password &mdash; blank to leave unchanged</label>
<input type='password' id='sta_pass' maxlength='63' placeholder='unchanged'>
</div>

<div class='card'>
<p class='note'>Saving disconnects you. Reconnect and confirm within 60 s, or the
previous settings come back by themselves. Nothing is written until you confirm.</p>
<button class='btn' style='width:100%;' onclick='save()'>Save and test</button>
<button class='btn danger' style='width:100%;margin-top:10px;' onclick='factory()'>
Factory reset WiFi</button>
</div>

<button class='btn' style='width:100%;' onclick='location.href="/"'>Back to Control</button>
</div>
<script>
const $=id=>document.getElementById(id);
let settleUntil=0;
function busy(){settleUntil=Date.now()+1500;}
function idle(el){return Date.now()>=settleUntil&&el!==document.activeElement;}

async function update(){
  try{
    const d=await(await fetch('/wifi/status')).json();
    $('c_ap').textContent=d.ap_ssid;
    $('c_ip').textContent=d.ap_ip;
    $('c_sta').textContent=d.sta_en?(d.sta_conn?(d.sta_ssid+' - '+d.sta_ip):(d.sta_ssid+' - connecting')):'off';
    if(idle($('ap_ssid')))$('ap_ssid').value=d.ap_ssid;
    if(idle($('sta_ssid')))$('sta_ssid').value=d.sta_ssid;
    if(idle($('sta_en')))$('sta_en').checked=d.sta_en;
    $('trialCard').classList.toggle('hidden',d.trial<=0);
    $('left').textContent=d.trial;
  }catch(e){console.error(e);}
}

async function save(){
  const p=$('ap_pass').value;
  if(p.length>0&&p.length<8){alert('The access point password must be at least 8 characters.');return;}
  const sp=$('sta_pass').value;
  if($('sta_en').checked&&sp.length>0&&sp.length<8){alert('The home network password must be at least 8 characters.');return;}
  if(!confirm('You will be disconnected. Reconnect and confirm within 60 s, '+
    'or the current settings come back.'))return;
  busy();
  const q='/wifi/apply?ap_ssid='+encodeURIComponent($('ap_ssid').value)+
    '&ap_pass='+encodeURIComponent(p)+
    '&sta_en='+($('sta_en').checked?1:0)+
    '&sta_ssid='+encodeURIComponent($('sta_ssid').value)+
    '&sta_pass='+encodeURIComponent(sp);
  const r=await fetch(q,{method:'POST'});
  if(!r.ok){alert('Refused: check the network name and password.');return;}
  $('ap_pass').value='';$('sta_pass').value='';
  alert('Applying now. Reconnect to the network and reopen this page to confirm.');
}

async function confirmCfg(){
  const r=await fetch('/wifi/confirm',{method:'POST'});
  alert(r.ok?'Saved.':'Too late - the previous settings have already been restored.');
  update();
}

async function factory(){
  if(!confirm('Reset WiFi to the factory settings?'))return;
  busy();
  await fetch('/wifi/reset',{method:'POST'});
  alert('Applying the factory settings. Reconnect and confirm within 60 seconds.');
}

setInterval(update,2000); window.onload=update;
</script></body></html>)HTML";

static void handleWifiPage() {
  server.send_P(200, PSTR("text/html"), WIFI_HTML);
}

// ============================================================================
// WEB SERVER - STATISTICS PAGE
// ============================================================================
static const char STATS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Statistics</title><link rel='stylesheet' href='/style.css?v=611'>
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
    document.body.className=(d.manual==1)?'manual':'';
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
  // Sized for the full payload with every sensor present and every optional
  // field populated. A silently truncated /status would take the whole UI down
  // with it, so there is deliberate headroom here.
  StaticJsonDocument<1024> doc;

  doc["fw"] = FIRMWARE_VERSION;
  doc["up"] = millis() / 1000;

  doc["avg"] = sensorData.averageTemp;
  doc["heat_temp"] = sensorData.heaterTemp;
  doc["heater"] = heaterOn ? 1 : 0;
  doc["fans"] = fansOn ? 1 : 0;
  doc["cooldown"] = (currentMode == MODE_COOLDOWN) ? 1 : 0;
  doc["mode"] = modeName(currentMode);
  doc["fault"] = (int)activeFault;
  doc["fault_name"] = faultName(activeFault);

  doc["manual"] = settings.manualMode ? 1 : 0;
  doc["man_heat"] = manualHeaterReq ? 1 : 0;
  doc["man_fan"] = manualFanReq ? 1 : 0;
  doc["hold"] = manualHoldReason();      // const char*: stored by pointer

  doc["sim_air"] = sim.airActive ? 1 : 0;
  doc["sim_air_v"] = sim.air;
  doc["sim_heat"] = sim.heaterActive ? 1 : 0;
  doc["sim_heat_v"] = sim.heater;
  doc["sim_left"] = simSecondsLeft();

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

  // A document that ran out of pool serialises as a truncated fragment, the
  // client's JSON.parse throws, and every value on the page stops updating -
  // silently, and with no clue pointing at this function. Say so on serial.
  if (doc.overflowed()) {
    Serial.println(F("WARNING: /status JSON overflowed - increase the document size"));
  }

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
// MANUAL MODE AND SIMULATION
// ============================================================================
// These handlers set a REQUEST and nothing more. Whether the relay actually
// moves is control.cpp's decision, made against the same interlocks as every
// automatic path. There is no digitalWrite on a relay pin in this file, and
// there must never be one.
static bool argIsOn(const String& name) { return server.arg(name) == "1"; }

static void handleManual() {
  if (!server.hasArg(F("on"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing on\"}"));
    return;
  }
  setManualMode(argIsOn(F("on")), millis());
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleOutput() {
  // A stale tab on someone else's phone must not be able to drive a relay
  // after the controller has been put back into automatic.
  if (!settings.manualMode) {
    server.send(409, F("application/json"), F("{\"error\":\"not in manual mode\"}"));
    return;
  }
  if (!server.hasArg(F("dev")) || !server.hasArg(F("on"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing dev/on\"}"));
    return;
  }

  const String dev = server.arg(F("dev"));
  const bool on = argIsOn(F("on"));
  const unsigned long now = millis();

  if (dev == F("heater"))   setManualHeater(on, now);
  else if (dev == F("fan")) setManualFan(on, now);
  else {
    server.send(404, F("application/json"), F("{\"error\":\"unknown device\"}"));
    return;
  }

  Serial.printf("MANUAL request: %s -> %s\n", dev.c_str(), on ? "ON" : "OFF");
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleSim() {
  if (!server.hasArg(F("target")) || !server.hasArg(F("on"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing target/on\"}"));
    return;
  }

  const String target = server.arg(F("target"));
  const bool on = argIsOn(F("on"));
  const float value = server.arg(F("value")).toFloat();

  bool ok;
  if (target == F("air"))         ok = setSimAir(on, value);
  else if (target == F("heater")) ok = setSimHeater(on, value);
  else {
    server.send(404, F("application/json"), F("{\"error\":\"unknown target\"}"));
    return;
  }

  if (!ok) {
    server.send(400, F("application/json"), F("{\"error\":\"value out of range\"}"));
    return;
  }

  Serial.printf("SIM %s: %s %.1fC\n", target.c_str(), on ? "armed" : "cleared", value);
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// ============================================================================
// WIFI CONFIGURATION
// ============================================================================
static void handleWifiStatus() {
  StaticJsonDocument<384> doc;
  const WifiConfig& cfg = netLiveConfig();

  doc["ap_ssid"]  = cfg.apSsid;
  doc["ap_ip"]    = netApAddress();
  doc["sta_en"]   = cfg.staEnabled ? 1 : 0;
  doc["sta_ssid"] = cfg.staSsid;
  doc["sta_conn"] = netStaConnected() ? 1 : 0;
  doc["sta_ip"]   = netStaAddress();
  doc["trial"]    = netTrialSecondsLeft();

  String output;
  output.reserve(measureJson(doc) + 1);
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}

static void copyArg(char* dst, size_t cap, const String& value) {
  strlcpy(dst, value.c_str(), cap);
}

static void handleWifiApply() {
  // Start from what is running now, so an omitted field means "leave it
  // alone". Passwords are never sent to the browser, so a blank password box
  // has to mean unchanged rather than empty - otherwise merely opening this
  // page and pressing Save would drop the network to an open one.
  WifiConfig cfg = netLiveConfig();

  if (server.hasArg(F("ap_ssid")))  copyArg(cfg.apSsid, WIFI_SSID_LEN, server.arg(F("ap_ssid")));
  if (server.hasArg(F("sta_ssid"))) copyArg(cfg.staSsid, WIFI_SSID_LEN, server.arg(F("sta_ssid")));
  if (server.hasArg(F("sta_en")))   cfg.staEnabled = argIsOn(F("sta_en"));

  const String apPass = server.arg(F("ap_pass"));
  if (apPass.length() > 0) copyArg(cfg.apPass, WIFI_PASS_LEN, apPass);

  const String staPass = server.arg(F("sta_pass"));
  if (staPass.length() > 0) copyArg(cfg.staPass, WIFI_PASS_LEN, staPass);

  if (!netRequestTrial(cfg)) {
    server.send(400, F("application/json"), F("{\"error\":\"invalid configuration\"}"));
    return;
  }

  Serial.println(F("[WiFi] New configuration queued on trial"));
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleWifiConfirm() {
  if (!netConfirm()) {
    server.send(409, F("application/json"), F("{\"error\":\"no trial running\"}"));
    return;
  }
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleWifiReset() {
  if (!netRequestFactoryReset()) {
    server.send(500, F("application/json"), F("{\"error\":\"reset failed\"}"));
    return;
  }
  Serial.println(F("[WiFi] Factory settings queued on trial"));
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

  server.on("/manual", HTTP_POST, handleManual);
  server.on("/output", HTTP_POST, handleOutput);
  server.on("/sim", HTTP_POST, handleSim);

  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/apply", HTTP_POST, handleWifiApply);
  server.on("/wifi/confirm", HTTP_POST, handleWifiConfirm);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);

  server.begin();
  Serial.println(F("Web server started"));
}

void webLoop() {
  server.handleClient();
}
