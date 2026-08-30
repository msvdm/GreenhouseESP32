#include "webui.h"
#include <ArduinoJson.h>
#include <WiFi.h>          // scan results, read directly in handleWifiScan()
#include "auth.h"
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
h1{color:#4CAF50;text-align:center;margin-bottom:20px;}
h2{color:#4CAF50;border-bottom:2px solid #4CAF50;padding-bottom:5px;margin-top:20px;
   display:flex;justify-content:space-between;align-items:center;gap:10px;}
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
.info-row,.stat{display:flex;justify-content:space-between;margin:5px 0;gap:10px;}
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
input[type=text],input[type=password],select{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;
  border-radius:5px;border:1px solid #555;background:#1f1f1f;color:#fff;font-size:16px;}

/* Buttons that sit side by side and share the width, and the single action
   button that sits in the bottom-right corner of a settings card. */
.btnrow{display:flex;gap:10px;margin-top:10px;}
.btnrow .btn{flex:1;margin:0;}
.actions{display:flex;justify-content:flex-end;margin-top:12px;}

/* A card whose controls do nothing in the current mode. Dimmed rather than
   removed, so the page does not reflow when the mode switch is flipped. */
.card.off{opacity:.4;pointer-events:none;}

/* The WiFi trial prompt. It lives on the CONTROL page rather than the settings
   page because that is where an operator lands after reconnecting, and the
   clock is already running by then. */
.overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.85);
  display:flex;align-items:center;justify-content:center;padding:20px;z-index:99;}
.overlay .box{background:#2a2a2a;padding:20px;border-radius:8px;max-width:420px;width:100%;
  box-shadow:0 4px 20px rgba(0,0,0,.6);}
.overlay h2{margin-top:0;}

/* The default-password nag. Deliberately loud and deliberately not dismissable:
   it is the one thing standing between a stranger on the network and 2200 W. */
.banner{background:#5a1a1a;border:2px solid #f44336;color:#ffd7d7;padding:12px;
  border-radius:8px;margin:10px 0;font-size:14px;}
.banner b{color:#fff;}
.banner .btn{margin-top:10px;width:100%;}

/* Login page: one field, centred, nothing else to get in the way. */
.login{max-width:340px;margin:60px auto;}

/* Last, and !important, so it beats the display value of anything it is put
   on. This rule was missing entirely, which is why the trial card on the WiFi
   page was permanently visible however the script tried to hide it. */
.hidden{display:none !important;}

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
body.manual input[type=text],body.manual input[type=password],body.manual select{
  background:#fff;color:#111;border-color:#aaa;}
body.manual .overlay .box{background:#f4f4f4;}
)CSS";

// ============================================================================
// WEB SERVER - LOGIN PAGE
// ============================================================================
// One password, no username. A username on a single-operator appliance is a
// second thing to forget for no security at all - the same reasoning a consumer
// router applies when it asks only for a password.
static const char LOGIN_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Greenhouse Login</title><link rel='stylesheet' href='/style.css?v=700'>
</head><body><div class='container'><div class='login'>
<h1>Greenhouse</h1>
<div class='card'>
<label class='field'>Password</label>
<input type='password' id='pass' autofocus onkeydown='if(event.key=="Enter")go()'>
<button class='btn' style='width:100%;margin-top:12px;' onclick='go()'>Log in</button>
<p class='note' id='msg'></p>
</div>
<p class='note' style='text-align:center;'>Readings stay visible without logging
in, and a manually-energised heater can always be switched OFF. A login is
needed to switch anything ON or to change a setting.</p>
</div></div>
<script>
const $=id=>document.getElementById(id);
async function go(){
  const r=await fetch('/login',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'pass='+encodeURIComponent($('pass').value)});
  if(r.ok){location.href='/';return;}
  const d=await r.json().catch(()=>({}));
  $('msg').style.color='#FF6B6B';
  $('msg').textContent=(r.status==429)
    ?'Too many attempts - wait a minute and try again.'
    :(d.error||'Wrong password.');
  $('pass').value='';$('pass').focus();
}
</script></body></html>)HTML";

// ============================================================================
// WEB SERVER - ROOT PAGE
// ============================================================================
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Greenhouse Control</title><link rel='stylesheet' href='/style.css?v=700'>
</head><body><div class='container'>
<h1>Greenhouse Controller</h1>

<div class='banner hidden' id='pwNag'>
<b>The web password is still the default.</b> Anyone who can reach this board
can switch on the 2200 W element. Change it, or set the access point password
so that fewer people can reach it.
<button class='btn' onclick='location.href="/settings"'>Change password</button>
</div>

<div class='overlay hidden' id='netTrial'><div class='box'>
<h2>New WiFi settings</h2>
<p class='note'>These are on trial. Unless you keep them they revert by
themselves in <span id='trialLeft'>-</span>s, and the previous settings come back.</p>
<div class='btnrow'>
<button class='btn' onclick='netKeep()'>Keep these settings</button>
<button class='btn danger' onclick='netRevert()'>Revert now</button>
</div></div></div>

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

