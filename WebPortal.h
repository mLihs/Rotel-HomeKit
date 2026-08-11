/*
 * WebPortal.h – WLAN-Onboarding, Geräte-Dashboard und GitHub-OTA
 *
 * Architektur:
 *   - WiFiManagerLite besitzt das WLAN (Verbindung, Captive Portal, NVS).
 *     HomeSpan erkennt die Verbindung passiv über die Arduino-Netzwerk-Events
 *     und startet dann den HAP-Server (Port 8080, siehe setPortNum im Sketch).
 *   - Ein gemeinsamer AsyncWebServer auf Port 80 traegt das WML-Portal
 *     (Pfade unter "/wml/") und das eigene Dashboard ("/" und "/api/").
 *   - GitFirmwareUpdate (HTTP-only) laedt Firmware von einem eigenen Server.
 *
 * Nebenlaeufigkeit (ESP32-C3 = Single-Core):
 *   Die AsyncWebServer-Handler laufen im async_tcp-Task und duerfen nicht
 *   blockieren. Sie setzen nur volatile Flags; die eigentliche Arbeit
 *   (Update-Check, Download/Flash, SRP-Berechnung, Factory-Reset) passiert
 *   im Haupt-Loop (webPortalLoop). Waehrend Download/Flash steht der
 *   HomeSpan-Loop still – akzeptabel, danach rebootet das Geraet.
 *
 * Speicher: alle Puffer statisch/fix, keine wiederkehrenden Allokationen.
 */

#pragma once

#include <ESPAsyncWebServer.h>
#include <wifiMangerLite.h>
#include <GitFirmwareUpdate.h>   // HTTP-only (Default); HTTPS via build_opt.h
#include <Preferences.h>

// ---------- Konfiguration ----------

// TODO: eigene Update-URL eintragen (latest.json mit {"version","url"})
constexpr const char * FW_LATEST_JSON_URL = "http://example.com/rotel-firmware/latest.json";

constexpr uint32_t RESET_DELAY_MS      = 750;    // Antwort an Browser rausschieben, dann Reset
constexpr size_t   PAIRCODE_LEN        = 8;      // HomeKit-Setup-Code: exakt 8 Ziffern
constexpr const char * PAIRCODE_DEFAULT = "46637726";  // HomeSpan-Default (466-37-726)
constexpr size_t   HOSTNAME_MAX        = 24;     // mDNS-Label: konservative Obergrenze
constexpr const char * HOSTNAME_DEFAULT = "Rotel-Remote";  // -> rotel-remote.local

// ---------- Globale Objekte (einmalige Allokation beim Boot) ----------

AsyncWebServer          webServer(80);
WML::WiFiManagerLite    wifiMgr;
WML::Storage            wmlStorage("wificfg", "wifi");
WML::StorageProvider    wmlConfigProvider(wmlStorage);
WML::CaptivePortal      wmlPortal(webServer, wifiMgr);
GitFirmwareUpdate       fwUpdate(wymrfirmware, FW_LATEST_JSON_URL);
Preferences             devicePrefs;   // Namespace "rotelcfg": Pairing-Code-Klartext

// ---------- Zustand (async_tcp-Task <-> Haupt-Loop) ----------

volatile bool     wpCheckRequested   = false;
volatile bool     wpUpdateRequested  = false;
volatile bool     wpResetRequested   = false;
volatile bool     wpPairRequested    = false;
volatile bool     wpModelRequested   = false;
volatile uint8_t  wpPendingModel     = 0;
volatile bool     wpHostRequested    = false;
volatile bool     wpUpdateInProgress = false;
volatile int      wpUpdateProgress   = 0;

volatile bool wpUpdateAvailable = false;   // Schreiber: Haupt-Loop, Leser: async_tcp
uint32_t wpResetRequestAt  = 0;
uint32_t wpModelRequestAt  = 0;
uint32_t wpHostRequestAt   = 0;
char     wpPendingHost[HOSTNAME_MAX + 1]     = "";
char     wpHostName[HOSTNAME_MAX + 1]        = "Rotel-Remote";   // aktiver mDNS-/AP-Basisname
char     wpPendingPairCode[PAIRCODE_LEN + 1] = "";
char     wpPairCode[PAIRCODE_LEN + 1]        = "";   // Klartext-Kopie fuer Anzeige
char     wpRemoteVersion[16]                 = "";
char     wpLastError[64]                     = "";
char     wpPairMessage[48]                   = "";

// ---------- Hilfsfunktionen ----------

// Gleiche Trivial-Code-Liste wie HomeSpans Network_HS::allowedCode();
// muss repliziert werden, damit die Klartext-Kopie nie von der
// SRP-Realitaet abweicht (HomeSpan lehnt diese Codes still ab)
static bool pairCodeAllowed(const char *s){
  static const char * const banned[] = {
    "00000000","11111111","22222222","33333333","44444444","55555555",
    "66666666","77777777","88888888","99999999","12345678","87654321"
  };
  if (strlen(s) != PAIRCODE_LEN) return false;
  for (size_t i = 0; i < PAIRCODE_LEN; i++)
    if (s[i] < '0' || s[i] > '9') return false;
  for (auto b : banned)
    if (strcmp(s, b) == 0) return false;
  return true;
}

// mDNS-/AP-Basisname pruefen (RFC-1123-Label): 1-24 Zeichen, nur Buchstaben,
// Ziffern und '-', kein Bindestrich am Anfang oder Ende
static bool hostNameAllowed(const char *s){
  const size_t n = strlen(s);
  if (n < 1 || n > HOSTNAME_MAX) return false;
  if (s[0] == '-' || s[n - 1] == '-') return false;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '-') return false;
  return true;
}

// Muss im Sketch VOR homeSpan.begin() laufen: der Name geht als hostNameBase
// an HomeSpan (mDNS, .local-Adresse) und als AP-Basisname an den WLAN-Manager
void wpLoadHostName(){
  devicePrefs.begin("rotelcfg", true);
  devicePrefs.getString("hostname", wpHostName, sizeof(wpHostName));
  devicePrefs.end();
  if (!hostNameAllowed(wpHostName)) strlcpy(wpHostName, HOSTNAME_DEFAULT, sizeof(wpHostName));
}

