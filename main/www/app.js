"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

// ---- tabs ----
$$("nav button").forEach(b => b.onclick = () => {
  $$("nav button").forEach(x => x.classList.toggle("active", x === b));
  $$(".tab").forEach(t => t.classList.toggle("active", t.id === b.dataset.tab));
  if (b.dataset.tab === "files") browse(FS_ROOT);
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
async function refresh(){
  try{
    const r = await fetch("/api/status"); const j = await r.json();
    $("#conn").textContent = (j.wifi_connected?"Wi-Fi":"AP") + (j.mqtt_connected?" · MQTT":"") + (j.ble_connected?" · BLE":"");
    $("#conn").className = "pill " + ((j.mqtt_connected&&j.ble_connected)?"ok":(j.wifi_connected?"":"bad"));
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
