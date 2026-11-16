#pragma once

const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html><html lang="he" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>בקר השקיה</title>
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="השקיה"><meta name="theme-color" content="#1a7f37">
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#eef2ee;color:#1b2b1e}
.w{max-width:520px;margin:auto;padding:12px}
.c{background:#fff;border-radius:12px;padding:14px;margin-bottom:12px;box-shadow:0 1px 3px #0002}
h2{font-size:16px;margin:0 0 10px}
.big{font-size:30px;font-weight:700}.on{color:#1a7f37}.off{color:#777}
button{font-size:17px;padding:12px 18px;border:0;border-radius:10px;background:#1a7f37;color:#fff;margin:4px 2px}
.stop{background:#c62828}.g{background:#556;font-size:14px;padding:8px 12px}
.bigstop{width:100%;font-size:22px;padding:18px;margin-top:10px}
input{font-size:16px;padding:8px;width:64px}
.ev{padding:6px 0;border-bottom:1px solid #eee;font-size:14px}.ev small{color:#777;display:block}
.l1{color:#b26a00}.l2{color:#c62828;font-weight:600}
.alert{background:#fdecea;color:#c62828;border-radius:8px;padding:10px;margin-bottom:8px;font-weight:600}
.m{color:#555;font-size:14px;line-height:1.8}
</style></head><body><div class="w">

<div class="c"><div id="st" class="big">...</div><div id="sub" class="m"></div>
<div id="ctlOn"><input id="min" type="number" value="1" min="0.5" max="10" step="0.5"> דקות
<button onclick="pump('on')">הדלק</button></div>
<button id="ctlOff" class="stop bigstop" onclick="pump('off')" style="display:none">כבה עכשיו</button></div>

<div class="c"><h2>השקיה יומית</h2><div id="alert"></div><div id="sch" class="m"></div>
<button class="g" id="skipBtn" onclick="skip()">דלג על ההשקיה הבאה</button></div>

<div class="c"><h2>מצב מערכת</h2><div id="info" class="m"></div></div>

<div class="c"><h2>היסטוריה</h2>
<label class="m"><input type="checkbox" id="onlyErr" style="width:auto" onchange="render()"> רק שגיאות ואזהרות</label>
<div id="ev"></div>
<button class="g" id="more" onclick="loadMore()">טען עוד</button>
<button class="g" onclick="clearLog()">נקה יומן</button></div>

</div><script>
const $=i=>document.getElementById(i),SRC=['','טלפון','כפתור','תזמון','טלגרם'];
const SR=['','בוצעה ✓','דולגה לבקשתך','לא בוצעה – לא היה חיבור לרשת!','לא בוצעה – הבקר היה כבוי או בלי שעה!','לא בוצעה – המשאבה כבר פעלה','בוצעה חלקית – הופסקה באמצע!','בתהליך…'];
let evs=[],total=0,lastHead=-1,busy=false,skipOn=false,timeChecked=false;
function checkTime(s){if(timeChecked)return;timeChecked=true;const now=Math.floor(Date.now()/1000);
if(!s.timeValid||Math.abs(now-s.epoch)>30)fetch('/api/time',{method:'POST',body:new URLSearchParams({epoch:now})});}
function fmt(t){t=Math.max(0,t|0);const h=t/3600|0,m=(t%3600)/60|0,s=t%60;return(h?h+':':'')+String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');}
async function post(u,p){try{const r=await fetch(u,{method:'POST',body:new URLSearchParams(p)});if(!r.ok)alert(await r.text());}catch(e){alert('אין חיבור לבקר');}status();}
async function pump(a){if(busy)return;busy=true;await post('/api/pump',{action:a,sec:Math.round($('min').value*60)});busy=false;}
function skip(){post('/api/skip',{on:skipOn?0:1});}
function clearLog(){if(confirm('למחוק את כל ההיסטוריה?'))post('/api/clear',{});}
async function status(){try{const s=await(await fetch('/api/status')).json();
checkTime(s);
$('st').textContent=s.pump?'משאבה פועלת':'משאבה כבויה';$('st').className='big '+(s.pump?'on':'off');
$('sub').textContent=s.pump?`מקור: ${SRC[s.src]} · נותרו ${fmt(s.remaining)}`:'';
$('ctlOn').style.display=s.pump?'none':'';$('ctlOff').style.display=s.pump?'':'none';
const c=s.sched;skipOn=c.skip;
let line=!s.timeValid?'השעה לא מסונכרנת – התזמון מושהה':(c.handled?`היום: ${c.res?SR[c.res]:'—'} · הבאה: מחר ${c.time}`:`הבאה: היום ${c.time}`);
$('sch').innerHTML=`כל יום ב-<b>${c.time}</b> למשך ${fmt(c.dur)}<br>${line}${c.skip?'<br><b class="l1">ההשקיה הבאה תדולג</b>':''}`;
$('alert').innerHTML=(c.handled&&(c.res==3||c.res==4||c.res==6))?`<div class="alert">⚠ השקיית ${c.time} ${SR[c.res]}</div>`:'';
$('skipBtn').textContent=c.skip?'בטל דילוג':'דלג על ההשקיה הבאה';
$('info').innerHTML=`שעה: <b>${s.time}</b><br>רשת: ${s.ssid} · ${s.ip} · ${s.rssi} dBm<br>פועל ברציפות: ${fmt(s.uptime)} · אתחול מס' ${s.boot}<br>אירועים שמורים: ${s.logCount} / ${s.logCap}`;
if(s.logHead!==lastHead){lastHead=s.logHead;reloadEvents();}
}catch(e){$('st').textContent='אין חיבור לבקר';$('st').className='big l2';}}
async function reloadEvents(){const r=await(await fetch('/api/events?offset=0&limit='+Math.min(100,Math.max(50,evs.length)))).json();evs=r.events;total=r.total;render();}
async function loadMore(){const r=await(await fetch('/api/events?offset='+evs.length+'&limit=50')).json();evs=evs.concat(r.events);total=r.total;render();}
function render(){const only=$('onlyErr').checked,E=$('ev');E.innerHTML='';
evs.filter(e=>!only||e.l>0).forEach(e=>{const d=document.createElement('div');d.className='ev l'+e.l;
const sm=document.createElement('small');sm.textContent=e.t;d.appendChild(sm);d.appendChild(document.createTextNode(e.x));E.appendChild(d);});
$('more').style.display=evs.length<total?'':'none';}
status();setInterval(status,2000);
</script></body></html>
)rawliteral";