static void fwProgressCallback(int percent, size_t bytesRead, size_t totalBytes){
  (void)bytesRead; (void)totalBytes;
  wpUpdateProgress = percent;
}

// Kompletter Werksreset: WML-Config + Geraete-Prefs + HomeSpan (Pairing, WLAN).
// processSerialCommand("F") loescht die HomeSpan-NVS-Bereiche und rebootet –
// kehrt also nie zurueck. Laeuft ausschliesslich im Haupt-Loop.
static void doFactoryReset(){
  wmlConfigProvider.clearConfig();
  devicePrefs.begin("rotelcfg", false);
  devicePrefs.clear();
  devicePrefs.end();
  homeSpan.processSerialCommand("F");   // rebootet
}

// ---------- Web-UI (statisches HTML/CSS im Flash) ----------
// Nutzt das Designsystem des WML-Portals: /wml/style.css wird direkt
// eingebunden (Klassen: app, card, btn, ... aus common.css) – dadurch
// identische Optik ohne dupliziertes CSS. Eigene Ergaenzungen (Status-Zeilen,
// Meldungsboxen, Dropdown, Stepper) liegen in APP_CSS und werden von
// Dashboard UND Webremote unter /app.css geteilt.

static const char APP_CSS[] PROGMEM = R"CSS(
.row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid rgba(141,158,185,.25);font-size:var(--font-size-body-medium)}
.row:last-child{border-bottom:none}
.row .k{color:var(--text-muted)}
.row .v{color:var(--text-main);font-weight:var(--font-weight-semibold);text-align:right}
.msg{padding:12px 16px;border-radius:var(--radius-md);margin-top:16px;font-size:var(--font-size-body-medium);display:none}
.msg.ok{background:rgba(37,211,125,.12);color:#00A554;display:block}
.msg.err{background:var(--accent-pink-light);color:var(--accent-pink);display:block}
.msg.info{background:rgba(141,158,185,.18);color:var(--text-label);display:block}
.hint{font-size:var(--font-size-body-small);color:var(--text-muted);margin-top:4px}
.page-footer{color:var(--text-muted)}
/* Eigenes Dropdown im WML-Design: native option-Listen sind nicht stylbar,
   daher Button + eigene Liste (rein statisch, kein zusaetzlicher RAM) */
.dd{position:relative}
.dd-btn{width:100%;padding:var(--spc-lg) 44px var(--spc-lg) var(--spc-xl);background:var(--bg-white);border:1px solid var(--border-soft);border-radius:var(--radius-md);color:var(--text-main);font-size:var(--font-size-body-medium);font-family:var(--font-ui);font-weight:var(--font-weight-semibold);text-align:left;transition:border-color var(--transition-fast),box-shadow var(--transition-fast);appearance:none;-webkit-appearance:none;cursor:pointer;background-image:url("data:image/svg+xml,%3Csvg%20xmlns='http://www.w3.org/2000/svg'%20width='16'%20height='16'%20viewBox='0%200%2016%2016'%3E%3Cpath%20d='M4%206l4%204%204-4'%20fill='none'%20stroke='%238d9eb9'%20stroke-width='2'%20stroke-linecap='round'%20stroke-linejoin='round'/%3E%3C/svg%3E");background-repeat:no-repeat;background-position:right 14px center;background-size:16px}
.dd-btn:focus{outline:none;border-color:var(--accent-pink);box-shadow:0 0 0 3px var(--accent-pink-light)}
.dd.open .dd-btn{border-color:var(--accent-pink)}
.dd-list{display:none;position:absolute;top:calc(100% + 6px);left:0;right:0;background:var(--bg-white);border:1px solid var(--border-soft);border-radius:var(--radius-md);box-shadow:0 12px 28px rgba(31,43,61,.16);max-height:264px;overflow-y:auto;z-index:20;padding:6px}
.dd.open .dd-list{display:block}
.dd-item{padding:11px 14px;border-radius:calc(var(--radius-md) - 4px);font-size:var(--font-size-body-medium);color:var(--text-main);cursor:pointer;display:flex;justify-content:space-between;align-items:center;gap:8px}
.dd-item:hover{background:var(--accent-pink-light)}
.dd-item.sel{color:var(--accent-pink);font-weight:var(--font-weight-semibold)}
.dd-item.sel::after{content:'\2713'}
/* Karte mit offenem Dropdown ueber die Folgekarten heben (jede .card bildet
   durch die fadeIn-Animation einen eigenen Stacking-Context) */
.card.dd-top{position:relative;z-index:30}
)CSS";

static const char DASHBOARD_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>Rotel Remote</title>
<link rel="stylesheet" href="/wml/style.css">
<link rel="stylesheet" href="/app.css">
</head><body>
<div class="app">

<div class="page-header">
  <h1>Rotel Remote</h1>
  <div class="subtitle">Device Dashboard</div>
</div>

<div class="card">
  <div class="card-header"><div class="card-title">Status</div></div>
  <div class="row"><span class="k">Firmware</span><span class="v" id="fw">–</span></div>
  <div class="row"><span class="k">Latest version</span><span class="v" id="remote">–</span></div>
  <div class="row"><span class="k">Model</span><span class="v" id="model">–</span></div>
  <div class="row"><span class="k">Protocol</span><span class="v" id="gen">–</span></div>
  <div class="row"><span class="k">Amplifier</span><span class="v" id="amp">–</span></div>
  <div class="row"><span class="k">HomeKit code</span><span class="v" id="pair">–</span></div>
  <div class="row"><span class="k">Free heap</span><span class="v" id="heap">–</span></div>
  <div class="msg" id="status"></div>
</div>

<div class="button-stack">
  <a href="/remote" class="btn btn-large btn-cta">Open Remote</a>
</div>