<div class='card' id='tempCard'><h2>Temperature Control</h2>
<div class='control'><span>Heating Starts:</span><span class='value' id='temp_min'>--</span>
<div><button class='btn' onclick='adj("tempmin","down")'>-</button>
<button class='btn' onclick='adj("tempmin","up")'>+</button></div></div>
<div class='control'><span>Cooling Starts:</span><span class='value' id='temp_max'>--</span>
<div><button class='btn' onclick='adj("tempmax","down")'>-</button>
<button class='btn' onclick='adj("tempmax","up")'>+</button></div></div>
<p class='note hidden' id='tempOff'>Not used in manual mode.</p>
</div>

<div class='card' id='limitCard'><h2>Safety Limits</h2>
<div class='control'><span>Heater Safety Limit:</span><span class='value' id='heater_max'>--</span>
<div><button class='btn' onclick='adj("heatmax","down")'>-</button>
<button class='btn' onclick='adj("heatmax","up")'>+</button></div></div>
<div class='info-row'><span class='info-label'>Critical Shutdown:</span><span class='info-value' id='heater_crit'>--</span></div>
<p class='note'>Max continuous runtime: 30 min | Min off-time: 5 min | Max cycles/hour: 6</p>
<p class='note hidden' id='limitOff'>Not used in manual mode - only the critical
shutdown still applies.</p>
</div>

<div class='card'><div style='text-align:center;'>
<button class='btn' onclick='location.href="/stats"'>Statistics</button>
<button class='btn' onclick='location.href="/settings"'>Settings</button>
<button class='btn' onclick='location.href="/update"'>Firmware</button>
<button class='btn' id='authBtn' onclick='authTap()'>Log in</button>
</div></div>

<div class='foot'>Greenhouse Controller v<span id='fw2'>-</span></div>
</div>
<script src='/ui.js?v=700'></script></body></html>)HTML";

static const char UI_JS[] PROGMEM = R"JS(
const $=id=>document.getElementById(id);
const C=v=>(v>-100)?v.toFixed(1)+'C':'ERROR';
function hms(s){return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m';}
function mmss(s){return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');}
function cells(arr,tag){return arr.map(function(v,i){
  return "<div class='sensor'><div class='sensor-label'>"+tag+" "+i+"</div>"+
    "<div class='sensor-value"+(v>-100?"":" err")+"'>"+
    (v>-100?v.toFixed(1)+'C':'ERROR')+"</div></div>";}).join('');}

let S={};                 // last /status
let settleUntil=0;        // stop the 2 s poll undoing something just tapped
function touched(){settleUntil=Date.now()+1500;}
function manual(){return S.manual==1;}

// Every state-changing request goes through here, so an expired session has
// exactly one handler instead of one per call site. Note that switching an
// output OFF is deliberately allowed without a session, so this can send
// everything the same way and let the board decide what needs a login.
async function call(url,method){
  const r=await fetch(url,{method:method||'POST'});
  if(r.status==401){location.href='/login';return null;}
  return r;
}

async function authTap(){
  if(S.auth==1){await fetch('/logout',{method:'POST'});update();}
  else location.href='/login';
}

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

    // In manual both badges ARE the switches, and they are independent of each
    // other: the fan no longer follows the heater, so nothing blocks either tap.
    badge($('heaterState'),S.heater==1,'Heater',man&&!S.crit);
    badge($('fansState'),S.fans==1,'Fans',man);

    // The blind countdown outranks everything except a latched fault. While it
    // is running the element has no supervision at all, so it gets the warning
    // style rather than a line of small print.
    let m='Mode: '+S.mode+(S.cooldown==1?' [CD]':'');
    let warn=false;
    if(S.crit){m='Mode: '+S.fault_name;warn=true;}
    else if(man&&S.blind>0){m='UNSUPERVISED - no probe, '+mmss(S.blind);warn=true;}
    else if(!man&&S.fault){m='Mode: '+S.fault_name;warn=true;}
    else if(man&&S.fault)m='MANUAL - '+S.fault_name+' (ignored)';
    else if(man&&S.hold)m='Mode: MANUAL - '+S.hold;
    $('modeState').textContent=m;
    $('modeState').className='status '+(warn?'warn':'off');

    // Neither of these does anything in manual mode, so they are dimmed out
    // rather than left looking live. The server refuses /adjust in manual too.
    $('tempCard').classList.toggle('off',man);
    $('limitCard').classList.toggle('off',man);
    $('tempOff').classList.toggle('hidden',!man);
    $('limitOff').classList.toggle('hidden',!man);

    // Temperatures double as the simulated-input controls in manual mode.
    sim($('avgTemp'),S.avg,S.sim_air==1,S.sim_air_v,man);
    sim($('heatTemp'),S.heat_temp,S.sim_heat==1,S.sim_heat_v,man);

    $('cycles').textContent=S.cycles;
    $('runtime').textContent=hms(S.runtime);
    $('sensors').innerHTML=cells(S.left,'LEFT')+cells(S.right,'RIGHT');
    ['temp_min','temp_max','heater_max','heater_crit'].forEach(function(k){
      $(k).textContent=S[k].toFixed(1)+'C';});
    $('fw2').textContent=S.fw;

    // A WiFi trial is running: this is the first page the operator sees after
    // reconnecting, so the decision is put in front of them here.
    $('netTrial').classList.toggle('hidden',!(S.trial>0));
    $('trialLeft').textContent=S.trial;

    // The nag is shown only to someone logged in. Announcing "this board still
    // has its default password" to every passer-by would be an invitation
    // rather than a warning, and they could not act on it in any case.
    $('pwNag').classList.toggle('hidden',!(S.auth==1&&S.pw_def==1));
    $('authBtn').textContent=(S.auth==1)?'Log out':'Log in';
  }catch(e){console.error(e);}
}

