"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

// ---- web password ----
// Every /api call goes through api(). Once a password is set, a 401 means this browser
// has no valid session cookie: the login view replaces the tabs and the polls stop.
// The login sends the password once (HTTP Basic); the brush answers with the cookie.
// A 401 is asked again once first: a poll that waited behind the Save that set a new
// password went out with the old cookie, and the new one is in the jar by then (the
// brush refuses before the handler runs, so a POST is safe to repeat).
let LOCKED = false;
async function api(url, opts = {}){
  let r = await fetch(url, opts);
  if (r.status === 401) r = await fetch(url, opts);
  if (r.status === 401){ showLogin(); throw new Error("password required"); }
  return r;
}
const postJson = (url, body) => api(url, {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
function showLogin(){
  if (LOCKED) return;
  LOCKED = true; logStop();
  $("nav").hidden = true;
  $$(".tab").forEach(t => t.classList.toggle("active", t.id === "login"));
  $("#conn").textContent = "locked"; $("#conn").className = "pill";
  $("#loginPass").focus();
}
$("#loginForm").onsubmit = async (e) => {
  e.preventDefault();
  const msg = $("#loginMsg");
  let auth;
  try { auth = "Basic " + btoa("oclean:" + $("#loginPass").value); }
  catch(_){ msg.textContent = "wrong password"; return; }   // not ASCII: cannot be the password
  msg.textContent = "checking…";
  try {
    const r = await fetch("/api/config", {headers:{Authorization:auth}, cache:"no-store"});
    if (r.ok){
      // Right password; but a browser that does not keep the cookie (this page inside
      // another site's frame, or cookies blocked) would come straight back here.
      if ((await fetch("/api/status", {cache:"no-store"})).status === 401){
        msg.textContent = "password accepted, but this browser did not keep the login: open the brush in a tab of its own, by its IP address";
        return;
      }
      location.reload(); return;
    }
    msg.textContent = r.status === 401 ? "wrong password" : "failed: " + r.status;
  } catch(_){ msg.textContent = "the brush did not answer"; }
};

// ---- tabs ----
$$("nav button").forEach(b => b.onclick = () => {
  $$("nav button").forEach(x => x.classList.toggle("active", x === b));
  $$(".tab").forEach(t => t.classList.toggle("active", t.id === b.dataset.tab));
  if (b.dataset.tab === "files") parts();
  if (b.dataset.tab === "logs") logStart(); else logStop();
});

// ---- dashboard / metrics ----
const LABELS = {
  battery:"Battery", battery_mv:"Battery Voltage", charging:"Charging", power_state:"Power", brushing:"Brushing",
  paused:"Paused", mode:"Mode", strength:"Intensity", session_secs:"Session Elapsed", session_total:"Session Length",
  brush_score:"Brush Score", sessions_today:"Sessions Today", seconds_today:"Brushed Today", screen:"Screen",
  asleep:"Screen Off", locked:"Touch Lock", pressure:"Pressure", imu_temp:"Brush Temp",
  fw_version:"Firmware", uptime:"Uptime", free_heap:"Free Heap", min_free_heap:"Min Heap",
  largest_block:"Largest Block", task_count:"Tasks", cpu_temp:"CPU Temp", reset_reason:"Reset Reason",
  idf_version:"ESP-IDF", app_version:"App Version", flash_size:"Flash", wifi_rssi:"Wi-Fi RSSI",
  wifi_ssid:"SSID", wifi_ip:"IP", wifi_channel:"Channel", mac:"MAC"
};
const UNITS = { battery:"%", battery_mv:"mV", session_secs:"s", session_total:"s", seconds_today:"s", imu_temp:"°C", uptime:"s",
  free_heap:"B", min_free_heap:"B", largest_block:"B", cpu_temp:"°C", flash_size:"B", wifi_rssi:"dBm" };

function fmt(k, v){
  if (v === null || v === undefined) return "—";
  if (["free_heap","min_free_heap","largest_block","flash_size"].includes(k) && typeof v==="number"){
    if (v>=1048576) return (v/1048576).toFixed(1)+" MB"; if (v>=1024) return (v/1024).toFixed(0)+" KB";
  }
  if (k==="uptime" && typeof v==="number"){ const d=Math.floor(v/86400),h=Math.floor(v%86400/3600),m=Math.floor(v%3600/60);
    return (d?d+"d ":"")+(h?h+"h ":"")+m+"m"; }
  if (k==="cpu_temp" && typeof v==="number") return v.toFixed(1);
  return v;
}
let otaBusy = false;   // a firmware upload is in flight: the server can't answer anything else
let PROJECT = null;    // project name of the running firmware, once known
async function refresh(){
  if (otaBusy || LOCKED) return;
  try{
    const r = await api("/api/status"); const j = await r.json();
    PROJECT = j.project ?? null;
    $("#conn").textContent = (j.wifi_connected?"Wi-Fi":"AP") + (j.mqtt_connected?" · MQTT":"") + (j.ble_connected?" · BLE":"") + (j.safe_mode?" · SAFE MODE":"") + (j.oem_pictures?"":" · no OEM pictures");
    renderDiag(j);
    $("#conn").className = "pill " + ((j.wifi_connected&&j.mqtt_connected)?"ok":(j.wifi_connected?"":"bad"));
    $("#fwVer").value = j.metrics.app_version ?? "";
    const g = $("#metrics"); g.innerHTML = "";
    for (const [k,v] of Object.entries(j.metrics)){
      const unit = UNITS[k] ? `<span class="u">${UNITS[k]==="B"||["KB","MB"].some(x=>String(fmt(k,v)).includes(x))?"":UNITS[k]}</span>`:"";
      const d = document.createElement("div"); d.className="metric";
      d.innerHTML = `<div class="k">${LABELS[k]||k}</div><div class="v">${fmt(k,v)}${k in UNITS && !String(fmt(k,v)).match(/[A-Za-z]$/)?' <span class="u">'+UNITS[k]+'</span>':''}</div>`;
      g.appendChild(d);
    }
  }catch(e){ if (!LOCKED){ $("#conn").textContent="offline"; $("#conn").className="pill bad"; } }
}
setInterval(refresh, 5000); refresh();

// ---- brush tab ----
function renderDiag(j){
  const m = j.metrics, d = j.diag || {};
  const st = m.brushing==="ON" ? `brushing ${m.session_secs}/${m.session_total} s` : m.paused==="ON" ? "paused" : m.asleep==="ON" ? "screen off" : `idle (screen ${m.screen})`;
  $("#brushState").textContent = st + ` · mode ${m.mode} · intensity ${m.strength}`;
  const rows = {
    "Touch state": d.touch_state + (d.touch_state===5?" (running)":""), "Touch X/Y": `${d.touch_x} / ${d.touch_y}`,
    "Force sensor": d.force_available ? "ok" : "unavailable", "Force raw / base": `${d.force_raw} / ${d.force_base}`,
    "Force coef": d.force_coef, "Pressure": m.pressure, "Motor state": d.motor_state, "Gear": d.gear,
    "Battery fault": d.batt_fault ? "yes" : "no", "OTA flag": d.ota ? "set" : "clear", "Screen": m.screen,
    "Battery raw": `${d.batt_raw_mv} <span class="u">mV</span>`,
    "Charger input": d.charger_present == null ? "unknown" : d.charger_present ? "present" : "absent",
    "Charge pin": d.charge_blocked == null ? "unknown" : d.charge_blocked ? (d.thermal_cut ? "blocked (hot)" : "blocked") : "allowed",
    "Thermal cut-off": d.thermal_cut ? "latched" : "no", "Alive edges": d.alive_edges,
    "CPU scaling": `APB lock ${d.pm_apb_lock ? "held" : "free"}, CPU locks ${d.pm_cpu_locks}`,
  };
  const g = $("#diag"); g.innerHTML = "";
  for (const [k,v] of Object.entries(rows)){
    const e = document.createElement("div"); e.className="metric";
    e.innerHTML = `<div class="k">${k}</div><div class="v">${v}</div>`; g.appendChild(e);
  }
}
async function brushCmd(body){
  $("#brushMsg").textContent = "sending…";
  try { const r = await postJson("/api/brush", body);
    $("#brushMsg").textContent = r.ok ? "sent ✓" : "failed: " + r.status; }
  catch(e){ $("#brushMsg").textContent = "failed"; }
  setTimeout(refresh, 800);
}
$("#brushStart").onclick = () => brushCmd({brushing:true});
$("#brushStop").onclick = () => brushCmd({brushing:false});
$("#brushForm").onsubmit = (e) => { e.preventDefault(); const f = e.target;
  brushCmd({mode:Number(f.elements.mode.value), strength:Number(f.elements.strength.value)}); };

// ---- settings ----
// POSIX TZ string of this browser's time zone, e.g. "<+01>-1<+02>,M3.5.0,M10.5.0/3"
// (the brush has no zone database, so a name like Europe/Berlin is of no use to it).
// The offsets and this year's two clock changes are read from Date; a change is taken
// to fall on the n-th, or the last, such weekday of its month every year, which is how
// nearly every zone defines it.
function browserTz(){
  const y = new Date().getFullYear(), H = 3600e3, M = 60e3;
  const off = t => -new Date(t).getTimezoneOffset();                  // minutes east of UTC
  const p2 = n => String(n).padStart(2, "0");
  const hm = m => (m < 0 ? "-" : "") + Math.floor(Math.abs(m) / 60) + (m % 60 ? ":" + p2(Math.abs(m) % 60) : "");
  const name = m => "<" + (m < 0 ? "-" : "+") + p2(Math.floor(Math.abs(m) / 60)) + (m % 60 ? p2(Math.abs(m) % 60) : "") + ">";
  const zone = m => name(m) + hm(-m);                                 // POSIX counts the offset westwards
  const ch = [];                                                      // [instant, minutes east before, after]
  for (let t = Date.UTC(y, 0, 1); t < Date.UTC(y + 1, 0, 1); t += H){
    if (off(t) === off(t + H)) continue;
    let a = t, b = t + H;                                             // narrow it down to the minute
    while (b - a > M){ const m = a + Math.floor((b - a) / (2 * M)) * M; if (off(m) === off(a)) a = m; else b = m; }
    ch.push([b, off(a), off(b)]);
  }
  if (ch.length !== 2 || ch[0][1] !== ch[1][2]){                      // no daylight saving time
    const now = off(Date.now()); return now ? zone(now) : "UTC0";
  }
  const [dst, std] = ch[0][2] > ch[0][1] ? ch : [ch[1], ch[0]];       // the change to summer time, and the one back
  const rule = ([t, before]) => {
    const d = new Date(t + before * M);                               // the wall clock at the change, read with getUTC*
    const last = d.getUTCDate() + 7 > new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 0)).getUTCDate();
    const min = d.getUTCHours() * 60 + d.getUTCMinutes();
    return `M${d.getUTCMonth() + 1}.${last ? 5 : Math.ceil(d.getUTCDate() / 7)}.${d.getUTCDay()}` + (min === 120 ? "" : "/" + hm(min));
  };
  return zone(dst[1]) + name(dst[2]) + (dst[2] - dst[1] === 60 ? "" : hm(-dst[2])) + "," + rule(dst) + "," + rule(std);
}
function tzFromBrowser(){
  $("#cfgForm").elements.tz.value = browserTz();
  $("#tzMsg").textContent = "from this browser — Save to apply";
}
$("#tzBtn").onclick = tzFromBrowser;
let PASS_SET = false, CFG_SSID = "", SAFE = false;   // as loaded: a web password exists, the saved network, safe mode
function webPassHint(set){
  PASS_SET = !!set;
  $("#cfgForm").elements.web_pass.placeholder = set ? "(unchanged)" : "(none set: the web UI is open)";
  $("#wifiNeedsPass").hidden = PASS_SET;
  $("#wifiNeedsPass").textContent = SAFE
    ? "Safe mode: firmware update only. A first web UI password, and with it a network, can be set only once the brush runs normally: Reboot, then set both over the setup AP."
    : "The brush joins its Wi-Fi network only once a web UI password is set: set one below, in the same save as the network.";
}
// Save stays disabled until this has worked: saving the form as the page first shows it
// would write its blank fields, an empty Wi-Fi name among them.
async function loadCfg(){
  const c = await (await api("/api/config")).json();
  const f = $("#cfgForm");
  for (const [k,v] of Object.entries(c)){
    const el = f.elements[k]; if (!el) continue;
    if (el.type === "checkbox") el.checked = !!v; else el.value = v ?? "";
    if (el.tagName === "SELECT") el.value = String(v ?? 0);
  }
  SAFE = !!c.safe_mode;
  webPassHint(c.web_pass_set);
  CFG_SSID = c.wifi_ssid ?? "";
  // No network or no web password yet (then the brush is on its setup AP): Settings is
  // what this visit is for. Not in safe mode, which is for a firmware update.
  if ((!CFG_SSID || !PASS_SET) && !LOCKED && !SAFE) $('nav button[data-tab="settings"]').click();
  // A brush that was never given a time zone runs on UTC: offer the browser's.
  if (c.tz === "UTC0" && browserTz() !== "UTC0") tzFromBrowser();
  $("#saveBtn").disabled = false;
}
const SECRETS = ["mqtt_pass", "wifi_pass", "web_pass"];   // blank = unchanged, cleared after Save
$("#cfgForm").onsubmit = async (e) => {
  e.preventDefault();
  const f = e.target, body = {};
  for (const el of f.elements){
    if (!el.name) continue;
    if (el.type === "checkbox") body[el.name] = el.checked;
    else if (el.type === "number" || el.tagName === "SELECT") body[el.name] = Number(el.value);
    else if (el.value !== "" || !SECRETS.includes(el.name)) body[el.name] = el.value;
  }
  if (SAFE && !PASS_SET && (body.web_pass || (body.wifi_ssid && body.wifi_ssid !== CFG_SSID))){
    $("#saveMsg").textContent = "safe mode takes no first web UI password and no new network without one: Reboot, then set them over the setup AP";
    return;
  }
  // As the brush rules: no new network without a web password (one set in this save counts).
  // A Wi-Fi password typed again for the same network is left to the brush, which can tell.
  if (!PASS_SET && !body.web_pass && body.wifi_ssid && body.wifi_ssid !== CFG_SSID){
    $("#saveMsg").textContent = "set a web UI password first: the brush joins no Wi-Fi network without one";
    f.elements.web_pass.focus();
    return;
  }
  $("#saveMsg").textContent = "saving…";
  let r;
  try { r = await postJson("/api/config", body); } catch(_){ $("#saveMsg").textContent = "save failed"; return; }
  if (!r.ok){ $("#saveMsg").textContent = "save failed: " + (await r.text() || r.status); return; }
  const j = await r.json();
  $("#saveMsg").textContent = !j.saved ? "save failed"
    : !PASS_SET && j.web_pass_set && body.wifi_ssid ? `saved ✓ — the brush now joins ${body.wifi_ssid}: find it there`
    : "saved ✓ (applied live)";
  SECRETS.forEach(n => f.elements[n].value = "");
  webPassHint(j.web_pass_set);
  if (j.saved) CFG_SSID = body.wifi_ssid ?? CFG_SSID;
  if (j.tz !== undefined){   // the zone in effect: the brush keeps the old one if it is given no POSIX TZ string
    $("#tzMsg").textContent = j.tz === body.tz ? "" : "not a POSIX TZ string — kept the previous zone";
    f.elements["tz"].value = j.tz;
  }
};
$("#rebootBtn").onclick = async () => { if(confirm("Reboot device?")){ try { await postJson("/api/reboot", {}); } catch(_){} $("#saveMsg").textContent="rebooting…"; } };
function loadCfgRetry(){
  loadCfg().then(() => { if ($("#saveMsg").textContent.startsWith("could not")) $("#saveMsg").textContent = ""; })
    .catch(() => {
      if (LOCKED) return;                   // a 401: the login view took over
      $("#saveMsg").textContent = "could not load the settings — retrying…";
      setTimeout(loadCfgRetry, otaBusy ? 10000 : 3000);
    });
}
loadCfgRetry();