<div class="card">
  <div class="card-header"><div class="card-title">Amplifier Model</div></div>
  <label>Select model</label>
  <div class="dd" id="modelDD">
    <button type="button" class="dd-btn" id="modelBtn" onclick="ddToggle(event)"><span id="modelCur">–</span></button>
    <div class="dd-list" id="modelList"></div>
  </div>
  <div class="hint">The device restarts after switching; source renames and visibility settings are reset.</div>
  <div class="button-stack">
    <button class="btn btn-large btn-cta" onclick="doModel()">Apply &amp; Restart</button>
  </div>
</div>

<div class="card">
  <div class="card-header"><div class="card-title">HomeKit Code</div></div>
  <label for="code">New code</label>
  <input type="text" id="code" maxlength="8" inputmode="numeric" placeholder="8 digits" autocomplete="off">
  <div class="hint">Applies to future pairings only.</div>
  <div class="button-stack">
    <button class="btn btn-large btn-cta" id="btnPair" onclick="doPair()" disabled>Set code</button>
  </div>
</div>

<div class="card">
  <div class="card-header"><div class="card-title">Device Address</div></div>
  <label for="host">Name</label>
  <input type="text" id="host" maxlength="24" placeholder="rotel-remote" autocomplete="off">
  <div class="hint" id="hostHint">Letters, digits and hyphens; also used as the setup Wi-Fi name.</div>
  <div class="button-stack">
    <button class="btn btn-large btn-cta" id="btnHost" onclick="doHost()" disabled>Apply &amp; Restart</button>
  </div>
</div>

<div class="card">
  <div class="card-header"><div class="card-title">Firmware Update</div></div>
  <div class="button-stack">
    <button class="btn btn-large btn-secondary" id="btnCheck" onclick="doCheck()">Check for update</button>
    <button class="btn btn-large btn-cta" id="btnUpdate" onclick="doUpdate()" disabled>Install update</button>
  </div>
</div>

<div class="button-stack">
  <a href="/wml/setup" class="btn btn-large btn-secondary">Wi-Fi Settings</a>
  <button class="btn btn-large btn-danger" onclick="doReset()">Factory Reset</button>
</div>

<div class="page-footer">Rotel Remote</div>
</div>
<script>
function S(t,c){var e=document.getElementById('status');e.textContent=t;e.className='msg '+c;}
function refresh(){fetch('/api/info').then(r=>r.json()).then(d=>{
 document.getElementById('fw').textContent=d.fw;
 document.getElementById('remote').textContent=d.remote||'not checked';
 document.getElementById('model').textContent=d.model;
 document.getElementById('gen').textContent=d.gen;
 document.getElementById('amp').textContent=d.power?('On – '+d.source):'Standby';
 document.getElementById('pair').textContent=d.pair.replace(/(\d{3})(\d{2})(\d{3})/,'$1-$2-$3');
 document.getElementById('heap').textContent=d.heap+' B';
 var h=document.getElementById('host');
 if(h.value===''&&document.activeElement!==h){h.placeholder=d.host.toLowerCase();
  document.getElementById('hostHint').textContent='Current: http://'+d.host.toLowerCase()+'.local';}
 document.getElementById('btnUpdate').disabled=!d.updateAvail||d.updating;
 if(d.updating){S('Updating: '+d.progress+' %','info');}
 else if(d.pairMsg){S(d.pairMsg,'ok');}
 else if(d.error){S(d.error,'err');}
}).catch(()=>{});}
function doCheck(){S('Checking for update…','info');fetch('/api/check',{method:'POST'});setTimeout(refresh,4000);}
var updT=0;
function doUpdate(){if(!confirm('Install firmware update? The device will restart afterwards.'))return;
 fetch('/api/update',{method:'POST'});S('Update started…','info');
 if(!updT)updT=setInterval(refresh,2000);}
document.getElementById('code').addEventListener('input',function(){
 document.getElementById('btnPair').disabled=!/^\d{8}$/.test(this.value);});
function doPair(){var c=document.getElementById('code').value;
 if(!/^\d{8}$/.test(c)){S('Please enter exactly 8 digits.','err');return;}
 fetch('/api/paircode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'code='+c})
 .then(r=>r.text()).then(t=>{S(t,'info');setTimeout(refresh,4000);});}
var hostRe=/^[a-z0-9]([a-z0-9-]{0,22}[a-z0-9])?$/i;
document.getElementById('host').addEventListener('input',function(){
 var ok=hostRe.test(this.value);
 document.getElementById('btnHost').disabled=!ok;
 document.getElementById('hostHint').textContent=ok
  ?'New: http://'+this.value.toLowerCase()+'.local'
  :'Letters, digits and hyphens; also used as the setup Wi-Fi name.';});
function doHost(){var v=document.getElementById('host').value;
 if(!hostRe.test(v)){S('Invalid name.','err');return;}
 if(!confirm('Change address? The device restarts and will be reachable at http://'+v.toLowerCase()+'.local.'))return;
 fetch('/api/hostname',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'name='+encodeURIComponent(v)})
 .then(r=>r.text()).then(t=>{S(t,'info');});}
function doReset(){if(!confirm('Factory reset: Wi-Fi, HomeKit pairing and all settings will be erased. Continue?'))return;
 if(!confirm('Are you sure? The device must be set up again afterwards.'))return;
 fetch('/api/reset',{method:'POST'});S('Factory reset – device is restarting…','info');}
var mSel=0;
function ddSet(o){var d=document.getElementById('modelDD');d.classList.toggle('open',o);
 d.closest('.card').classList.toggle('dd-top',o);}
function ddToggle(e){e.stopPropagation();
 ddSet(!document.getElementById('modelDD').classList.contains('open'));}
document.addEventListener('click',function(e){
 if(!document.getElementById('modelDD').contains(e.target))ddSet(false);});
function loadModels(){fetch('/api/models').then(r=>r.json()).then(d=>{
 var l=document.getElementById('modelList');l.innerHTML='';mSel=d.active;
 d.models.forEach(function(m,i){var o=document.createElement('div');
  o.className='dd-item'+(i===d.active?' sel':'');o.textContent=m;
  o.onclick=function(){mSel=i;document.getElementById('modelCur').textContent=m;
   for(var k=0;k<l.children.length;k++)l.children[k].classList.toggle('sel',k===i);
   ddSet(false);};
  l.appendChild(o);});
 document.getElementById('modelCur').textContent=d.models[d.active]||'–';
}).catch(()=>{});}
function doModel(){
 if(!confirm('Switch model? The device restarts and the input sources are recreated.'))return;
 fetch('/api/model',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'idx='+mSel})
 .then(r=>r.text()).then(t=>{S(t,'info');});}
