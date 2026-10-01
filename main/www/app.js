"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

// ---- tabs ----
$$("nav button").forEach(b => b.onclick = () => {
  $$("nav button").forEach(x => x.classList.toggle("active", x === b));
  $$(".tab").forEach(t => t.classList.toggle("active", t.id === b.dataset.tab));
  if (b.dataset.tab === "files") browse(FS_ROOT);
  if (b.dataset.tab === "logs") logStart(); else logStop();
});

// ---- dashboard / metrics ----
const LABELS = {
  battery:"Battery", charging:"Charging", brushing:"Brushing", mode:"Mode",
  last_session_secs:"Last Session", last_session_time:"Last Session Time",
  brush_score:"Brush Score", total_sessions:"Total Sessions", brush_head_days:"Brush Head Age",
  fw_version:"Firmware", uptime:"Uptime", free_heap:"Free Heap", min_free_heap:"Min Heap",
  largest_block:"Largest Block", task_count:"Tasks", cpu_temp:"CPU Temp", reset_reason:"Reset Reason",
  idf_version:"ESP-IDF", app_version:"App Version", flash_size:"Flash", wifi_rssi:"Wi-Fi RSSI",
  wifi_ssid:"SSID", wifi_ip:"IP", wifi_channel:"Channel", mac:"MAC"
};
const UNITS = { battery:"%", last_session_secs:"s", brush_head_days:"d", uptime:"s",
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
  if (otaBusy) return;
  try{
    const r = await fetch("/api/status"); const j = await r.json();
    PROJECT = j.project ?? null;
    $("#conn").textContent = (j.wifi_connected?"Wi-Fi":"AP") + (j.mqtt_connected?" · MQTT":"") + (j.ble_connected?" · BLE":"");
    $("#conn").className = "pill " + ((j.wifi_connected&&j.mqtt_connected)?"ok":(j.wifi_connected?"":"bad"));
    $("#fwVer").value = j.metrics.app_version ?? "";
    const g = $("#metrics"); g.innerHTML = "";
    for (const [k,v] of Object.entries(j.metrics)){
      const unit = UNITS[k] ? `<span class="u">${UNITS[k]==="B"||["KB","MB"].some(x=>String(fmt(k,v)).includes(x))?"":UNITS[k]}</span>`:"";
      const d = document.createElement("div"); d.className="metric";
      d.innerHTML = `<div class="k">${LABELS[k]||k}</div><div class="v">${fmt(k,v)}${k in UNITS && !String(fmt(k,v)).match(/[A-Za-z]$/)?' <span class="u">'+UNITS[k]+'</span>':''}</div>`;
      g.appendChild(d);
    }
  }catch(e){ $("#conn").textContent="offline"; $("#conn").className="pill bad"; }
}
setInterval(refresh, 5000); refresh();

// ---- settings ----
async function loadCfg(){
  const c = await (await fetch("/api/config")).json();
  const f = $("#cfgForm");
  for (const [k,v] of Object.entries(c)){
    const el = f.elements[k]; if (!el) continue;
    if (el.type === "checkbox") el.checked = !!v; else el.value = v ?? "";
  }
}
$("#cfgForm").onsubmit = async (e) => {
  e.preventDefault();
  const f = e.target, body = {};
  for (const el of f.elements){
    if (!el.name) continue;
    if (el.type === "checkbox") body[el.name] = el.checked;
    else if (el.type === "number") body[el.name] = Number(el.value);
    else if (el.value !== "" || !["mqtt_pass","wifi_pass"].includes(el.name)) body[el.name] = el.value;
  }
  $("#saveMsg").textContent = "saving…";
  const r = await fetch("/api/config", {method:"POST", headers:{"Content-Type":"application/json"}, body:JSON.stringify(body)});
  const j = await r.json();
  $("#saveMsg").textContent = j.saved ? "saved ✓ (applied live)" : "save failed";
  f.elements["mqtt_pass"].value = ""; f.elements["wifi_pass"].value = "";
};
$("#rebootBtn").onclick = async () => { if(confirm("Reboot device?")){ await fetch("/api/reboot",{method:"POST"}); $("#saveMsg").textContent="rebooting…"; } };
loadCfg();

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
    if (x.status !== 200) { fail("failed: " + (x.responseText || x.status)); return; }
    let rollback = true; try { rollback = JSON.parse(x.responseText).rollback !== false; } catch(_){}
    msg.textContent = "flashed ✓ — rebooting…" + (rollback ? "" : " (kept without automatic rollback)");
    bar.hidden = true;
    $("#conn").textContent = "rebooting…"; $("#conn").className = "pill";
    // Reload once the new firmware answers, to pick up its copy of this page.
    // Other firmware may never answer here, so stop asking after two minutes.
    let tries = 40;
    const t = setInterval(async () => {
      try { if ((await fetch("/api/status", {cache:"no-store"})).ok) { clearInterval(t); location.reload(); return; } } catch(_){}
      if (--tries <= 0) { clearInterval(t); msg.textContent = "flashed ✓ — the device has not come back with this web UI"; }
    }, 3000);
  };
  otaBusy = true; btn.disabled = true; bar.hidden = false; bar.value = 0; msg.textContent = "uploading…";
  x.send(f);
};