function sim(el,real,armed,value,man){
  el.textContent=armed?(value.toFixed(1)+'C SIM'):C(real);
  el.classList.toggle('simon',armed);
  el.classList.toggle('simtap',man);
}

async function adj(p,a){
  const r=await call('/adjust?param='+p+'&action='+a,'GET');
  if(r&&!r.ok&&r.status==409)alert('These are not used in manual mode.');
  setTimeout(update,200);
}

async function setManual(on){
  touched();
  await call('/manual?on='+(on?1:0));
  setTimeout(update,200);
}

async function tapOut(dev){
  if(!manual())return;
  if(dev=='heater'&&S.crit){alert(S.fault_name+' - the heater cannot be switched on.');return;}
  const on=(dev=='heater')?S.man_heat!=1:S.man_fan!=1;
  if(dev=='heater'&&on){
    // Manual has no zone protection left beyond the critical trip, and with no
    // probe it has not even got that. Say which of the two this is.
    const blind=!(S.heat_temp>-100);
    const msg=blind
      ?'There is NO heater zone reading. The 50C trip cannot fire, so nothing '+
       'will limit the 2200 W element except a 5 minute cutoff.\n\nEnergise it anyway?'
      :'Energise the 2200 W element?\n\nManual mode: only the critical shutdown applies.';
    if(!confirm(msg))return;
  }
  touched();
  const r=await call('/output?dev='+dev+'&on='+(on?1:0));
  if(r&&!r.ok)alert('Refused: not in manual mode.');
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
  const r=await call('/sim?target='+target+'&on='+(on?1:0)+'&value='+(on?v.trim():0));
  if(r&&!r.ok)alert('Value must be between -40 and 100 C.');
  setTimeout(update,200);
}

async function netKeep(){
  const r=await call('/wifi/confirm');
  if(!r)return;
  alert(r.ok?'Saved.':'Too late - the previous settings have already been restored.');
  update();
}

