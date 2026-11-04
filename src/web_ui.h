#pragma once

static const char INDEX_HTML[] PROGMEM = R"====(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CYD Dashboard</title>
<style>
:root{--bg:#0b1020;--card:#151d33;--line:#243153;--txt:#e8eefc;--dim:#8fa2c4;--acc:#00b4ff;--ok:#00d68f;--warn:#ffb400;--bad:#ff4d6d}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.45 system-ui,Segoe UI,Roboto,sans-serif}
header{padding:20px 16px 8px;max-width:900px;margin:0 auto}
h1{margin:0 0 10px;font-size:22px;letter-spacing:.3px}
h1 span{color:var(--acc)}
.chips{display:flex;flex-wrap:wrap;gap:8px}
.chip{background:var(--card);border:1px solid var(--line);border-radius:999px;padding:4px 10px;font-size:12px;color:var(--dim)}
.chip b{color:var(--txt);font-weight:600}
main{max-width:900px;margin:0 auto;padding:8px 16px 40px;display:grid;gap:14px;grid-template-columns:1fr}
@media(min-width:760px){main{grid-template-columns:1fr 1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}
.card h2{margin:0 0 12px;font-size:14px;text-transform:uppercase;letter-spacing:.8px;color:var(--dim)}
label{display:block;font-size:12px;color:var(--dim);margin:10px 0 4px}
input,select,button,textarea{font:inherit}
input[type=text],input[type=password],input[type=number],select,textarea{width:100%;background:#0e1527;border:1px solid var(--line);border-radius:9px;padding:9px 10px;color:var(--txt);outline:none}
textarea{resize:vertical;min-height:74px}
input:focus,select:focus,textarea:focus{border-color:var(--acc)}
.row{display:flex;gap:10px}.row>*{flex:1}
button{cursor:pointer;border:0;border-radius:9px;padding:10px 14px;background:var(--acc);color:#001522;font-weight:600}
button.ghost{background:#1d2842;color:var(--txt);border:1px solid var(--line)}
button.danger{background:#3a1524;color:#ff8fa3;border:1px solid #64233a}
button:active{transform:translateY(1px)}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:6px}
.msg{font-size:12px;color:var(--dim);min-height:16px;margin-top:8px}
.msg.ok{color:var(--ok)}.msg.bad{color:var(--bad)}
.hint{font-size:12px;color:var(--dim);margin-top:10px;line-height:1.5}
code{background:#0e1527;border:1px solid var(--line);border-radius:6px;padding:2px 6px;font-size:12px;word-break:break-all}
input[type=range]{width:100%;accent-color:var(--acc)}
.toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%) translateY(20px);background:#0e1527;border:1px solid var(--line);color:var(--txt);padding:10px 16px;border-radius:10px;opacity:0;transition:.25s;pointer-events:none;font-size:13px}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
</style>
</head>
<body>
<header>
  <h1>CYD <span>Dashboard</span></h1>
  <div class="chips" id="chips"><span class="chip">loading…</span></div>
</header>
<main>
  <section class="card">
    <h2>WiFi</h2>
    <label>Network</label>
    <div class="row">
      <select id="ssidList"><option value="">— scan for networks —</option></select>
      <button class="ghost" id="scanBtn" style="flex:0 0 90px">Scan</button>
    </div>
    <label>Password</label>
    <input type="password" id="pass" placeholder="WiFi password" autocomplete="off">
    <div class="actions"><button id="wifiBtn">Connect &amp; reboot</button></div>
    <div class="msg" id="wifiMsg"></div>
    <div class="hint">Current: <b id="curSsid">—</b>. If the new network fails, the CYD falls back to its own setup hotspot <code>CYD-Setup</code> at <code>192.168.4.1</code>.</div>
  </section>

  <section class="card">
    <h2>Location &amp; time</h2>
    <label>Find weather location</label>
    <div class="row">
      <input type="text" id="locationSearch" placeholder="City, postcode or place">
      <button class="ghost" id="locationBtn" style="flex:0 0 90px">Search</button>
    </div>
    <label id="locationResultsLabel" hidden>Matching locations</label>
    <select id="locationResults" hidden></select>
    <div class="msg" id="locationMsg">Search by city or postcode, then choose the correct match.</div>
    <label>Display label</label>
    <input type="text" id="city" placeholder="London">
    <div class="row">
      <div><label>Latitude</label><input type="number" id="lat" step="0.0001" placeholder="51.5072"></div>
      <div><label>Longitude</label><input type="number" id="lon" step="0.0001" placeholder="-0.1276"></div>
    </div>
    <div class="row">
      <div><label>Units</label><select id="units"><option value="metric">°C / km/h</option><option value="imperial">°F / mph</option></select></div>
      <div><label>Timezone (POSIX)</label><input type="text" id="tz" placeholder="GMT0BST,M3.5.0/1,M10.5.0"></div>
    </div>
    <label>News feeds (RSS, one URL per line)</label>
    <textarea id="feed" rows="3" placeholder="http://feeds.bbci.co.uk/news/rss.xml&#10;http://rss.cnn.com/rss/edition.rss&#10;https://news.google.com/rss"></textarea>
    <label>Tide port (Environment Agency live gauge)</label>
    <select id="tideStation">
      <option value="E70724">Aberdeen</option><option value="E72624">Avonmouth Portbury</option>
      <option value="E73824">Bangor</option><option value="E73224">Barmouth</option>
      <option value="E71924">Bournemouth</option><option value="E71224">Cromer</option>
      <option value="E71624">Dover</option><option value="E73124">Fishguard</option>
      <option value="E71424">Harwich</option><option value="E73424">Heysham</option>
      <option value="E72524">Hinkley Point</option><option value="E70524">Holyhead</option>
      <option value="E72424">Ilfracombe</option><option value="E71124">Immingham</option>
      <option value="E70224">Jersey</option><option value="E70324">Kinlochbervie</option>
      <option value="E70824">Leith</option><option value="E74424">Lerwick</option>
      <option value="E70124">Liverpool</option><option value="E73324">Llandudno</option>
      <option value="E70024">Lowestoft</option><option value="E73024">Milford Haven</option>
      <option value="E74024">Millport</option><option value="E72924">Mumbles</option>
      <option value="E71724">Newhaven</option><option value="E72224">Newlyn</option>
      <option value="E72824">Newport</option><option value="E70924">North Shields</option>
      <option value="E72124">Plymouth</option><option value="E73524">Port Erin</option>
      <option value="E73724">Portpatrick</option><option value="E73924">Portrush</option>
      <option value="E71824">Portsmouth</option><option value="E71524">Sheerness</option>
      <option value="E72324">St Marys</option><option value="E74324">Stornoway</option>
      <option value="E74224">Tobermory</option><option value="E70424">Ullapool</option>
      <option value="E71324">Washpile</option><option value="E72024">Weymouth</option>
      <option value="E71024">Whitby</option><option value="E70624">Wick</option>
      <option value="E73624">Workington</option>
    </select>
    <div class="actions"><button id="saveBtn">Save settings</button></div>
    <div class="msg" id="saveMsg"></div>
    <div class="hint">Location search by Open-Meteo and GeoNames. Manual coordinates remain available for exact positioning.</div>
  </section>

  <section class="card">
    <h2>Display</h2>
    <label>Automatic brightness</label>
    <select id="autoBrightness"><option value="0">Off</option><option value="1">On — use ambient light sensor</option></select>
    <label>When the PC agent stops responding</label>
    <select id="sleepNoAgent"><option value="0">Keep the display on</option><option value="1">Turn display off after 15 minutes</option></select>
    <label>Automatic screen rotation</label>
    <select id="autoRotate"><option value="0">Off — change screens by tapping</option><option value="1">On</option></select>
    <label>Time per screen</label>
    <select id="rotateMinutes"><option value="1">1 minute</option><option value="2">2 minutes</option><option value="5">5 minutes</option><option value="10">10 minutes</option><option value="15">15 minutes</option><option value="30">30 minutes</option></select>
    <label>Manual brightness <span id="briVal" style="color:var(--txt)"></span></label>
    <input type="range" id="brightness" min="10" max="255" step="5">
    <label>Screen colours</label>
    <div class="row"><div><label>Background</label><input type="color" id="colorBg"></div><div><label>Panels</label><input type="color" id="colorPanel"></div><div><label>Accent</label><input type="color" id="colorAccent"></div></div>
    <div class="row"><div><label>Main text</label><input type="color" id="colorText"></div><div><label>Secondary text</label><input type="color" id="colorDim"></div><div><label>Grid lines</label><input type="color" id="colorLine"></div></div>
    <label>Colour invert (if colours look wrong)</label>
    <select id="invert"><option value="0">Off</option><option value="1">On</option></select>
    <label>Orientation</label>
    <select id="rotation"><option value="1">Landscape</option><option value="3">Landscape (flipped)</option></select>
    <div class="hint" id="brightnessHint">Manual brightness is used when automatic brightness is off.</div>
    <div class="hint">Automatic rotation skips System. Tap to advance manually, including to System.</div>
  </section>

  <section class="card">
    <h2>PC agent</h2>
    <div class="hint">Run the agent on your PC and point it here:</div>
    <label>Stats endpoint</label>
    <code id="agentUrl">—</code>
    <div class="hint">From the project folder:<br><code>python agent\agent.py</code><br>It auto-discovers the CYD on your LAN. Use <code>--host</code> to set it manually.</div>
    <label>Agent status</label>
    <div class="msg" id="agentMsg">—</div>
  </section>

  <section class="card">
    <h2>Maintenance</h2>
    <div class="actions">
      <button class="ghost" id="rebootBtn">Reboot</button>
      <button class="danger" id="resetBtn">Factory reset</button>
    </div>
    <div class="hint">SD card: <b id="sdStatus">checking…</b></div>
    <div class="actions"><button class="ghost" id="sdFormatBtn">Format / initialise SD card</button></div>
    <div class="msg" id="sdMsg"></div>
    <div class="hint">Factory reset erases WiFi and all settings, then restarts the setup hotspot.</div>
  </section>
</main>
<div class="toast" id="toast"></div>
<script>
const $=s=>document.querySelector(s);
let cfg={};
let locations=[];
function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('show');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('show'),2200);}
function post(url,data){return fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data||{})}).then(r=>r.json());}
function fmtUp(s){s=Math.floor(s);const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?`${d}d ${h}h`:(h?`${h}h ${m}m`:`${m}m`);}
function updateBrightnessControls(){
  const auto=$('#autoBrightness').value==='1';
  $('#brightness').disabled=auto;
  $('#brightnessHint').textContent=auto?'The onboard light sensor is controlling the backlight.':'Manual brightness is used when automatic brightness is off.';
}
async function loadCfg(){
  cfg=await fetch('/api/config').then(r=>r.json());
  $('#city').value=cfg.city||'';$('#lat').value=cfg.lat;$('#lon').value=cfg.lon;
  $('#locationSearch').value=cfg.city||'';
  $('#units').value=cfg.units||'metric';$('#tz').value=cfg.tz||'';$('#feed').value=cfg.feed||'';
  $('#tideStation').value=cfg.tideStation||'E73424';
  $('#brightness').value=cfg.brightness||255;$('#briVal').textContent=cfg.brightness||255;
  for(const key of ['colorBg','colorPanel','colorAccent','colorText','colorDim','colorLine']) $('#'+key).value='#'+Number(cfg[key]||0).toString(16).padStart(6,'0');
  $('#autoBrightness').value=cfg.autoBrightness?'1':'0';updateBrightnessControls();
  $('#sleepNoAgent').value=cfg.sleepNoAgent?'1':'0';
  $('#autoRotate').value=cfg.autoRotate?'1':'0';$('#rotateMinutes').value=String(cfg.rotateMinutes||5);
  $('#invert').value=cfg.invert?'1':'0';$('#rotation').value=String(cfg.rotation||1);
  $('#curSsid').textContent=cfg.ssid||'(not set)';
  $('#agentUrl').textContent=`http://${cfg.ip}/api/stats`;
}
async function loadStatus(){
  try{
    const s=await fetch('/api/status').then(r=>r.json());
    const dot=s.agent?'var(--ok)':'var(--bad)';
    $('#chips').innerHTML=
      `<span class="chip">IP <b>${s.ip}</b></span>`+
      `<span class="chip">${s.mode==='ap'?'Setup hotspot':'WiFi'} <b>${s.mode==='ap'?'AP':s.ssid}</b></span>`+
      `<span class="chip">RSSI <b>${s.mode==='ap'?'—':s.rssi+' dBm'}</b></span>`+
      `<span class="chip">Heap <b>${(s.heap/1024).toFixed(0)} KB</b></span>`+
      `<span class="chip">Light <b>${s.light}</b> · Backlight <b>${s.brightness}</b></span>`+
      (s.sleeping?`<span class="chip"><span style="color:var(--warn)">●</span> Display <b>sleeping</b></span>`:'')+
      `<span class="chip">Uptime <b>${fmtUp(s.uptime)}</b></span>`+
      `<span class="chip"><span style="color:${dot}">●</span> Agent <b>${s.agent?s.agentAge+'s ago':'offline'}</b></span>`;
    $('#agentMsg').innerHTML=s.agent
      ? `<span style="color:var(--ok)">● connected</span> — last packet ${s.agentAge}s ago`
      : `<span style="color:var(--bad)">● offline</span> — start agent\\agent.py on your PC`;
  }catch(e){$('#chips').innerHTML='<span class="chip">device unreachable</span>';}
}
function locationName(r){
  return [r.name,r.admin1&&r.admin1!==r.name?r.admin1:'',r.country].filter(Boolean).join(', ');
}
function chooseLocation(index){
  const r=locations[index];if(!r)return;
  $('#city').value=r.name||'';
  $('#lat').value=Number(r.latitude).toFixed(4);
  $('#lon').value=Number(r.longitude).toFixed(4);
  $('#locationMsg').className='msg ok';
  $('#locationMsg').textContent=`Selected ${locationName(r)} — save settings to apply it.`;
}
async function searchLocations(){
  const query=$('#locationSearch').value.trim();
  if(query.length<2){$('#locationMsg').className='msg bad';$('#locationMsg').textContent='Enter at least two characters';return;}
  const b=$('#locationBtn');b.disabled=true;b.textContent='…';
  $('#locationMsg').className='msg';$('#locationMsg').textContent='Searching…';
  try{
    const url='https://geocoding-api.open-meteo.com/v1/search?name='+encodeURIComponent(query)+'&count=10&language=en&format=json';
    const data=await fetch(url).then(r=>{if(!r.ok)throw new Error('search failed');return r.json();});
    locations=data.results||[];
    const sel=$('#locationResults');sel.innerHTML='';
    if(!locations.length){
      sel.hidden=true;$('#locationResultsLabel').hidden=true;
      $('#locationMsg').className='msg bad';$('#locationMsg').textContent='No matching locations found';
    }else{
      locations.forEach((r,i)=>{const o=document.createElement('option');o.value=String(i);o.textContent=locationName(r);sel.appendChild(o);});
      sel.hidden=false;$('#locationResultsLabel').hidden=false;
      chooseLocation(0);
    }
  }catch(e){
    $('#locationMsg').className='msg bad';
    $('#locationMsg').textContent='Location search needs internet access. You can still enter coordinates manually.';
  }
  b.disabled=false;b.textContent='Search';
}
$('#locationBtn').onclick=searchLocations;
$('#locationSearch').onkeydown=e=>{if(e.key==='Enter'){e.preventDefault();searchLocations();}};
$('#locationResults').onchange=e=>chooseLocation(+e.target.value);
$('#scanBtn').onclick=async()=>{
  const b=$('#scanBtn');b.disabled=true;b.textContent='…';
  try{
    const nets=await fetch('/api/scan').then(r=>r.json());
    const sel=$('#ssidList');sel.innerHTML='';
    if(!nets.length)sel.innerHTML='<option value="">no networks found</option>';
    nets.forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=`${n.ssid} (${n.rssi} dBm)${n.enc?' 🔒':''}`;sel.appendChild(o);});
    if(cfg.ssid)[...sel.options].some(o=>o.value===cfg.ssid)&&(sel.value=cfg.ssid);
  }catch(e){toast('Scan failed');}
  b.disabled=false;b.textContent='Scan';
};
$('#wifiBtn').onclick=async()=>{
  const ssid=$('#ssidList').value||$('#ssidList').options[0]?.value;
  if(!ssid){$('#wifiMsg').className='msg bad';$('#wifiMsg').textContent='Pick a network first';return;}
  $('#wifiMsg').className='msg';$('#wifiMsg').textContent='Saving…';
  try{
    await post('/api/wifi',{ssid,pass:$('#pass').value});
    $('#wifiMsg').className='msg ok';
    $('#wifiMsg').textContent='Saved. Rebooting — reconnect to your WiFi and open the new IP.';
  }catch(e){$('#wifiMsg').className='msg bad';$('#wifiMsg').textContent='Failed to save';}
};
$('#brightness').oninput=e=>{$('#briVal').textContent=e.target.value;post('/api/config',{brightness:+e.target.value});};
for(const key of ['colorBg','colorPanel','colorAccent','colorText','colorDim','colorLine']) $('#'+key).onchange=()=>{const body={};for(const name of ['colorBg','colorPanel','colorAccent','colorText','colorDim','colorLine'])body[name]=parseInt($('#'+name).value.slice(1),16);post('/api/config',body).then(()=>toast('Screen colours updated'));};
$('#autoBrightness').onchange=e=>{updateBrightnessControls();post('/api/config',{autoBrightness:e.target.value==='1'}).then(()=>toast(e.target.value==='1'?'Automatic brightness on':'Automatic brightness off'));};
$('#sleepNoAgent').onchange=e=>post('/api/config',{sleepNoAgent:e.target.value==='1'}).then(()=>toast(e.target.value==='1'?'Agent timeout enabled':'Agent timeout disabled'));
$('#autoRotate').onchange=e=>post('/api/config',{autoRotate:e.target.value==='1'}).then(()=>toast(e.target.value==='1'?'Automatic rotation on':'Automatic rotation off'));
$('#rotateMinutes').onchange=e=>post('/api/config',{rotateMinutes:+e.target.value}).then(()=>toast('Screen interval saved'));
$('#invert').onchange=e=>post('/api/config',{invert:e.target.value==='1'}).then(()=>toast('Display updated'));
$('#rotation').onchange=e=>post('/api/config',{rotation:+e.target.value}).then(()=>toast('Display updated'));
$('#saveBtn').onclick=async()=>{
  const tide=$('#tideStation');
  const body={city:$('#city').value,lat:+$('#lat').value,lon:+$('#lon').value,units:$('#units').value,tz:$('#tz').value,feed:$('#feed').value,tideStation:tide.value,tidePort:tide.options[tide.selectedIndex].text};
  try{await post('/api/config',body);$('#saveMsg').className='msg ok';$('#saveMsg').textContent='Saved';toast('Settings saved');loadCfg();}
  catch(e){$('#saveMsg').className='msg bad';$('#saveMsg').textContent='Save failed';}
};
$('#rebootBtn').onclick=()=>{if(confirm('Reboot the CYD?'))post('/api/reboot').then(()=>toast('Rebooting…'));};
$('#resetBtn').onclick=()=>{if(confirm('Erase all settings and start the setup hotspot?'))post('/api/reset').then(()=>toast('Resetting…'));};
async function loadSd(){
  try{
    const s=await fetch('/api/sd').then(r=>r.json());
    $('#sdStatus').textContent=s.ready?`${s.usedMb} / ${s.totalMb} MB used`:'not detected';
    $('#sdStatus').style.color=s.ready?'var(--ok)':'var(--bad)';
  }catch(e){$('#sdStatus').textContent='unavailable';$('#sdStatus').style.color='var(--bad)';}
}
$('#sdFormatBtn').onclick=async()=>{
  if(!confirm('Initialise the SD card if it is unformatted? Existing files may be lost if the filesystem must be created.'))return;
  $('#sdMsg').className='msg';$('#sdMsg').textContent='Initialising…';
  try{
    const r=await post('/api/sd',{format:true});
    $('#sdMsg').className=r.ok?'msg ok':'msg bad';
    $('#sdMsg').textContent=r.ok?'SD card ready':'SD card could not be mounted';
    loadSd();
  }catch(e){$('#sdMsg').className='msg bad';$('#sdMsg').textContent='SD card action failed';}
};
loadSd();setInterval(loadSd,5000);
loadCfg();loadStatus();setInterval(loadStatus,3000);
</script>
</body>
</html>
)====";