refresh();loadModels();setInterval(refresh,5000);
</script></body></html>)HTML";

// ---------- Webremote (funktioniert ohne HomeKit-Kopplung) ----------
// Eigenstaendiges Design nach Figma-Vorlage (Active / Inactive States):
// runder Power-Button, Quellen-Rad, Fuellstands-Slider fuer Volume und
// Tone (mittig verankert). Bewusst ohne /wml/style.css – die Seite bringt
// ihr komplettes CSS selbst mit und ist damit unabhaengig vom Portal-Theme.

static const char REMOTE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>Rotel Remote</title>
<style>
/* touch-action:manipulation: kein Doppeltipp-Zoom bei schnellen +/- Tipps
   (iOS/Android), Scrollen im Quellen-Rad bleibt erhalten */
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;touch-action:manipulation}
body{background:#eef2fa;font-family:-apple-system,BlinkMacSystemFont,'Inter','Segoe UI',Roboto,sans-serif;color:#132237;min-height:100vh}
/* Oberer Abstand wie beim Geraete-Dashboard (WML: 16px Body-Padding) */
.app{max-width:400px;margin:0 auto;padding:16px;display:flex;flex-direction:column;gap:16px;align-items:center}
.hdr{text-align:center;width:100%}
.hdr h1{font-size:24px;font-weight:700;line-height:24px}
.hdr .fw{font-size:14px;color:rgba(20,31,46,.62);margin-top:2px}
.pwr{width:79px;height:80px;border-radius:100px;border:none;background:#8d9eb9;color:#fff;font-family:inherit;font-size:24px;font-weight:700;box-shadow:0 4px 24px rgba(0,0,0,.1);cursor:pointer;transition:background .2s}
.pwr.on{background:#f00f66}
/* Quellen-Rad: natives Scroll-Snap, Ein-/Ausblenden per CSS-Maske */
.src{position:relative;width:100%;max-width:336px;transition:opacity .2s}
.wheel{height:132px;overflow-y:auto;scroll-snap-type:y mandatory;text-align:center;scrollbar-width:none;-webkit-mask-image:linear-gradient(transparent,#000 30%,#000 70%,transparent);mask-image:linear-gradient(transparent,#000 30%,#000 70%,transparent)}
.wheel::-webkit-scrollbar{display:none}
.wheel::before,.wheel::after{content:'';display:block;height:44px}
.wheel div{height:44px;line-height:44px;scroll-snap-align:center;font-size:16px;cursor:pointer}
.wheel div.on{font-weight:700;font-size:18px}
.lines{position:absolute;top:44px;left:0;right:0;height:44px;border-top:1px solid #ccd5e4;border-bottom:1px solid #ccd5e4;pointer-events:none}
.card{background:rgba(255,255,255,.8);border-radius:24px;box-shadow:0 4px 24px rgba(0,0,0,.1);padding:12px;width:100%;display:flex;flex-direction:column;gap:16px}
.chd{display:flex;justify-content:space-between;align-items:center;width:100%}
.chd .t{font-size:17px;font-weight:700}
.chd .r{display:flex;align-items:center;gap:8px;font-size:14px;transition:opacity .2s}
.tgl{width:44px;min-width:44px;height:28px;border-radius:14px;background:#e6e6e6;display:flex;align-items:center;padding:4px;cursor:pointer;transition:background .15s}
.tgl .kn{width:20px;height:20px;border-radius:12px;background:#00a554;transition:transform .15s,background .15s}
.tgl.on{background:#00a554}
.tgl.on .kn{background:#fff;transform:translateX(16px)}
.strip{display:flex;align-items:center;gap:5px;width:100%;transition:opacity .2s}
.rnd{width:28px;min-width:28px;height:28px;border-radius:14px;background:#eef2fa;border:none;color:#3d4f67;font-family:inherit;font-size:18px;font-weight:700;line-height:1;cursor:pointer;user-select:none;-webkit-user-select:none}
.rnd:active{background:#dbe3f2}
.rnd.sm{font-size:16px}
.trk{flex:1;height:28px;border-radius:14px;background:#eef2fa;position:relative;overflow:hidden}
.fill{position:absolute;top:0;left:0;height:28px;min-width:36px;background:#f00f66;border-radius:14px;color:#fff;font-size:14px;font-weight:700;display:flex;align-items:center;justify-content:flex-end;padding:0 9px;transition:width .2s}
.tone .fill{background:#516682;min-width:28px;padding:0 8px}
.grp{display:flex;flex-direction:column;gap:3px;align-items:center;width:100%;transition:opacity .2s}
.grp .lbl{font-size:14px;font-weight:700;line-height:28px}
.dim{opacity:.5;pointer-events:none}
.settings{display:block;width:100%;height:50px;border-radius:48px;background:#d1daeb;color:#132237;font-size:17px;font-weight:700;line-height:50px;text-align:center;text-decoration:none}
</style></head><body>
<div class="app">

<div class="hdr">
  <h1>Rotel Remote</h1>
  <div class="fw" id="fw">Firmware: –</div>
</div>

<button class="pwr" id="pwr">…</button>

<div class="src" id="srcBox">
  <div class="wheel" id="srcW"></div>
  <div class="lines"></div>
</div>

<div class="card" id="cardVol">
  <div class="chd"><span class="t">Volume</span>
    <span class="r" id="muteRow">Mute <span class="tgl" id="tglMute" onclick="cmd('mute')"><span class="kn"></span></span></span>
  </div>
  <div class="strip" id="volStrip">
    <button class="rnd" onclick="step('vol',-1)">&minus;</button>
    <div class="trk"><div class="fill" id="volFill">–</div></div>
    <button class="rnd" onclick="step('vol',1)">+</button>
  </div>
</div>

<div class="card" id="cardTone">
  <div class="chd"><span class="t">Sound Settings</span>
    <span class="r" id="bypRow">Bypass <span class="tgl" id="tglByp" onclick="cmd('bypass')"><span class="kn"></span></span></span>
  </div>
  <div class="grp" id="grpTreb">
    <div class="lbl">Treble</div>
    <div class="strip">
      <button class="rnd" onclick="step('treb',-1)">&minus;</button>
      <div class="trk tone" onclick="setv('treb',0)"><div class="fill" id="trebFill">–</div></div>
      <button class="rnd" onclick="step('treb',1)">+</button>
    </div>
  </div>
  <div class="grp" id="grpBass">
    <div class="lbl">Bass</div>
    <div class="strip">
      <button class="rnd" onclick="step('bass',-1)">&minus;</button>
      <div class="trk tone" onclick="setv('bass',0)"><div class="fill" id="bassFill">–</div></div>
      <button class="rnd" onclick="step('bass',1)">+</button>
    </div>
  </div>
  <div class="grp" id="grpBal">
    <div class="lbl">Balance</div>
    <div class="strip">
      <button class="rnd sm" onclick="step('bal',-1)">L</button>
      <div class="trk tone" onclick="setv('bal',0)"><div class="fill" id="balFill">–</div></div>
      <button class="rnd sm" onclick="step('bal',1)">R</button>
    </div>
  </div>
</div>

<a class="settings" href="/">Settings</a>

</div>
<script>
var S=null,VOL_MAX=96;
function el(i){return document.getElementById(i)}
function cmd(a){fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'a='+a}).then(()=>setTimeout(st,400)).catch(()=>{});}
/* Klick-Buendelung: UI reagiert sofort, gesendet wird erst nach 350 ms Ruhe
   EIN absoluter Wert - schnelle Klickfolgen fluten sonst die RS232-Strecke
   (Rotel-CPU resettet dann, Verstaerker faellt in Standby) */
var pend={},pt={},hold={};
function cur(k){return k=='vol'?S.vol:k=='bass'?S.bass:k=='treb'?S.treble:S.bal}
function lim(k){return k=='vol'?[0,VOL_MAX]:k=='bal'?[-S.balMax,S.balMax]:[-10,10]}
/* pend[k] bleibt nach dem Senden Rechenbasis fuer Folgeklicks und faellt erst
   weg, wenn die Geraetebestaetigung da sein muss (Hold abgelaufen) - sonst
   wuerden Klicks kurz nach dem Senden vom veralteten Poll-Stand weiterzaehlen */
function fresh(k){if(Date.now()-(hold[k]||0)<=1200)return false;delete pend[k];return true;}
function paint(k,v){if(k=='vol'){var f=el('volFill');f.style.width=Math.max(v/VOL_MAX*100,14)+'%';f.textContent=v;}
 else tone(k=='bass'?'bassFill':k=='treb'?'trebFill':'balFill',v,k=='bal'?(S.balMax||15):10);}
function setv(k,v){if(!S)return;var l=lim(k);v=Math.min(l[1],Math.max(l[0],v));
 pend[k]=v;hold[k]=Date.now();paint(k,v);clearTimeout(pt[k]);
 pt[k]=setTimeout(function(){hold[k]=Date.now();cmd(k+'set&v='+pend[k]);},350);}
function step(k,d){if(S)setv(k,(k in pend?pend[k]:cur(k))+d);}
/* Quellen-Rad: Snap macht der Browser, JS sendet nur die eingerastete Zeile */
var W=el('srcW'),wt,wTouch=0;
function mark(i){for(var k=0;k<W.children.length;k++)W.children[k].className=k===i-1?'on':'';}
W.addEventListener('scroll',function(){wTouch=Date.now();
 var i=Math.round(W.scrollTop/44)+1;mark(i);
 clearTimeout(wt);wt=setTimeout(function(){if(S&&i!==S.src)cmd('src&id='+i);},180);});
/* Runde Pille: bei 0 zentrierter 28px-Chip, sonst waechst sie von der
   Mitte nach aussen; der Wert steht am aeusseren Ende */
function tone(id,v,max){var f=el(id);
 f.style.width='calc(28px + '+(Math.abs(v)/max*39)+'%)';
 f.style.transform=v?'none':'translateX(-50%)';
 if(v<0){f.style.left='auto';f.style.right='calc(50% - 14px)';f.style.justifyContent='flex-start';}
 else if(v>0){f.style.right='auto';f.style.left='calc(50% - 14px)';f.style.justifyContent='flex-end';}
 else{f.style.right='auto';f.style.left='50%';f.style.justifyContent='center';}
 f.textContent=v;}
function st(){fetch('/api/state').then(r=>r.json()).then(d=>{S=d;render(d);}).catch(()=>{});}
function render(d){
 el('fw').textContent='Firmware: '+d.fw;
 var p=el('pwr');p.textContent=d.power?'ON':'OFF';p.classList.toggle('on',d.power);
 p.onclick=function(){cmd(d.power?'power_off':'power_on')};
 if(d.powerAmp){el('srcBox').style.display='none';el('cardVol').style.display='none';el('cardTone').style.display='none';return;}
 if(!d.hasTone)el('cardTone').style.display='none';
 var off=!d.power;
 el('srcBox').classList.toggle('dim',off);
 el('muteRow').classList.toggle('dim',off);
 el('bypRow').classList.toggle('dim',off);
 el('volStrip').classList.toggle('dim',off||d.mute);
 el('grpTreb').classList.toggle('dim',off||d.bypass);
 el('grpBass').classList.toggle('dim',off||d.bypass);
 el('grpBal').classList.toggle('dim',off);
 el('tglMute').classList.toggle('on',d.mute);
 el('tglByp').classList.toggle('on',d.bypass);
 if(fresh('vol'))paint('vol',d.vol);
 if(fresh('treb'))tone('trebFill',d.treble,10);
 if(fresh('bass'))tone('bassFill',d.bass,10);
 if(fresh('bal'))tone('balFill',d.bal,d.balMax||15);
 if(!W.children.length)d.sources.forEach(function(s){var o=document.createElement('div');o.textContent=s;W.appendChild(o);});
 if(Date.now()-wTouch>1200){W.scrollTop=(d.src-1)*44;mark(d.src);}
}
st();setInterval(st,1500);
</script></body></html>)HTML";

// ---------- Webremote: Befehls-Mailbox (async_tcp-Task -> Haupt-Loop) ----------
// Die TX-Queue von rotelCallback ist bewusst Single-Producer (Haupt-Loop).
// HTTP-Handler legen Aktionen deshalb in diesen kleinen SPSC-Ring; erst
// webPortalLoop() ruft die _rotel-Methoden auf. Byte-Indizes, keine Sperren.

// Volume/Bass/Treble/Balance kommen als ABSOLUTE Setzbefehle an: das Web-UI
// buendelt schnelle Klickfolgen per Debounce zu einem Wert. Einzelschritt-
// Befehle im 100-ms-Takt koennen laut Protokoll-Doku die Geraete-CPU resetten
// (Verstaerker faellt in Standby).
enum WrAction : uint8_t {
  WR_NONE = 0, WR_POWER_ON, WR_POWER_OFF, WR_SOURCE,
  WR_MUTE, WR_BYPASS,
  WR_VOL_SET, WR_BASS_SET, WR_TREBLE_SET, WR_BAL_SET
};

static constexpr uint8_t WR_RING_LEN = 8;
static volatile uint8_t wrActionRing[WR_RING_LEN];
static volatile int16_t wrParamRing[WR_RING_LEN];
static volatile uint8_t wrHead = 0;   // Schreiber: async_tcp-Task
static volatile uint8_t wrTail = 0;   // Leser: Haupt-Loop

static bool wrPush(uint8_t action, int16_t param){
  const uint8_t next = (uint8_t)((wrHead + 1) % WR_RING_LEN);
  if (next == wrTail) return false;    // Ring voll -> Befehl verwerfen
  wrActionRing[wrHead] = action;
  wrParamRing[wrHead]  = param;
  wrHead = next;                       // erst nach den Daten publizieren
  return true;
}

// Name aus dem POST-Parameter -> Aktion (Tabelle im Flash)
struct WrCmdName { const char *name; uint8_t action; };
static const WrCmdName WR_CMD_NAMES[] = {
  { "power_on", WR_POWER_ON }, { "power_off", WR_POWER_OFF },
  { "src",      WR_SOURCE },
  { "mute",     WR_MUTE },     { "bypass",    WR_BYPASS },
  { "volset",   WR_VOL_SET },  { "bassset",   WR_BASS_SET },
  { "trebset",  WR_TREBLE_SET }, { "balset",  WR_BAL_SET },
};

// ---------- Routen (Handler laufen im async_tcp-Task: nur Flags setzen!) ----------

static void registerDashboardRoutes(){

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200, "text/html", DASHBOARD_HTML);
  });

  webServer.on("/remote", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200, "text/html", REMOTE_HTML);
  });

  webServer.on("/app.css", HTTP_GET, [](AsyncWebServerRequest *req){
    req->send(200, "text/css", APP_CSS);
  });

  webServer.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *req){
    // Statischer Puffer (async_tcp arbeitet Requests sequenziell ab):
    // ~170 B Festanteil + max. 15 Quellen a ~14 B -> 768 B reichen sicher
    static char json[768];
    size_t pos = snprintf(json, sizeof(json),
      "{\"power\":%s,\"vol\":%d,\"src\":%d,\"mute\":%s,\"bypass\":%s,"
      "\"bass\":%d,\"treble\":%d,\"bal\":%d,\"balMax\":%d,"
      "\"hasTone\":%s,\"powerAmp\":%s,\"fw\":\"%s\",\"sources\":[",
      _rotel._currentPower ? "true" : "false",
      _rotel._currentVolume, _rotel._currentSource,
      _rotel._currentMute ? "true" : "false",
      _rotel._currentBypass ? "true" : "false",
      _rotel._currentBass, _rotel._currentTreble, _rotel._currentBalance,
      (int)rotelModel->balanceMax,
      rotelModel->hasTone ? "true" : "false",
      (rotelModel->deviceType == ROTEL_POWER_AMP) ? "true" : "false",
      wymrfirmware);
    for (uint8_t i = 0; i < rotelModel->sourceCount && pos < sizeof(json); i++)
      pos += snprintf(json + pos, sizeof(json) - pos, "%s\"%s\"",
                      i ? "," : "", rotelModel->sources[i].name);
    if (pos < sizeof(json)) pos += snprintf(json + pos, sizeof(json) - pos, "]}");
    req->send(200, "application/json", json);
  });

  webServer.on("/api/cmd", HTTP_POST, [](AsyncWebServerRequest *req){
    if (!req->hasParam("a", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
    const String &a = req->getParam("a", true)->value();
    uint8_t action = WR_NONE;
    int16_t param  = 0;
    for (const WrCmdName &c : WR_CMD_NAMES)
      if (a == c.name) { action = c.action; break; }
    if (action == WR_NONE) { req->send(400, "text/plain", "Unknown command"); return; }
    if (action == WR_SOURCE) {
      if (!req->hasParam("id", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
      const int id = req->getParam("id", true)->value().toInt();
      if (id < 1 || id > (int)rotelModel->sourceCount) {
        req->send(400, "text/plain", "Invalid source");
        return;
      }
      param = (int16_t)id;
    } else if (action >= WR_VOL_SET) {
      // Absolutwert-Befehle: Bereich VOR der Uebernahme pruefen
      if (!req->hasParam("v", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
      const int v = req->getParam("v", true)->value().toInt();
      const int lo = (action == WR_VOL_SET) ? 0
                   : (action == WR_BAL_SET) ? -(int)rotelModel->balanceMax : -10;
      const int hi = (action == WR_VOL_SET) ? 96
                   : (action == WR_BAL_SET) ? (int)rotelModel->balanceMax : 10;
      if (v < lo || v > hi) { req->send(400, "text/plain", "Value out of range"); return; }
      param = (int16_t)v;
    }
    if (!wrPush(action, param)) { req->send(429, "text/plain", "Too many commands"); return; }
    req->send(202, "text/plain", "OK");
  });

  webServer.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *req){
    // Statischer Puffer: der async_tcp-Task arbeitet Requests sequenziell ab
    static char json[640];
    const char *src = "-";
    if (_rotel._currentSource >= 1 && _rotel._currentSource <= (int)rotelModel->sourceCount)
      src = rotelModel->sources[_rotel._currentSource - 1].name;
    snprintf(json, sizeof(json),
      "{\"fw\":\"%s\",\"remote\":\"%s\",\"updateAvail\":%s,\"updating\":%s,"
      "\"progress\":%d,\"error\":\"%s\",\"pair\":\"%s\",\"pairMsg\":\"%s\","
      "\"model\":\"%s\",\"gen\":\"%s\",\"host\":\"%s\","
      "\"power\":%s,\"source\":\"%s\",\"heap\":%lu}",
      wymrfirmware, wpRemoteVersion,
      wpUpdateAvailable ? "true" : "false",
      wpUpdateInProgress ? "true" : "false",
      (int)wpUpdateProgress, wpLastError,
      wpPairCode[0] ? wpPairCode : PAIRCODE_DEFAULT, wpPairMessage,
      rotelModel->name, _rotel.generationLabel(), wpHostName,
      _rotel._currentPower ? "true" : "false", src,
      (unsigned long)ESP.getFreeHeap());
    req->send(200, "application/json", json);
  });

  webServer.on("/api/models", HTTP_GET, [](AsyncWebServerRequest *req){
    // Modellliste aus der Flash-Tabelle; Namen sind ASCII ohne Anfuehrungszeichen.
    // Aktiver Index aus dem Modellzeiger (kein NVS-Zugriff im async_tcp-Task).
    static char json[640];
    size_t pos = snprintf(json, sizeof(json), "{\"active\":%u,\"models\":[",
                          (unsigned)(rotelModel - ROTEL_MODELS));
    for (uint8_t i = 0; i < ROTEL_MODEL_COUNT && pos < sizeof(json); i++) {
      pos += snprintf(json + pos, sizeof(json) - pos, "%s\"%s\"",
                      i ? "," : "", ROTEL_MODELS[i].name);
    }
    if (pos < sizeof(json)) pos += snprintf(json + pos, sizeof(json) - pos, "]}");
    req->send(200, "application/json", json);
  });

  webServer.on("/api/model", HTTP_POST, [](AsyncWebServerRequest *req){
    if (!req->hasParam("idx", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
    const int idx = req->getParam("idx", true)->value().toInt();
    if (idx < 0 || idx >= (int)ROTEL_MODEL_COUNT) {
      req->send(400, "text/plain", "Unknown model");
      return;
    }
    wpPendingModel = (uint8_t)idx;
    wpModelRequested = true;   // NVS-Schreiben + Reboot im Haupt-Loop
    wpModelRequestAt = millis();
    req->send(202, "text/plain", "Model saved - device is restarting...");
  });

  webServer.on("/api/hostname", HTTP_POST, [](AsyncWebServerRequest *req){
    if (!req->hasParam("name", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
    const String &name = req->getParam("name", true)->value();
    if (!hostNameAllowed(name.c_str())) {
      req->send(400, "text/plain", "Invalid: 1-24 characters, letters/digits/hyphens");
      return;
    }
    strlcpy(wpPendingHost, name.c_str(), sizeof(wpPendingHost));
    wpHostRequested = true;   // NVS-Schreiben + Reboot im Haupt-Loop
    wpHostRequestAt = millis();
    req->send(202, "text/plain", "Address saved - device is restarting...");
  });

  webServer.on("/api/check", HTTP_POST, [](AsyncWebServerRequest *req){
    wpCheckRequested = true;
    req->send(202, "text/plain", "Check scheduled");
  });

  webServer.on("/api/update", HTTP_POST, [](AsyncWebServerRequest *req){
    if (!wpUpdateAvailable) { req->send(409, "text/plain", "No update available"); return; }
    wpUpdateRequested = true;
    req->send(202, "text/plain", "Update scheduled");
  });

  webServer.on("/api/paircode", HTTP_POST, [](AsyncWebServerRequest *req){
    if (!req->hasParam("code", true)) { req->send(400, "text/plain", "Missing parameter"); return; }
    const String &code = req->getParam("code", true)->value();
    if (!pairCodeAllowed(code.c_str())) {
      req->send(400, "text/plain", "Invalid: 8 digits, no trivial codes");
      return;
    }
    strlcpy(wpPendingPairCode, code.c_str(), sizeof(wpPendingPairCode));
    wpPairRequested = true;   // SRP-Berechnung erfolgt im Haupt-Loop (blockiert einige Sekunden)
    req->send(202, "text/plain", "Setting code (takes a few seconds)");
  });

  webServer.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *req){
    wpResetRequested = true;
    wpResetRequestAt = millis();
    req->send(202, "text/plain", "Performing factory reset");
  });
}

// ---------- Setup / Loop ----------

void webPortalSetup(){

  // Klartext-Kopie des Pairing-Codes laden (nur gesetzt, wenn er je ueber
  // dieses Portal geaendert wurde – der HomeSpan-CLI-Weg 'S' laeuft daran vorbei)
  devicePrefs.begin("rotelcfg", true);
  devicePrefs.getString("paircode", wpPairCode, sizeof(wpPairCode));
  devicePrefs.end();

  // WML-Konfiguration: mDNS muss aus bleiben – HomeSpan besitzt den
  // mDNS-Responder fuer HomeKit (_hap._tcp); ein zweites MDNS.begin()
  // wuerde dessen Hostname/Records ueberschreiben
  WML::Config cfg = wmlConfigProvider.getConfig();
  if (cfg.enableMDNS) {
    cfg.enableMDNS = false;
    wmlConfigProvider.updateConfig(cfg);
  }

  wifiMgr.setConfigProvider(&wmlConfigProvider);
  wifiMgr.setIdentityBaseName(wpHostName);   // Einrichtungs-WLAN folgt der Geraete-Adresse

  wmlPortal.setDeviceName("Rotel Remote");
  wmlPortal.setFirmwareVersion(wymrfirmware);
  wmlPortal.onConfigGet([](){ return wmlConfigProvider.getConfig(); });
  wmlPortal.onConfigChange([](const WML::Config &c){ return wmlConfigProvider.updateConfig(c); });
  // Reset aus dem WML-Portal loest denselben Voll-Reset aus wie der Dashboard-Button
  wmlPortal.onFactoryReset([](){ doFactoryReset(); });
  wmlPortal.onWiFiReset([](){
    WML::Config c = wmlConfigProvider.getConfig();
    c.primary.setSsid(""); c.primary.setPassword("");
    c.secondary.setSsid(""); c.secondary.setPassword("");
    wmlConfigProvider.updateConfig(c);
  });

  fwUpdate.setProgressCallback(fwProgressCallback);
  fwUpdate.setTimeout(60000);
  fwUpdate.setRetryCount(1);

  registerDashboardRoutes();   // eigene Routen vor portal.begin() registrieren
  wifiMgr.begin();
  wmlPortal.begin();
  webServer.begin();
}

void webPortalLoop(){

  wifiMgr.loop();
  wmlPortal.loop();

  // Webremote-Befehle aus dem HTTP-Ring in die Rotel-TX-Queue uebertragen –
  // so bleibt der Haupt-Loop der einzige Producer der TX-Queue
  while (wrTail != wrHead) {
    const uint8_t action = wrActionRing[wrTail];
    const int16_t param  = wrParamRing[wrTail];
    wrTail = (uint8_t)((wrTail + 1) % WR_RING_LEN);
    switch (action) {
      case WR_POWER_ON:   _rotel.setPower(true);         break;
      case WR_POWER_OFF:  _rotel.setPower(false);        break;
      case WR_SOURCE:     _rotel.setSource(param);       break;
      case WR_MUTE:       _rotel.toggleMute();           break;
      case WR_BYPASS:     _rotel.toggleBypass();         break;
      case WR_VOL_SET:    _rotel.setVolumeNN(param);     break;
      case WR_BASS_SET:   _rotel.setBassValue(param);    break;
      case WR_TREBLE_SET: _rotel.setTrebleValue(param);  break;
      case WR_BAL_SET:    _rotel.setBalanceValue(param); break;
      default: break;
    }
  }

  // Pairing-Code setzen: SRP-Berechnung blockiert einige Sekunden – hier im
  // Haupt-Loop unkritisch (typischerweise waehrend der Einrichtung)
  if (wpPairRequested) {
    wpPairRequested = false;
    char cmd[PAIRCODE_LEN + 3];
    snprintf(cmd, sizeof(cmd), "S %s", wpPendingPairCode);
    homeSpan.processSerialCommand(cmd);
    strlcpy(wpPairCode, wpPendingPairCode, sizeof(wpPairCode));
    devicePrefs.begin("rotelcfg", false);
    devicePrefs.putString("paircode", wpPairCode);   // Klartext-Kopie fuer Anzeige
    devicePrefs.end();
    snprintf(wpPairMessage, sizeof(wpPairMessage), "New HomeKit code active");
  }

  // Update-Check: ein HTTP-Request (~1 s), blockiert den Loop kurz
  if (wpCheckRequested) {
    wpCheckRequested = false;
    wpLastError[0] = '\0';
    wpPairMessage[0] = '\0';   // sonst verdeckt die alte Pairing-Meldung Fehler dauerhaft
    if (fwUpdate.checkForUpdate()) {
      wpUpdateAvailable = true;
      strlcpy(wpRemoteVersion, fwUpdate.getRemoteVersion(), sizeof(wpRemoteVersion));
    } else {
      wpUpdateAvailable = false;
      strlcpy(wpRemoteVersion, "", sizeof(wpRemoteVersion));
      if (fwUpdate.getLastError() != GitFirmwareUpdate::NO_UPDATE_AVAILABLE)
        strlcpy(wpLastError, fwUpdate.getLastErrorString(), sizeof(wpLastError));
      else
        strlcpy(wpRemoteVersion, "up to date", sizeof(wpRemoteVersion));
    }
  }

  // Update installieren: blockiert bis Reboot (Fortschritt liefert der
  // async_tcp-Task weiter ueber /api/info aus)
  if (wpUpdateRequested) {
    wpUpdateRequested = false;
    wpUpdateInProgress = true;
    wpUpdateProgress = 0;
    wpPairMessage[0] = '\0';
    if (!fwUpdate.performUpdate()) {          // bei Erfolg: Reboot, kein Ruecksprung
      wpUpdateInProgress = false;
      strlcpy(wpLastError, fwUpdate.getLastErrorString(), sizeof(wpLastError));
    }
  }

  // Modellwechsel: Auswahl + Generation-Cache im NVS aktualisieren, dann die
  // gespeicherten Characteristic-Werte loeschen (Quellen-Anzahl/-Namen aendern
  // sich -> alte aid/iid-Zuordnung waere falsch) und neu starten. Kurz warten,
  // damit die HTTP-Antwort den Browser erreicht.
  if (wpModelRequested && (millis() - wpModelRequestAt >= RESET_DELAY_MS)) {
    wpModelRequested = false;
    rotelSaveModelIndex(wpPendingModel);
    homeSpan.processSerialCommand("V");   // charNVS leeren (rebootet nicht)
    ESP.restart();
  }

  // Geraete-Adresse (mDNS-/AP-Basisname): im NVS speichern und neu starten –
  // HomeSpan setzt den mDNS-Hostnamen nur einmal in begin()
  if (wpHostRequested && (millis() - wpHostRequestAt >= RESET_DELAY_MS)) {
    wpHostRequested = false;
    devicePrefs.begin("rotelcfg", false);
    devicePrefs.putString("hostname", wpPendingHost);
    devicePrefs.end();
    ESP.restart();
  }

  // Werksreset: kurz warten, damit die HTTP-Antwort den Browser erreicht
  if (wpResetRequested && (millis() - wpResetRequestAt >= RESET_DELAY_MS)) {
    wpResetRequested = false;
    doFactoryReset();   // rebootet
  }
}