async function netRevert(){
  const r=await call('/wifi/revert');
  if(!r)return;
  alert(r.ok?'Previous settings restored. You may need to reconnect.'
            :'Nothing to revert.');
  update();
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
// The trial prompt is deliberately NOT on this page. Applying an access point
// change disconnects the browser, so by the time the operator can act on it
// they have reconnected and landed on the control page. Putting it there costs
// them nothing and saves a navigation while a 60-second clock runs.
static const char WIFI_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Settings</title><link rel='stylesheet' href='/style.css?v=700'>
</head><body><div class='container'>
<h1>Settings</h1>

<div class='banner hidden' id='pwNag'>
<b>The web password is still the default.</b> Anyone who can reach this board can
switch on the 2200 W element.
</div>

<div class='card'><h2>Web Password</h2>
<p class='note'>Guards this page, the controls and the firmware upload. It is
not the access point password below, and it is not the one
<code>pio run -e ota</code> uses &mdash; that one is compiled in, so changing
this cannot lock you out of a board with no USB port.</p>
<label class='field'>Current password</label>
<input type='password' id='pw_cur' maxlength='63'>
<label class='field'>New password &mdash; at least 8 characters</label>
<input type='password' id='pw_new' maxlength='63'>
<label class='field'>Repeat new password</label>
<input type='password' id='pw_new2' maxlength='63'>
<div class='actions'><button class='btn' onclick='savePw()'>Change password</button></div>
</div>

<div class='card'><h2>Current</h2>
<div class='info-row'><span class='info-label'>Access point:</span><span class='info-value' id='c_ap'>-</span></div>
<div class='info-row'><span class='info-label'>Address:</span><span class='info-value' id='c_ip'>-</span></div>
<div class='info-row'><span class='info-label'>Network:</span><span class='info-value' id='c_sta'>-</span></div>
<h2 style='font-size:16px;'>Connected devices</h2>
<div id='clients'></div>
</div>

<div class='card'><h2>Access Point</h2>
<label class='field'>Network name (SSID)</label>
<input type='text' id='ap_ssid' maxlength='32' autocapitalize='off' autocorrect='off'
 oninput='mark("ap_ssid")'>
<label class='field'>Password &mdash; blank leaves it unchanged</label>
<input type='password' id='ap_pass' maxlength='63' placeholder='unchanged'
 oninput='mark("ap_pass")'>

<div class='control' style='margin-top:14px;'>
<span class='mode-label' id='addrMode'>Use a .local name</span>
<label class='switch'><input type='checkbox' id='mdns_en' onchange='addrSwitch()'>
<span class='slider'></span></label></div>
<label class='field' id='addrLabel'>Address</label>
<input type='text' id='ap_addr' maxlength='32' autocapitalize='off' autocorrect='off'
 oninput='mark("ap_addr")'>
<p class='note' id='addrNote'></p>

<div class='actions'><button class='btn' onclick='applyAp()'>Apply</button></div>
<p class='note'>Applying disconnects you. Reconnect and the control page will ask
whether to keep the new settings; ignore it for 60 s and the old ones come back.</p>
</div>

<div class='card'><h2><span>Join Network</span>
<label class='switch'><input type='checkbox' id='sta_en' onchange='staSwitch()'>
<span class='slider'></span></label></h2>
<p class='note'>Additional, not instead &mdash; the access point stays up either
way, so nothing here can lock you out and nothing here needs confirming.</p>

<label class='field'>Network</label>
<select id='sta_list' onchange='pickNet()'><option value=''>-</option></select>
<input type='text' id='sta_ssid' class='hidden' maxlength='32' autocapitalize='off'
 autocorrect='off' placeholder='Network name' oninput='mark("sta_ssid")'>
<p class='note' id='scanNote'></p>

<label class='field'>Password</label>
<input type='password' id='sta_pass' maxlength='63' oninput='mark("sta_pass")'>

<div class='actions'><button class='btn' onclick='join()'>Join</button></div>
</div>

<div class='btnrow'>
<button class='btn danger' onclick='factory()'>Reset to Default</button>
<button class='btn' onclick='location.href="/"'>Back to Control</button>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
let cur={};                     // last /wifi/status
let scanning=false;
let lastNets=[];                // results of the most recent completed scan
let scanTries=0;

// Per-field edit state. The previous version only refused to overwrite a field
// while it held focus, so the 2 s poll put every value back the moment the
// operator clicked anywhere else - which is exactly why the Join switch
// appeared to turn itself off after a few seconds.
const dirty={};
function mark(id){dirty[id]=true;}
function clean(){for(const k in dirty)delete dirty[k];}
function canSet(id){return !dirty[id]&&$(id)!==document.activeElement;}
function esc(s){return String(s).replace(/[&<>"']/g,c=>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}

// Changing the password ends every session including this one, which is the
// point: it is how an operator responds to thinking someone else has it. The
// redirect to the login page is the confirmation that it worked.
async function savePw(){
  const cur=$('pw_cur').value, np=$('pw_new').value;
  if(np.length<8){alert('The new password must be at least 8 characters.');return;}
  if(np!==$('pw_new2').value){alert('The two new passwords do not match.');return;}
  const r=await fetch('/password',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'cur='+encodeURIComponent(cur)+'&next='+encodeURIComponent(np)});
  if(r.status==401){alert('That is not the current password.');return;}
  if(!r.ok){const d=await r.json().catch(()=>({}));
    alert(d.error||'Could not change the password.');return;}
  alert('Password changed. Log in again with the new one.');
  location.href='/login';
}

async function update(){
  try{
    const res=await fetch('/wifi/status');
    if(res.status==401){location.href='/login';return;}
    const d=await res.json();
    cur=d;
    $('pwNag').classList.toggle('hidden',!(d.pw_def==1));
    $('c_ap').textContent=d.ap_ssid;
    $('c_ip').textContent=d.ap_ip+(d.mdns_en?('  /  '+d.host+'.local'):'');
    $('c_sta').textContent=d.sta_en?(d.sta_conn?(d.sta_ssid+' - '+d.sta_ip)
                                               :(d.sta_ssid+' - connecting')):'off';
    $('clients').innerHTML=(d.clients&&d.clients.length)?d.clients.map(c=>
      "<div class='info-row'><span class='info-label'>"+esc(c.mac)+
      "</span><span class='info-value'>"+esc(c.ip)+"</span></div>").join('')
      :"<div class='info-row'><span class='info-label'>none</span></div>";

    if(canSet('ap_ssid'))$('ap_ssid').value=d.ap_ssid;
    if(canSet('mdns_en')){$('mdns_en').checked=d.mdns_en;}
    if(canSet('ap_addr'))$('ap_addr').value=d.mdns_en?d.host:d.ap_ip_cfg;
    addrLabels();

    if(canSet('sta_en'))$('sta_en').checked=d.sta_en;
    // lastNets, not an empty list: rebuilding from nothing every two seconds
    // would throw away the scan the operator is in the middle of choosing from.
    if(canSet('sta_list')&&!scanning)fillList(lastNets);
    if(canSet('sta_pass'))$('sta_pass').placeholder=
      (selected()===d.sta_ssid&&d.sta_ssid)?'saved':'';
  }catch(e){console.error(e);}
}

// ---------------------------------------------------------------- address
function addrLabels(){
  const m=$('mdns_en').checked;
  $('addrLabel').textContent=m?'Hostname':'IP address';
  $('addrNote').textContent=m
    ?'Reachable as '+($('ap_addr').value||'name')+'.local on the access point and '+
     'on your home network. The numeric address keeps working either way.'
    :'The access point address, e.g. 192.168.4.1.';
}

function addrSwitch(){
  mark('mdns_en');mark('ap_addr');
  $('ap_addr').value=$('mdns_en').checked?(cur.host||''):(cur.ap_ip_cfg||'');
  addrLabels();
}

async function applyAp(){
  const p=$('ap_pass').value;
  if(p.length>0&&p.length<8){alert('The access point password must be at least 8 characters.');return;}
  const addr=$('ap_addr').value.trim();
  if(!addr){alert('Enter an address.');return;}
  if(!confirm('You will be disconnected. Reconnect and the control page will ask '+
    'whether to keep the new settings, or the old ones come back after 60 s.'))return;
  const m=$('mdns_en').checked;
  const q='/wifi/ap?ap_ssid='+encodeURIComponent($('ap_ssid').value)+
    '&ap_pass='+encodeURIComponent(p)+
    '&mdns_en='+(m?1:0)+
    '&'+(m?'host=':'ap_ip=')+encodeURIComponent(addr);
  const r=await fetch(q,{method:'POST'});
  if(!r.ok){alert('Refused: check the name, password and address.');return;}
  $('ap_pass').value='';clean();
  alert('Applying now. Reconnect, then keep or revert from the control page.');
}

// ------------------------------------------------------------------- join
function selected(){
  const v=$('sta_list').value;
  return v=='?'?$('sta_ssid').value.trim():v;
}

function pickNet(){
  mark('sta_list');
  const other=$('sta_list').value=='?';
  $('sta_ssid').classList.toggle('hidden',!other);
  if(other)$('sta_ssid').focus();
  $('sta_pass').placeholder=(selected()===cur.sta_ssid&&cur.sta_ssid)?'saved':'';
}

function fillList(nets){
  const want=selected()||cur.sta_ssid||'';
  let seen=false;
  let html="<option value=''>-</option>";
  nets.forEach(function(n){
    if(n.ssid===want)seen=true;
    html+="<option value='"+esc(n.ssid)+"'>"+esc(n.ssid)+
      (n.enc?'':' (open)')+"  "+n.rssi+"dBm</option>";
  });
  if(want&&!seen)html+="<option value='"+esc(want)+"'>"+esc(want)+"</option>";
  html+="<option value='?'>Other (type the name)</option>";
  $('sta_list').innerHTML=html;
  $('sta_list').value=want||'';
  $('sta_ssid').classList.toggle('hidden',$('sta_list').value!='?');
}

async function staSwitch(){
  mark('sta_en');
  if($('sta_en').checked){startScan();return;}
  // Switching off is applied straight away: it cannot cost anyone their access
  // point, so there is nothing to confirm and nothing to press Join for.
  const r=await fetch('/wifi/sta?sta_en=0',{method:'POST'});
  if(!r.ok){alert('Could not switch the network off.');return;}
  scanning=false;$('scanNote').textContent='';
  clean();update();
}

async function startScan(){
  scanning=true;scanTries=0;
  $('scanNote').textContent='Scanning...';
  await fetch('/wifi/scan?start=1');
  setTimeout(scanTick,1200);
}

// -1 is WIFI_SCAN_RUNNING and -2 is WIFI_SCAN_FAILED, which is also what an
// idle radio reports. Since -2 can show up in the moment between asking for a
// scan and the radio starting one, it is only taken as a real answer after a
// few polls rather than on the first sight of it.
async function scanTick(){
  if(!scanning)return;
  try{
    const d=await(await fetch('/wifi/scan')).json();
    if(d.state==-1||(d.state==-2&&scanTries<3)){
      scanTries++;setTimeout(scanTick,1200);return;
    }
    scanning=false;
    if(d.state<0){
      $('scanNote').textContent='No networks found. Flip the switch to scan again.';
      return;
    }
    lastNets=d.nets;
    fillList(lastNets);
    $('scanNote').textContent=lastNets.length+' networks found. '+
      'Scanning briefly interrupts the access point.';
  }catch(e){scanning=false;console.error(e);}
}

async function join(){
  const ssid=selected();
  if(!ssid){alert('Choose a network first.');return;}
  const p=$('sta_pass').value;
  if(p.length>0&&p.length<8){alert('The password must be at least 8 characters.');return;}
  const r=await fetch('/wifi/sta?sta_en=1&sta_ssid='+encodeURIComponent(ssid)+
    '&sta_pass='+encodeURIComponent(p),{method:'POST'});
  if(!r.ok){alert('Refused: check the network name and password.');return;}
  $('sta_pass').value='';clean();
  alert('Joining '+ssid+'. Your access point is unaffected.');
  update();
}

// ------------------------------------------------------------------ reset
async function factory(){
  if(!confirm('Reset every WiFi setting to its default?\n\nThe access point name, '+
    'password and address all go back to the factory values, and any home network '+
    'is forgotten. You will be disconnected.'))return;
  const r=await fetch('/wifi/reset',{method:'POST'});
  if(!r.ok){alert('Reset failed.');return;}
  clean();
  alert('Applying the default settings. Reconnect, then keep or revert from the '+
    'control page within 60 seconds.');
}

setInterval(update,2000); window.onload=update;
</script></body></html>)HTML";

static void handleSettingsPage() {
  // Redirect rather than 401 - this is a page request typed or tapped by a
  // person, and a bare JSON error in the address bar is not an answer.
  if (!authCheck(server)) {
    server.sendHeader(F("Location"), F("/login"));
    server.send(302, F("text/plain"), F("login required"));
    return;
  }
  server.send_P(200, PSTR("text/html"), WIFI_HTML);
}

// ============================================================================
// WEB SERVER - STATISTICS PAGE
// ============================================================================
static const char STATS_HTML[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Statistics</title><link rel='stylesheet' href='/style.css?v=700'>
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
<div class='btnrow'>
<button class='btn danger' onclick='reset()'>Reset Statistics</button>
<button class='btn' onclick='location.href="/"'>Back to Control</button>
</div>
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
// LOGIN
// ============================================================================
static void handleLoginPage() {
  server.send_P(200, PSTR("text/html"), LOGIN_HTML);
}

// ============================================================================
// STATUS JSON - the single source of truth for every dynamic value in the UI
// ============================================================================
static void handleStatusJSON() {
  // Readable without a session, by design: this device is a heater switch
  // first, and glancing at the greenhouse temperature should not need a
  // password. The Host allowlist still applies - that is what needSession=false
  // leaves in place - so a rebound DNS name cannot read it either.
  if (!requireAuth(server, false)) return;

  // Sized for the full payload with every sensor present and every optional
  // field populated. A silently truncated /status would take the whole UI down
  // with it, so there is deliberate headroom here.
  StaticJsonDocument<1280> doc;

  doc["fw"] = FIRMWARE_VERSION;
  doc["up"] = millis() / 1000;

  // Drives the login/logout button and the default-password nag. Both are
  // presentation only; nothing here is a permission check.
  doc["auth"] = authCheck(server) ? 1 : 0;
  doc["pw_def"] = authPasswordIsDefault() ? 1 : 0;

  doc["avg"] = sensorData.averageTemp;
  doc["heat_temp"] = sensorData.heaterTemp;
  doc["heater"] = heaterOn ? 1 : 0;
  doc["fans"] = fansOn ? 1 : 0;
  doc["cooldown"] = (currentMode == MODE_COOLDOWN) ? 1 : 0;
  doc["mode"] = modeName(currentMode);
  doc["fault"] = (int)activeFault;
  doc["fault_name"] = faultName(activeFault);

  // The critical trip is the only fault manual mode honours, so the UI needs to
  // tell it apart from the sensor faults - which stay latched but are ignored
  // while manual is on. Without this the page would keep the heater badge
  // un-tappable for a condition the firmware is deliberately overlooking.
  doc["crit"] = (activeFault == FAULT_HEATER_CRITICAL) ? 1 : 0;

  doc["manual"] = settings.manualMode ? 1 : 0;
  doc["man_heat"] = manualHeaterReq ? 1 : 0;
  doc["man_fan"] = manualFanReq ? 1 : 0;
  doc["hold"] = manualHoldReason();      // const char*: stored by pointer

  // Non-zero only while the element is running with no zone probe, which is the
  // one state where nothing but a countdown is limiting it. The UI shows this
  // in the warning style rather than as small print.
  doc["blind"] = manualBlindSecondsLeft();

  // Drives the keep/revert overlay on the control page. It lives here rather
  // than on /wifi/status because the control page is where the operator lands
  // after reconnecting, and it polls this already.
  doc["trial"] = netTrialSecondsLeft();

  doc["sim_air"] = sim.airActive ? 1 : 0;
  doc["sim_air_v"] = sim.air;
  doc["sim_heat"] = sim.heaterActive ? 1 : 0;
  doc["sim_heat_v"] = sim.heater;

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
  if (!requireAuth(server)) return;

  // None of these govern anything in manual mode, which is why the two cards
  // carrying them are dimmed out. Refused here as well so a stale tab on
  // another phone cannot quietly move a setpoint the page says is inactive.
  if (settings.manualMode) {
    server.send(409, F("application/json"), F("{\"error\":\"not used in manual mode\"}"));
    return;
  }
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
  if (!requireAuth(server)) return;
  resetStats();
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// ============================================================================
// MANUAL MODE AND SIMULATION
// ============================================================================
// These handlers set a REQUEST and nothing more. Whether the relay actually
// moves is control.cpp's decision. There is no digitalWrite on a relay pin in
// this file, and there must never be one - that stays true even though manual
// mode now has almost nothing left for control.cpp to decide.
static bool argIsOn(const String& name) { return server.arg(name) == "1"; }

static void handleManual() {
  // Deliberately NOT exempt when switching manual off: leaving manual mode
  // hands the element back to the controller, which may well decide to heat.
  // Only an explicit output-OFF is the unconditionally safe direction.
  if (!requireAuth(server)) return;

  if (!server.hasArg(F("on"))) {
    server.send(400, F("application/json"), F("{\"error\":\"missing on\"}"));
    return;
  }
  setManualMode(argIsOn(F("on")), millis());
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleOutput() {
  // The arguments are read before the gate, because WHICH DIRECTION is being
  // requested decides whether a session is needed at all.
  if (!server.hasArg(F("dev")) || !server.hasArg(F("on"))) {
    if (!requireAuth(server, false)) return;
    server.send(400, F("application/json"), F("{\"error\":\"missing dev/on\"}"));
    return;
  }

  const String dev = server.arg(F("dev"));
  const bool on = argIsOn(F("on"));

  // SWITCHING SOMETHING OFF NEVER REQUIRES A SESSION, and that asymmetry is the
  // point. Every failure mode of the code above this line - an expired cookie,
  // a forgotten password, a bug in auth.cpp - must still leave a way to shed
  // 2200 W. The Host allowlist still applies in both directions.
  if (!requireAuth(server, /*needSession=*/on)) return;

  // A stale tab on someone else's phone must not be able to drive a relay
  // after the controller has been put back into automatic.
  if (!settings.manualMode) {
    server.send(409, F("application/json"), F("{\"error\":\"not in manual mode\"}"));
    return;
  }

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
  if (!requireAuth(server)) return;
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
  // Gated: this lists the SSIDs the board knows and the MAC of everything
  // associated with it. None of that belongs to a passer-by.
  if (!requireAuth(server)) return;

  StaticJsonDocument<1024> doc;
  const WifiConfig& cfg = netLiveConfig();

  doc["pw_def"]    = authPasswordIsDefault() ? 1 : 0;
  doc["ap_ssid"]   = cfg.apSsid;
  doc["ap_ip"]     = netApAddress();      // what the radio actually has
  doc["ap_ip_cfg"] = cfg.apIp;            // what is configured, for the form
  doc["mdns_en"]   = cfg.mdnsEnabled ? 1 : 0;
  doc["host"]      = cfg.hostname;
  doc["sta_en"]    = cfg.staEnabled ? 1 : 0;
  doc["sta_ssid"]  = cfg.staSsid;
  doc["sta_conn"]  = netStaConnected() ? 1 : 0;
  doc["sta_ip"]    = netStaAddress();
  doc["trial"]     = netTrialSecondsLeft();

  ApClient clients[AP_CLIENT_MAX];
  const int n = netApClients(clients, AP_CLIENT_MAX);
  JsonArray arr = doc.createNestedArray("clients");
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.createNestedObject();
    // char* rather than const char*: ArduinoJson duplicates the former into the
    // document pool, which is what these stack buffers need.
    o["mac"] = clients[i].mac;
    o["ip"]  = clients[i].ip;
  }

  if (doc.overflowed()) {
    Serial.println(F("WARNING: /wifi/status JSON overflowed - increase the document size"));
  }

  String output;
  output.reserve(measureJson(doc) + 1);
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}

static void copyArg(char* dst, size_t cap, const String& value) {
  strlcpy(dst, value.c_str(), cap);
}

// Access point side: name, password, address and the mDNS switch. Goes on
// trial, because every one of these can cost the operator their way in.
static void handleWifiAp() {
  if (!requireAuth(server)) return;

  // Start from what is running now, so an omitted field means "leave it alone".
  // Passwords are never sent to the browser, so a blank password box has to
  // mean unchanged rather than empty - otherwise merely opening this page and
  // pressing Apply would drop the access point to an open one.
  WifiConfig cfg = netLiveConfig();

  if (server.hasArg(F("ap_ssid"))) copyArg(cfg.apSsid, WIFI_SSID_LEN, server.arg(F("ap_ssid")));
  if (server.hasArg(F("ap_ip")))   copyArg(cfg.apIp, WIFI_ADDR_LEN, server.arg(F("ap_ip")));
  if (server.hasArg(F("host")))    copyArg(cfg.hostname, WIFI_HOST_LEN, server.arg(F("host")));
  if (server.hasArg(F("mdns_en"))) cfg.mdnsEnabled = argIsOn(F("mdns_en"));

  const String apPass = server.arg(F("ap_pass"));
  if (apPass.length() > 0) copyArg(cfg.apPass, WIFI_PASS_LEN, apPass);

  if (!netApplyAp(cfg)) {
    server.send(400, F("application/json"), F("{\"error\":\"invalid configuration\"}"));
    return;
  }

  Serial.println(F("[WiFi] AP configuration queued on trial"));
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// Station side: applied and committed immediately. No trial, because the access
// point is untouched - a wrong password here costs an address the operator did
// not have a moment ago, not their way into the board.
static void handleWifiSta() {
  if (!requireAuth(server)) return;

  WifiConfig cfg = netLiveConfig();

  const String ssid = server.hasArg(F("sta_ssid")) ? server.arg(F("sta_ssid"))
                                                   : String(cfg.staSsid);
  const String pass = server.arg(F("sta_pass"));

  // The password box is labelled just "Password", and what is typed in it is
  // the password. Blank is only treated as "keep the stored one" when the
  // network has not changed - which is what makes rejoining the same network
  // work without retyping, without the box ever lying about what it means. A
  // blank box against a DIFFERENT network is an open network, as it reads.
  const bool sameNetwork = ssid.equals(cfg.staSsid);
  if (pass.length() > 0)      copyArg(cfg.staPass, WIFI_PASS_LEN, pass);
  else if (!sameNetwork)      cfg.staPass[0] = 0;

  copyArg(cfg.staSsid, WIFI_SSID_LEN, ssid);
  if (server.hasArg(F("sta_en"))) cfg.staEnabled = argIsOn(F("sta_en"));

  if (!netApplySta(cfg)) {
    server.send(400, F("application/json"), F("{\"error\":\"invalid configuration\"}"));
    return;
  }

  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// Asynchronous by construction - see netStartScan(). ?start=1 kicks one off;
// a bare GET only reports, so the page can poll without restarting the scan.
static void handleWifiScan() {
  if (!requireAuth(server)) return;

  if (server.hasArg(F("start"))) netStartScan();

  const int state = netScanState();
  const int found = (state > 0) ? state : 0;
  const int show = (found > 20) ? 20 : found;

  // Sized from the actual result count rather than guessed at: an SSID can be
  // 32 characters, and twenty of them will not fit in any sensible fixed pool.
  DynamicJsonDocument doc(256 + show * 96);
  doc["state"] = state;

  JsonArray nets = doc.createNestedArray("nets");
  for (int i = 0; i < show; i++) {
    JsonObject o = nets.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["enc"]  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? 0 : 1;
  }

  String output;
  output.reserve(measureJson(doc) + 1);
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}

static void handleWifiConfirm() {
  if (!requireAuth(server)) return;
  if (!netConfirm()) {
    server.send(409, F("application/json"), F("{\"error\":\"no trial running\"}"));
    return;
  }
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleWifiRevert() {
  if (!requireAuth(server)) return;
  if (!netRevert()) {
    server.send(409, F("application/json"), F("{\"error\":\"no trial running\"}"));
    return;
  }
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

static void handleWifiReset() {
  if (!requireAuth(server)) return;
  if (!netFactoryReset()) {
    server.send(500, F("application/json"), F("{\"error\":\"reset failed\"}"));
    return;
  }
  Serial.println(F("[WiFi] Default settings queued on trial"));
  server.send(200, F("application/json"), F("{\"ok\":1}"));
}

// ============================================================================
void webBegin() {
  // Before any route is registered: authBegin() seeds the password on first
  // boot and makes the one collectHeaders() call the whole firmware gets.
  authBegin(server);

  // Static shells and the readings are public. Everything that CHANGES
  // something is gated inside its handler, with one deliberate exception:
  // POST /output with on=0 needs no session, because shedding 2200 W must
  // never depend on this file working correctly.
  server.on("/", handleRoot);
  server.on("/style.css", handleStyleCSS);
  server.on("/ui.js", handleUiJS);
  server.on("/status", handleStatusJSON);
  server.on("/stats", handleStats);

  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/login", HTTP_POST, []() { authLogin(server); });
  server.on("/logout", HTTP_POST, []() { authLogout(server); });
  server.on("/password", HTTP_POST, []() {
    if (!requireAuth(server)) return;
    authChangePassword(server);
  });

  server.on("/adjust", handleAdjust);
  server.on("/resetstats", handleResetStats);

  server.on("/manual", HTTP_POST, handleManual);
  server.on("/output", HTTP_POST, handleOutput);
  server.on("/sim", HTTP_POST, handleSim);

  server.on("/settings", HTTP_GET, handleSettingsPage);
  server.on("/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/wifi/ap", HTTP_POST, handleWifiAp);
  server.on("/wifi/sta", HTTP_POST, handleWifiSta);
  server.on("/wifi/confirm", HTTP_POST, handleWifiConfirm);
  server.on("/wifi/revert", HTTP_POST, handleWifiRevert);
  server.on("/wifi/reset", HTTP_POST, handleWifiReset);

  server.begin();
  Serial.println(F("Web server started"));
}

void webLoop() {
  server.handleClient();
}