// ---- firmware update ----
// Describe an image from its head: the ESP image magic, the ESP-IDF app
// descriptor (project / version) if it has one, and whether it is a merged
// full-flash image (partition table at 0x8000) rather than an app image.
async function inspectImage(f){
  const b = new Uint8Array(await f.slice(0, 0x8022).arrayBuffer());
  const str = (o, n) => new TextDecoder().decode(b.subarray(o, o + n)).split("\0")[0];
  const desc = b.length >= 288 && b[32] === 0x32 && b[33] === 0x54 && b[34] === 0xCD && b[35] === 0xAB;
  return {
    esp: b[0] === 0xE9,
    merged: b.length >= 0x8022 && b[0x8000] === 0xAA && b[0x8001] === 0x50 && b[0x8020] === 0xAA && b[0x8021] === 0x50,
    project: desc ? str(80, 32) : null,
    version: desc ? str(48, 32) : null,
  };
}
$("#otaForm").onsubmit = async (e) => {
  e.preventDefault();
  const f = $("#otaFile").files[0], msg = $("#otaMsg"), bar = $("#otaBar"), btn = e.target.querySelector("button");
  if (!f) { msg.textContent = "choose a firmware image first"; return; }
  if (dumping) { msg.textContent = "wait for the partition copy (Files tab) to finish"; return; }
  let im;
  try { im = await inspectImage(f); }
  catch (_) { msg.textContent = "could not read that file — select it again"; return; }
  let note = "";
  if (!im.esp) note = "This does not look like an ESP firmware image; the device will reject it.";
  else if (im.merged) note = "This looks like a merged full-flash image (bootloader + partition table + app). It cannot boot from an update slot: choose the app image instead.";
  else if (PROJECT !== null && im.project !== PROJECT) note = "This is not a build of the running firmware, so it is installed without automatic rollback: if it does not work, the device has to be reflashed over UART.";
  const what = im.project === null ? "no ESP-IDF app descriptor" : `${im.project} ${im.version}`;
  if (!confirm(`Flash ${f.name} (${human(f.size)}, ${what}) and reboot?` + (note ? "\n\n" + note : ""))) return;
  const fail = (text) => { msg.textContent = text; otaBusy = false; btn.disabled = false; bar.hidden = true; };
  const x = new XMLHttpRequest();
  x.open("POST", "/api/ota");
  x.setRequestHeader("Content-Type", "application/octet-stream");
  x.upload.onprogress = (ev) => {
    if (!ev.lengthComputable) return;
    bar.value = 100 * ev.loaded / ev.total;
    msg.textContent = ev.loaded < ev.total ? `uploading… ${Math.round(bar.value)}%` : "verifying…";
  };
  x.onerror = () => fail("upload failed (connection lost)");
  x.onload = () => {
    if (x.status === 401) { fail("password required"); showLogin(); return; }
    if (x.status !== 200) { fail("failed: " + (x.responseText || x.status)); return; }
    let rollback = true; try { rollback = JSON.parse(x.responseText).rollback !== false; } catch(_){}
    msg.textContent = "flashed ✓ — rebooting…" + (rollback ? "" : " (kept without automatic rollback)");
    bar.hidden = true;
    $("#conn").textContent = "rebooting…"; $("#conn").className = "pill";
    // Reload once the new firmware answers, to pick up its copy of this page (a 401
    // answers too: a firmware that wants the password, the login view follows).
    // Other firmware may never answer here, so stop asking after two minutes.
    let tries = 40;
    const t = setInterval(async () => {
      try { const r = await fetch("/api/status", {cache:"no-store"});
        if (r.ok || r.status === 401) { clearInterval(t); location.reload(); return; } } catch(_){}
      if (--tries <= 0) { clearInterval(t); msg.textContent = "flashed ✓ — the device has not come back with this web UI"; }
    }, 3000);
  };
  otaBusy = true; btn.disabled = true; bar.hidden = false; bar.value = 0; msg.textContent = "uploading…";
  x.send(f);
};

