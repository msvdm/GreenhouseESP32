#pragma once
#include <Arduino.h>
#include "config.h"

// Static pages. The stylesheet URL carries the firmware version, so browsers
// never pair a new page with a cached old stylesheet after an update.

#define HEAD(title) "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>" \
  "<meta name='viewport' content='width=device-width,initial-scale=1'>" \
  "<title>" title "</title><link rel='stylesheet' href='/app.css?v=" FW_VERSION "'></head><body>"

static const char APP_CSS[] PROGMEM = R"CSS(
:root{--bg:#f3f5f2;--card:#fff;--text:#1c231f;--dim:#69756e;--line:#dce2dd;--accent:#2f8f4e;
--heat:#d9480f;--fan:#1c7ed6;--err:#c92a2a;--warn:#e67700}
@media(prefers-color-scheme:dark){:root{--bg:#111513;--card:#1b211e;--text:#e5ebe7;--dim:#919e96;
--line:#2b3430;--accent:#4cc27a;--heat:#ff7a45;--fan:#4dabf7;--err:#ff6b6b;--warn:#ffa94d}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:12px}
header h1{font-size:19px;margin:0}
nav a,nav button{color:var(--dim);text-decoration:none;font-size:14px;margin-left:12px;background:none;border:0;cursor:pointer;padding:0}
nav a:hover,nav button:hover{color:var(--text)}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:12px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--dim);margin:0 0 10px}
.big{font-size:52px;font-weight:600;line-height:1.1;font-variant-numeric:tabular-nums}
.big small{font-size:24px;color:var(--dim);font-weight:400}
.lines{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:12px}
.line{border-top:1px solid var(--line);padding-top:8px}
.line b{font-variant-numeric:tabular-nums}
.probes{color:var(--dim);font-size:13px;font-variant-numeric:tabular-nums}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
.grid2 .card{margin-bottom:0}
@media(max-width:480px){.grid2,.lines{grid-template-columns:1fr}}
.relay .state{display:flex;align-items:center;justify-content:space-between;margin-bottom:12px}
.pill{font-weight:700;font-size:13px;padding:4px 12px;border-radius:999px;border:1px solid var(--line);color:var(--dim)}
.pill.on.heat{background:var(--heat);border-color:var(--heat);color:#fff}
.pill.on.fan{background:var(--fan);border-color:var(--fan);color:#fff}
.why{color:var(--dim);font-size:13px}
.seg{display:flex;border:1px solid var(--line);border-radius:8px;overflow:hidden}
.seg button{flex:1;padding:9px 6px;border:0;background:none;color:var(--text);font:inherit;cursor:pointer}
.seg button+button{border-left:1px solid var(--line)}
.seg button.sel{background:var(--accent);color:#fff;font-weight:600}
.hint{color:var(--dim);font-size:13px;margin:8px 0 0}
label{display:block;font-size:14px;margin:10px 0 4px}
input[type=number],input[type=text],input[type=password],input[type=file]{width:100%;padding:9px 10px;
border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--text);font:inherit}
.row3{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
@media(max-width:480px){.row3{grid-template-columns:1fr}}
.btn{margin-top:14px;padding:10px 18px;border:0;border-radius:8px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
.btn.alt{background:none;border:1px solid var(--line);color:var(--text)}
.msg{font-size:14px;margin-top:10px;min-height:1em}
.msg.err{color:var(--err)}
.banner{background:var(--warn);color:#000;border-radius:10px;padding:10px 14px;margin-bottom:12px;font-size:14px}
.err{color:var(--err)}
.check{display:flex;align-items:center;gap:8px;margin-top:12px}
.check input{width:18px;height:18px}
footer{color:var(--dim);font-size:13px;text-align:center;margin:16px 0}
progress{width:100%;height:14px;margin-top:12px}
.hidden{display:none}
)CSS";

static const char JS_COMMON[] PROGMEM = R"JS(
const $=id=>document.getElementById(id);
async function post(url,data){
  const r=await fetch(url,{method:'POST',body:new URLSearchParams(data||{})});
  if(r.status==401){location.href='/login';throw new Error('login');}
  let j={};try{j=await r.json();}catch(e){}
  if(!r.ok)throw new Error(j.error||('HTTP '+r.status));
  return j;
}
function say(id,text,err){const m=$(id);m.textContent=text;m.className='msg'+(err?' err':'');}
function logout(){post('/logout').finally(()=>location.href='/login');}
)JS";

// ----------------------------------------------------------------------------
static const char LOGIN_HTML[] PROGMEM = HEAD("Greenhouse - Login") R"HTML(
<main style='max-width:360px;padding-top:12vh'>
<div class='card'>
<h2>Greenhouse controller</h2>
<form id='f'>
<label for='p'>Password</label>
<input type='password' id='p' autocomplete='current-password' autofocus required>
<button class='btn' style='width:100%'>Log in</button>
<div id='m' class='msg'></div>
</form></div></main>
<script>
const $=id=>document.getElementById(id);
$('f').onsubmit=async e=>{
  e.preventDefault();
  const r=await fetch('/login',{method:'POST',body:new URLSearchParams({pass:$('p').value})});
  if(r.ok){location.href='/';return;}
  let j={};try{j=await r.json();}catch(x){}
  $('m').textContent=j.error||'Login failed';$('m').className='msg err';
};
</script></body></html>)HTML";

// ----------------------------------------------------------------------------
static const char INDEX_HTML[] PROGMEM = HEAD("Greenhouse") R"HTML(
<main>
<header><h1>Greenhouse</h1>
<nav><a href='/settings'>Settings</a><a href='/update'>Update</a><button onclick='logout()'>Log out</button></nav></header>

<div id='pwdef' class='banner hidden'>The web password is still the factory default. Change it under <a href='/settings'>Settings</a>.</div>

<div class='card'>
<h2>Average temperature</h2>
<div class='big' id='avg'>--.-<small> &deg;C</small></div>
<div class='lines'>
<div class='line'>Line 1 (GPIO 4): <b id='l1'>--</b><div class='probes' id='p1'></div></div>
<div class='line'>Line 2 (GPIO 15): <b id='l2'>--</b><div class='probes' id='p2'></div></div>
</div></div>

<div class='grid2'>
<div class='card relay'>
<h2>Heating</h2>
<div class='state'><span class='pill heat' id='hs'>OFF</span><span class='why' id='hw'></span></div>
<div class='seg' id='hm'><button data-v='auto'>Auto</button><button data-v='on'>On</button><button data-v='off'>Off</button></div>
</div>
<div class='card relay'>
<h2>Fans</h2>
<div class='state'><span class='pill fan' id='fs'>OFF</span><span class='why' id='fw'></span></div>
<div class='seg' id='fm'><button data-v='auto'>Auto</button><button data-v='on'>On</button><button data-v='off'>Off</button></div>
</div></div>

<div class='card'>
<h2>Heating mode</h2>
<div class='seg' id='room'><button data-v='0'>Zone &ndash; heaters only</button><button data-v='1'>Room &ndash; heaters + fans</button></div>
<p class='hint'>Room mode runs the ceiling fans whenever the heating is on, to spread the warm air through the whole greenhouse.</p>
</div>

<div class='card'>
<h2>Thermostat</h2>
<form id='sp'>
<div class='row3'>
<div><label for='heatOn'>Heating on at &le; &deg;C</label><input type='number' id='heatOn' step='0.1' min='0' max='35' required></div>
<div><label for='coolOn'>Fans on at &ge; &deg;C</label><input type='number' id='coolOn' step='0.1' min='5' max='50' required></div>
<div><label for='hyst'>Hysteresis &deg;C</label><input type='number' id='hyst' step='0.1' min='0.2' max='5' required></div>
</div>
<p class='hint' id='band'></p>
<button class='btn'>Save</button>
<div id='spm' class='msg'></div>
</form></div>

<footer id='foot'></footer>
</main>
<script src='/common.js?v=)HTML" FW_VERSION R"HTML('></script>
<script>
const f1=v=>v==null?'--.-':v.toFixed(1);
let filled=false;
function seg(id,val){for(const b of $(id).children)b.classList.toggle('sel',b.dataset.v==String(val));}
function band(){
  const h=+$('heatOn').value,c=+$('coolOn').value,y=+$('hyst').value;
  $('band').textContent='Heating: on at '+h.toFixed(1)+', off at '+(h+y).toFixed(1)+' °C.  Fans: on at '+c.toFixed(1)+', off at '+(c-y).toFixed(1)+' °C.';
}
function hrs(s){return (s/3600).toFixed(1)+' h';}
function up(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+h+'h '+m+'m';}
async function refresh(){
  let s;
  try{const r=await fetch('/api/status');if(r.status==401){location.href='/login';return;}s=await r.json();}catch(e){return;}
  $('avg').innerHTML=(s.avg==null?'<span class="err">--.-</span>':f1(s.avg))+'<small> &deg;C</small>';
  s.lines.forEach((l,i)=>{
    $('l'+(i+1)).textContent=l.avg==null?(l.n?'no reading':'no probes'):f1(l.avg)+' °C';
    $('l'+(i+1)).className=l.avg==null?'err':'';
    $('p'+(i+1)).textContent=l.t.length?l.n+' probe'+(l.n==1?'':'s')+': '+l.t.map(f1).join(', '):'';
  });
  $('hs').textContent=s.heater?'ON':'OFF';$('hs').classList.toggle('on',!!s.heater);
  $('fs').textContent=s.fan?'ON':'OFF';$('fs').classList.toggle('on',!!s.fan);
  $('hw').textContent=s.heaterMode!='auto'?'manual':(s.avg==null?'no temperature':'');
  $('fw').textContent=s.fanMode!='auto'?'manual':({cool:'cooling',room:'room heating'}[s.fanWhy]||(s.avg==null?'no temperature':''));
  seg('hm',s.heaterMode);seg('fm',s.fanMode);seg('room',s.room);
  if(!filled){$('heatOn').value=s.heatOn;$('coolOn').value=s.coolOn;$('hyst').value=s.hyst;band();filled=true;}
  $('pwdef').classList.toggle('hidden',!s.pwDefault);
  $('foot').textContent='v'+s.fw+' · up '+up(s.up)+' · heating '+hrs(s.heaterSec)+' · fans '+hrs(s.fanSec)+
    (s.staIp?' · '+s.staIp+' ('+s.rssi+' dBm)':' · home WiFi not connected');
}
for(const [id,target] of [['hm','heater'],['fm','fan']]){
  $(id).onclick=e=>{const v=e.target.dataset.v;if(v)post('/api/relay',{target,mode:v}).then(refresh).catch(x=>alert(x.message));};
}
$('room').onclick=e=>{const v=e.target.dataset.v;if(v)post('/api/room',{on:v}).then(refresh).catch(x=>alert(x.message));};
for(const id of ['heatOn','coolOn','hyst'])$(id).oninput=band;
$('sp').onsubmit=async e=>{
  e.preventDefault();
  try{await post('/api/setpoints',{heatOn:$('heatOn').value,coolOn:$('coolOn').value,hyst:$('hyst').value});say('spm','Saved');filled=false;refresh();}
  catch(x){say('spm',x.message,1);}
};
refresh();setInterval(refresh,2000);
</script></body></html>)HTML";

// ----------------------------------------------------------------------------
static const char SETTINGS_HTML[] PROGMEM = HEAD("Greenhouse - Settings") R"HTML(
<main>
<header><h1>Settings</h1><nav><a href='/'>Back</a><button onclick='logout()'>Log out</button></nav></header>

<div class='card'>
<h2>Home WiFi</h2>
<form id='wf'>
<div class='check'><input type='checkbox' id='sta'><label for='sta' style='margin:0'>Join a home network</label></div>
<label for='ssid'>Network name (SSID)</label><input type='text' id='ssid' maxlength='32'>
<label for='spass'>Password</label><input type='password' id='spass' maxlength='63' placeholder='leave empty to keep the current one'>
<h2 style='margin-top:20px'>Board access point</h2>
<p class='hint' style='margin-top:0'>Always on, at http://192.168.4.1 &mdash; your way in if the home network fails.</p>
<label for='apssid'>Access point name</label><input type='text' id='apssid' maxlength='32' required>
<label for='appass'>Access point password (8-63 characters)</label><input type='password' id='appass' maxlength='63' placeholder='leave empty to keep the current one'>
<label for='host'>Hostname (http://name.local)</label><input type='text' id='host' maxlength='32' required>
<button class='btn'>Save and reboot</button>
<div id='wm' class='msg'></div>
</form></div>

<div class='card'>
<h2>Web password</h2>
<form id='pf'>
<label for='cur'>Current password</label><input type='password' id='cur' autocomplete='current-password' required>
<label for='nxt'>New password (at least 8 characters)</label><input type='password' id='nxt' autocomplete='new-password' minlength='8' required>
<button class='btn'>Change password</button>
<div id='pm' class='msg'></div>
</form></div>

<div class='card'>
<h2>Device</h2>
<button class='btn alt' style='margin-top:0' onclick='reboot()'>Reboot</button>
<div id='rm' class='msg'></div>
</div>
</main>
<script src='/common.js?v=)HTML" FW_VERSION R"HTML('></script>
<script>
fetch('/api/wifi').then(r=>{if(r.status==401)location.href='/login';return r.json();}).then(w=>{
  $('sta').checked=!!w.sta;$('ssid').value=w.ssid;$('apssid').value=w.apSsid;$('host').value=w.host;
});
$('wf').onsubmit=async e=>{
  e.preventDefault();
  try{
    await post('/api/wifi',{sta:$('sta').checked?1:0,ssid:$('ssid').value,pass:$('spass').value,
      apSsid:$('apssid').value,apPass:$('appass').value,host:$('host').value});
    say('wm','Saved. The board is rebooting - reconnect in about 15 seconds.');
  }catch(x){say('wm',x.message,1);}
};
$('pf').onsubmit=async e=>{
  e.preventDefault();
  try{await post('/api/password',{cur:$('cur').value,next:$('nxt').value});
    say('pm','Password changed - please log in again.');setTimeout(()=>location.href='/login',1500);}
  catch(x){say('pm',x.message,1);}
};
function reboot(){
  if(!confirm('Reboot the controller? Both relays switch off for a few seconds.'))return;
  post('/api/reboot').then(()=>say('rm','Rebooting...')).catch(x=>say('rm',x.message,1));
}
</script></body></html>)HTML";

// ----------------------------------------------------------------------------
static const char UPDATE_HTML[] PROGMEM = HEAD("Greenhouse - Firmware update") R"HTML(
<main>
<header><h1>Firmware update</h1><nav><a href='/'>Back</a></nav></header>
<div class='card'>
<h2>Upload firmware.bin</h2>
<p class='hint' style='margin-top:0'>Both relays switch off during the update. If the upload fails, the current firmware keeps running.</p>
<input type='file' id='file' accept='.bin'>
<button class='btn' onclick='upload()'>Upload</button>
<progress id='pr' class='hidden' max='100' value='0'></progress>
<div id='m' class='msg'></div>
</div></main>
<script>
const $=id=>document.getElementById(id);
function upload(){
  const f=$('file').files[0];
  if(!f){alert('Choose a firmware .bin file first.');return;}
  const fd=new FormData();fd.append('firmware',f,f.name);
  const x=new XMLHttpRequest();
  $('pr').classList.remove('hidden');$('m').className='msg';$('m').textContent='Uploading...';
  x.upload.onprogress=e=>{if(e.lengthComputable)$('pr').value=e.loaded*100/e.total;};
  x.onload=()=>{
    if(x.status==401){location.href='/login';return;}
    $('m').textContent=x.responseText;
    if(x.status==200)setTimeout(()=>location.href='/',15000);else $('m').className='msg err';
  };
  x.onerror=()=>{$('m').textContent='Connection closed - if the board rebooted, the update worked.';};
  x.open('POST','/update');x.send(fd);
}
</script></body></html>)HTML";
