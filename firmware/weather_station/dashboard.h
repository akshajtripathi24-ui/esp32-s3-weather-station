// Live dashboard served at "/". Self-contained (no CDN), so it works without internet.
// Data arrives over the WebSocket at /ws; add ?demo to the URL to run it on sample data.
#pragma once

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Weather Station</title>
<style>
:root{--bg:#0b1120;--card:#111a2e;--sunk:#0e1627;--text:#e8eef8;--muted:#9aa8bd;--faint:#6b7a92;--border:#1f2b44;--accent:#38bdf8;
--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;--bad-soft:rgba(239,68,68,.12);
--c1:#38bdf8;--c2:#fb923c;--c3:#a78bfa;--c4:#4ade80;--c5:#facc15;--c6:#f472b6;--c7:#2dd4bf;color-scheme:dark}
@media (prefers-color-scheme:light){:root{--bg:#f6f8fb;--card:#fff;--sunk:#eef2f7;--text:#0f172a;--muted:#526075;--faint:#8391a7;--border:#dde4ee;--accent:#0284c7;
--bad-soft:#fef2f2;--c1:#0ea5e9;--c2:#ea580c;--c3:#7c3aed;--c4:#16a34a;--c5:#ca8a04;--c6:#db2777;--c7:#0d9488;color-scheme:light}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 ui-sans-serif,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:1100px;margin:0 auto;padding:20px 16px 32px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;margin-bottom:18px}
h1{font-size:22px;margin:0;letter-spacing:-.02em}
.sub{color:var(--faint);font-size:13px}
.tools{display:flex;gap:8px;align-items:center}
.pill{display:inline-flex;align-items:center;gap:8px;padding:6px 12px;border-radius:999px;border:1px solid var(--border);background:var(--card);font-size:13px;font-weight:600;font-variant-numeric:tabular-nums;white-space:nowrap}
.dot{width:9px;height:9px;border-radius:50%;background:var(--warn)}
.pill.live .dot{background:var(--ok);animation:ping 2s infinite}
.pill.offline .dot{background:var(--bad)}
@keyframes ping{0%{box-shadow:0 0 0 0 rgba(34,197,94,.55)}80%,100%{box-shadow:0 0 0 7px rgba(34,197,94,0)}}
button{font:inherit;color:var(--text);background:var(--card);border:1px solid var(--border);border-radius:10px;padding:6px 11px;cursor:pointer}
button:hover{border-color:var(--accent)}
button[aria-pressed=true]{border-color:var(--accent);color:var(--accent)}
.banner{display:none;align-items:center;gap:10px;background:var(--bad);color:#fff;font-weight:600;padding:10px 14px;border-radius:12px;margin-bottom:14px;animation:pulse 1.2s infinite}
.banner.show{display:flex}
@keyframes pulse{50%{opacity:.75}}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(165px,1fr));gap:12px}
.card{--c:var(--accent);position:relative;overflow:hidden;background:var(--card);border:1px solid var(--border);border-radius:14px;padding:12px 14px 8px;cursor:pointer;transition:border-color .2s,background .2s}
.card::before{content:"";position:absolute;left:0;top:0;bottom:0;width:4px;background:var(--c)}
.card:hover,.card.sel{border-color:var(--c)}
.card.alert{--c:var(--bad);background:var(--bad-soft)}
.lbl{display:flex;justify-content:space-between;font-size:11.5px;font-weight:600;letter-spacing:.06em;text-transform:uppercase;color:var(--faint)}
.lbl em{font-style:normal;color:var(--bad);display:none}.card.alert .lbl em{display:inline}
.val{font-size:25px;font-weight:700;letter-spacing:-.02em;font-variant-numeric:tabular-nums;white-space:nowrap;margin-top:2px}
.val small{font-size:13px;font-weight:500;color:var(--muted);margin-left:3px}
.note{font-size:12.5px;color:var(--faint);height:18px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.spark{display:block;width:100%;height:32px;margin-top:4px}
.panel{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:14px 16px;margin-top:12px}
.panel-head{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:8px}
.panel-head b{font-size:15px}
.chart{position:relative;height:200px}
.chart svg{width:100%;height:100%;display:block}
.axis{position:absolute;left:0;font-size:11.5px;color:var(--faint);font-variant-numeric:tabular-nums;background:var(--card);padding:0 4px;border-radius:4px}
.axis.top{top:0}.axis.bot{bottom:18px}
.xlab{display:flex;justify-content:space-between;font-size:11.5px;color:var(--faint)}
footer{display:flex;flex-wrap:wrap;gap:6px 18px;margin-top:14px;font-size:12.5px;color:var(--faint);font-variant-numeric:tabular-nums}
footer a{color:var(--accent)}
@media (max-width:420px){.grid{grid-template-columns:1fr 1fr}.val{font-size:21px}}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div><h1>Live IoT Weather Station</h1><div class="sub" id="host"></div></div>
    <div class="tools">
      <span class="pill" id="conn"><span class="dot"></span><span id="connText">Connecting&hellip;</span></span>
      <button id="sound" aria-pressed="false" title="Beep on alerts">&#128264; Sound</button>
    </div>
  </header>
  <div class="banner" id="banner" role="status"></div>
  <div class="grid" id="grid"></div>
  <div class="panel">
    <div class="panel-head"><b id="chartTitle"></b><span class="sub">last 5 minutes &middot; tap a card to switch</span></div>
    <div class="chart">
      <svg id="chart" viewBox="0 0 600 200" preserveAspectRatio="none">
        <path id="area" fill-opacity=".14"/><path id="line" fill="none" stroke-width="2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>
      </svg>
      <span class="axis top" id="yMax"></span><span class="axis bot" id="yMin"></span>
    </div>
    <div class="xlab"><span>-5 min</span><span>now</span></div>
  </div>
  <footer>
    <span id="uptime">Uptime --</span><span id="rssi">Wi-Fi --</span><span id="clients">Viewers --</span><span id="age">No data yet</span><a href="/data">Raw JSON</a>
  </footer>
</div>
<script>
(function(){
var DEMO=/[?&]demo/.test(location.search), HIST=300;
function aqi(v){return v==null?'':v<=9?'AQI: Good':v<=35.4?'AQI: Moderate':v<=55.4?'AQI: Sensitive groups':v<=125.4?'AQI: Unhealthy':v<=225.4?'AQI: Very unhealthy':'AQI: Hazardous';}
var M=[
 {k:'tempC',l:'Temperature',u:'°C',d:1,c:'--c1',n:function(x){return x.tempF==null?'':x.tempF.toFixed(1)+' °F'},bad:function(v){return v>45||v<0}},
 {k:'humidity',l:'Humidity',u:'%',d:1,c:'--c2',n:function(){return 'relative'},bad:function(v){return v>85||v<20}},
 {k:'pressure',l:'Pressure',u:'hPa',d:1,c:'--c3',n:function(){return 'barometric'},bad:function(v){return v<550||v>1400}},
 {k:'ppm',l:'Air quality',u:'ppm',d:0,c:'--c4',n:function(x){return x.ratio==null?'CO₂-equivalent':'CO₂-eq · Rs/Ro '+x.ratio.toFixed(2)},bad:function(v){return v>1600}},
 {k:'pm1',l:'PM1.0',u:'µg/m³',d:0,c:'--c5',n:function(){return 'ultrafine'},bad:function(){return false}},
 {k:'pm25',l:'PM2.5',u:'µg/m³',d:0,c:'--c6',n:function(x){return aqi(x.pm25)},bad:function(v){return v>65}},
 {k:'pm10',l:'PM10',u:'µg/m³',d:0,c:'--c7',n:function(){return 'coarse dust'},bad:function(v){return v>150}}
];
var hist={},sel=0,grid=document.getElementById('grid');
M.forEach(function(m,i){
  hist[m.k]=[];
  var el=document.createElement('div');el.className='card'+(i===0?' sel':'');el.style.setProperty('--c','var('+m.c+')');
  el.innerHTML='<div class="lbl">'+m.l+'<em>alert</em></div><div class="val">--</div><div class="note"></div>'+
    '<svg class="spark" viewBox="0 0 100 32" preserveAspectRatio="none"><path fill="none" stroke="var('+m.c+')" stroke-width="1.6" vector-effect="non-scaling-stroke"/></svg>';
  el.onclick=function(){sel=i;document.querySelectorAll('.card').forEach(function(c,j){c.classList.toggle('sel',j===i)});drawChart();};
  grid.appendChild(el);m.el=el;
});
document.getElementById('host').textContent=DEMO?'Demo mode · sample data':location.host;

function path(arr,w,h,pad){
  var pts=arr.filter(function(v){return v!=null});
  if(pts.length<2)return null;
  var lo=Math.min.apply(null,pts),hi=Math.max.apply(null,pts);if(hi-lo<1e-6){hi+=1;lo-=1;}
  var n=arr.length,d='',started=false;
  arr.forEach(function(v,i){if(v==null){started=false;return;}
    var x=(i/(n-1))*w,y=pad+(1-(v-lo)/(hi-lo))*(h-2*pad);d+=(started?'L':'M')+x.toFixed(1)+' '+y.toFixed(1);started=true;});
  return {d:d,lo:lo,hi:hi};
}
function drawChart(){
  var m=M[sel],p=path(hist[m.k],600,200,10),line=document.getElementById('line'),area=document.getElementById('area');
  document.getElementById('chartTitle').textContent=m.l+' ('+m.u+')';
  line.setAttribute('stroke','var('+m.c+')');area.setAttribute('fill','var('+m.c+')');
  if(!p){line.setAttribute('d','');area.setAttribute('d','');document.getElementById('yMax').textContent='';document.getElementById('yMin').textContent='';return;}
  line.setAttribute('d',p.d);
  var simple=p.d.indexOf('M',1)<0;   // only fill under an unbroken line
  area.setAttribute('d',simple?p.d+'L600 200L'+p.d.slice(1,p.d.indexOf(' '))+' 200Z':'');
  document.getElementById('yMax').textContent=p.hi.toFixed(m.d);
  document.getElementById('yMin').textContent=p.lo.toFixed(m.d);
}

var audio=null,soundOn=false,lastAlertKey='';
var soundBtn=document.getElementById('sound');
soundBtn.onclick=function(){soundOn=!soundOn;soundBtn.setAttribute('aria-pressed',soundOn);if(soundOn&&!audio){audio=new(window.AudioContext||window.webkitAudioContext)();}};
function beep(){if(!soundOn||!audio)return;var o=audio.createOscillator(),g=audio.createGain();o.frequency.value=880;g.gain.value=.1;o.connect(g);g.connect(audio.destination);o.start();o.stop(audio.currentTime+.3);}

var lastData=0;
function fmtUptime(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+'d ':'')+(d||h?h+'h ':'')+m+'m';}
function update(x){
  lastData=Date.now();
  var alerts=[];
  M.forEach(function(m){
    var v=x[m.k];v=(typeof v==='number')?v:null;
    hist[m.k].push(v);if(hist[m.k].length>HIST)hist[m.k].shift();
    m.el.querySelector('.val').innerHTML=v==null?'--':v.toFixed(m.d)+'<small>'+m.u+'</small>';
    m.el.querySelector('.note').textContent=v==null?'no reading':m.n(x);
    var bad=v!=null&&m.bad(v);m.el.classList.toggle('alert',bad);if(bad)alerts.push(m.l);
    var p=path(hist[m.k].slice(-60),100,32,3);m.el.querySelector('.spark path').setAttribute('d',p?p.d:'');
  });
  var b=document.getElementById('banner');
  if(alerts.length){b.textContent='⚠ Abnormal readings: '+alerts.join(', ');b.classList.add('show');}else b.classList.remove('show');
  var key=alerts.join();if(key&&key!==lastAlertKey)beep();lastAlertKey=key;
  if(x.uptime!=null)document.getElementById('uptime').textContent='Uptime '+fmtUptime(x.uptime);
  if(x.rssi!=null)document.getElementById('rssi').textContent='Wi-Fi '+(x.rssi?x.rssi+' dBm':'--');
  if(x.clients!=null)document.getElementById('clients').textContent='Viewers '+x.clients;
  drawChart();
}

// ---------- connection with ping + exponential-backoff reconnect ----------
var ws=null,state='connecting',retry=0,retryAt=0,rtt=null,lastMsg=0;
function connect(){
  state='connecting';
  ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');
  ws.onopen=function(){retry=0;state='live';lastMsg=Date.now();ping();};
  ws.onmessage=function(e){lastMsg=Date.now();
    if(e.data.slice(0,5)==='pong:'){rtt=Date.now()-Number(e.data.slice(5));return;}
    try{update(JSON.parse(e.data));}catch(err){}};
  ws.onclose=function(){var delay=Math.min(10000,1000*Math.pow(2,retry++));state='reconnecting';retryAt=Date.now()+delay;rtt=null;setTimeout(connect,delay);};
  ws.onerror=function(){ws.close();};
}
function ping(){if(ws&&ws.readyState===1)ws.send('ping:'+Date.now());}
function renderStatus(){
  var pill=document.getElementById('conn'),t=document.getElementById('connText');
  if(DEMO){state='live';}
  pill.className='pill'+(state==='live'?' live':retry>=3?' offline':'');
  if(state==='live')t.textContent='Live'+(rtt!=null?' · '+rtt+' ms':'');
  else if(state==='reconnecting'){var s=Math.max(0,Math.ceil((retryAt-Date.now())/1000));t.textContent=(retry>=3?'Offline':'Reconnecting')+' · retry in '+s+'s';}
  else t.textContent='Connecting…';
  document.getElementById('age').textContent=lastData?'Updated '+Math.round((Date.now()-lastData)/1000)+'s ago':'No data yet';
}
setInterval(function(){
  if(!DEMO&&state==='live'&&Date.now()-lastMsg>6000&&ws)ws.close();   // stream stalled: force a reconnect
  renderStatus();
},500);

drawChart();
if(DEMO){
  var s={tempC:27.4,humidity:58,pressure:1008.6,ratio:1.1,pm1:11,pm25:18,pm10:27},t0=Date.now();
  var dr=function(v,tg,st){return v+(tg-v)*.08+(Math.random()-.5)*st;};
  var tick=function(){s.tempC=dr(s.tempC,27.2,.3);s.humidity=dr(s.humidity,57,1);s.pressure=dr(s.pressure,1008.8,.2);s.ratio=Math.max(.2,dr(s.ratio,1.1,.03));
    s.pm1=Math.max(0,dr(s.pm1,11,1.5));s.pm25=Math.max(0,dr(s.pm25,17,2));s.pm10=Math.max(0,dr(s.pm10,26,3));
    update({tempC:s.tempC,tempF:s.tempC*1.8+32,humidity:s.humidity,pressure:s.pressure,ratio:s.ratio,ppm:Math.round(420*Math.pow(s.ratio,-2.769)),
      pm1:Math.round(s.pm1),pm25:Math.round(s.pm25),pm10:Math.round(s.pm10),uptime:Math.round((Date.now()-t0)/1000)+3600,rssi:-58,clients:1});
    rtt=18+Math.round(Math.random()*12);};
  for(var i=0;i<120;i++)tick();
  setInterval(tick,1000);
}else connect();
setInterval(ping,5000);
})();
</script>
</body>
</html>
)rawliteral";