// ---- file browser (read-only) ----
let FS_ROOT = "/data";
function human(n){ if(n>=1048576) return (n/1048576).toFixed(1)+" MB"; if(n>=1024) return (n/1024).toFixed(1)+" KB"; return n+" B"; }
async function browse(path){
  const r = await fetch("/api/fs/list?path=" + encodeURIComponent(path));
  if(!r.ok){ $("#fsTable tbody").innerHTML = `<tr><td colspan=4>cannot open ${path}</td></tr>`; return; }
  const j = await r.json(); FS_ROOT = j.path.startsWith("/data") ? "/data" : FS_ROOT;
  // breadcrumbs
  const parts = j.path.split("/").filter(Boolean); let acc="";
  const crumbs = ['<a data-p="/data">root</a>'];
  for (const p of parts){ if(p==="data"){continue;} acc += "/"+p; crumbs.push(`<a data-p="/data${acc}">${p}</a>`); }
  $("#crumbs").innerHTML = crumbs.join(" / ");
  $$("#crumbs a").forEach(a => a.onclick = () => browse(a.dataset.p));
  // rows
  const tb = $("#fsTable tbody"); tb.innerHTML = "";
  j.entries.sort((a,b)=> (a.type===b.type)? a.name.localeCompare(b.name) : (a.type==="dir"?-1:1));
  if(j.path !== "/data"){
    const up = j.path.replace(/\/[^/]+$/,"") || "/data";
    tb.insertAdjacentHTML("beforeend", `<tr><td class="name-dir"><a data-dir="${up}">..</a></td><td></td><td></td><td></td></tr>`);
  }
  for (const e of j.entries){
    const full = j.path.replace(/\/$/,"") + "/" + e.name;
    const when = e.mtime ? new Date(e.mtime*1000).toLocaleString() : "";
    if (e.type === "dir"){
      tb.insertAdjacentHTML("beforeend", `<tr><td class="name-dir"><a data-dir="${full}">${e.name}</a></td><td></td><td>${when}</td><td></td></tr>`);
    } else {
      const u = encodeURIComponent(full);
      tb.insertAdjacentHTML("beforeend",
        `<tr><td class="name-file">${e.name}</td><td>${human(e.size)}</td><td>${when}</td>
         <td class="actions"><a data-view="${u}">view</a><a href="/api/fs/download?path=${u}">download</a></td></tr>`);
    }
  }
  $$("#fsTable a[data-dir]").forEach(a => a.onclick = () => browse(a.dataset.dir));
  $$("#fsTable a[data-view]").forEach(a => a.onclick = () => view(a.dataset.view, a.closest("tr").querySelector(".name-file").textContent));
}
async function view(u, name){
  const r = await fetch("/api/fs/view?path=" + u);
  const txt = await r.text();
  $("#viewerName").textContent = name;
  $("#viewerBody").textContent = txt.length > 200000 ? txt.slice(0,200000)+"\n…(truncated)" : txt;
  $("#viewer").classList.remove("hidden");
}
$("#viewerClose").onclick = () => $("#viewer").classList.add("hidden");

// ---- device log (live tail over /api/log; the only debug channel without serial) ----
let logCursor = 0, logTimer = null;
async function logPoll(){
  if (otaBusy) return;
  try{
    const r = await fetch("/api/log?since=" + logCursor, {cache:"no-store"});
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
$("#logLevel").onchange = (e) => { fetch("/api/log?level=" + encodeURIComponent(e.target.value), {cache:"no-store"}); };
$("#logClear").onclick = () => { $("#logBody").textContent = ""; };