// ---- files: read-only copies of the flash regions (/api/parts, /api/res) ----
function human(n){ if(n>=1048576) return (n/1048576).toFixed(1)+" MB"; if(n>=1024) return (n/1024).toFixed(1)+" KB"; return n+" B"; }
const hex = (n, w = 2) => "0x" + n.toString(16).padStart(w, "0");
const esc = s => String(s).replace(/[&<>"']/g, c => "&#" + c.charCodeAt(0) + ";");   // strings read from flash
let PARTS = null, dumping = null;
function kind(p){
  if (p.type < 0) return "";
  if (p.type === 0) return "app/" + (p.sub === 0 ? "factory" : p.sub >= 0x10 && p.sub < 0x20 ? "ota_" + (p.sub - 0x10) : hex(p.sub));
  if (p.type === 1) return "data/" + (["ota", "phy", "nvs", "coredump", "nvs_keys", "efuse"][p.sub] ?? hex(p.sub));
  return hex(p.type) + "/" + hex(p.sub);
}
function what(p){
  if (p.label === "bootloader") return "second-stage bootloader";
  if (p.label === "partition_table") return "partition table";
  if (p.type === 0){
    const app = p.project === "blufi_demo" ? "stock Oclean firmware " + p.version : p.project ? p.project + " " + p.version
      : p.blank ? "blank" : "no ESP-IDF app descriptor";
    return app + (p.running ? " · running" : "") + (p.next ? " · the next firmware update overwrites this" : "");
  }
  if (p.type === 1) return ["which app slot boots", "radio init data", "settings, factory and radio calibration, Wi-Fi and MQTT passwords, web UI password hash and login token"][p.sub] ?? "";
  if (p.type === 0x40) return "OEM pictures, voice clips and brushing records";
  return "";
}
function renderParts(){
  const tb = $("#partTable tbody"); tb.innerHTML = "";
  PARTS.regions.forEach((p, i) => {
    const act = !p.readable ? '<span class="sub">set a web UI password to copy it</span>'
      : `<a data-i="${i}"${dumping || otaBusy ? ' class="off"' : ""}>download</a>`;
    tb.insertAdjacentHTML("beforeend", `<tr><td><b>${esc(p.label)}</b><span class="sub">${[kind(p), hex(p.addr, 6), esc(what(p))].filter(Boolean).join(" · ")}</span></td>
      <td class="nowrap">${human(p.size)}</td><td>${act}</td></tr>`);
  });
  $$("#partTable a[data-i]").forEach(a => a.onclick = () => dump(PARTS.regions[a.dataset.i]));
  $("#partInfo").textContent = `${human(PARTS.flash_size)} flash, flash encryption ${PARTS.encrypted ? "on (the copies are the encrypted bytes)" : "off"}.`;
}
async function parts(){
  if (dumping || otaBusy) return;   // the table is up and showing the copy / the server is busy taking an update
  try { PARTS = await (await api("/api/parts", {cache:"no-store"})).json(); renderParts(); }
  catch(e){ $("#partTable tbody").innerHTML = '<tr><td colspan="3">cannot list the partitions</td></tr>'; }
}
// Fetch the region in 64 KB pieces as dump_res.py does, so the one server task stays
// free for the dashboard in between; a piece that fails or comes back short is asked
// for again, up to 3 times. Cancel aborts the piece in flight and any wait.
async function dump(p){
  if (dumping) return;
  if (otaBusy){ $("#partMsg").textContent = "a firmware update is in progress"; return; }
  const st = dumping = {stop: false, ac: null, wake: null}, bar = $("#partBar"), msg = $("#partMsg"), pieces = [];
  bar.hidden = false; bar.value = 0; $("#partCancel").hidden = false; renderParts();
  try {
    for (let off = 0; off < p.size; ){
      const n = Math.min(65536, p.size - off);
      let buf = null, again401 = true;
      for (let tries = 0; ; tries++){
        let r = null;
        const ac = st.ac = new AbortController(), t = setTimeout(() => ac.abort(), 20000);
        try {
          r = await fetch(`/api/res?part=${encodeURIComponent(p.label)}&off=${off}&len=${n}`, {cache:"no-store", signal:ac.signal});
          if (r.ok){ const b = await r.arrayBuffer(); if (b.byteLength === n) buf = b; }
        } catch(_){} finally { clearTimeout(t); }
        if (st.stop) throw new Error("cancelled");
        // Waited behind a Save that set a new password: the new cookie is in the jar by now (as in api()).
        if (r && r.status === 401 && again401){ again401 = false; tries--; continue; }
        if (r && r.status === 401) showLogin();
        if (r && r.status >= 400 && r.status < 500) throw new Error(await r.text() || "error " + r.status);
        if (buf) break;
        if (tries === 3) throw new Error(`stopped at ${hex(off, 6)}: the brush stopped answering. Try again.`);
        await new Promise(f => { st.wake = f; setTimeout(f, 1000 << tries); });
        if (st.stop) throw new Error("cancelled");
      }
      pieces.push(buf); off += n;
      bar.value = 100 * off / p.size; msg.textContent = `${p.label}: ${human(off)} of ${human(p.size)}`;
    }
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob(pieces, {type:"application/octet-stream"}));
    a.download = `oclean-${p.label}-${hex(p.addr, 6)}.bin`;
    a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 60000);
    msg.textContent = `saved ${a.download} (${human(p.size)})`;
  } catch(e){ msg.textContent = e.message; }
  finally { dumping = null; bar.hidden = true; $("#partCancel").hidden = true; renderParts(); }
}
$("#partCancel").onclick = () => { if (dumping){ dumping.stop = true; dumping.ac?.abort(); dumping.wake?.(); } };

// ---- device log (live tail over /api/log; the only debug channel without serial) ----
let logCursor = 0, logTimer = null;
async function logPoll(){
  if (otaBusy || LOCKED) return;
  try{
    const r = await api("/api/log?since=" + logCursor, {cache:"no-store"});
    const next = r.headers.get("X-Log-Cursor"); if (next !== null) logCursor = +next;
    const t = await r.text();
    if (t){
      const el = $("#logBody");
      el.textContent += t;
      if (el.textContent.length > 240000) el.textContent = el.textContent.slice(-180000);
      if ($("#logFollow").checked) el.scrollTop = el.scrollHeight;
    }
  }catch(e){}
}
function logStart(){ if (!logTimer){ logPoll(); logTimer = setInterval(logPoll, 1500); } }
function logStop(){ if (logTimer){ clearInterval(logTimer); logTimer = null; } }
$("#logLevel").onchange = (e) => { postJson("/api/log", {level: e.target.value}).catch(() => {}); };
$("#logClear").onclick = () => { $("#logBody").textContent = ""; };
