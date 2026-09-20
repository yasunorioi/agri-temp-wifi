// webui.h — always-on HTTP UI, styled to match agri-node-poe-core/AgriWebUI.h.
//
// This node is pinned to arduino-esp32 2.x, so it cannot include agri::WebUI
// (3.x Network* classes, ETH-bound). This is a standalone WebServer-based
// reimplementation that mirrors the family's chrome, navigation, /api/status
// schema and HTTP OTA — same approach as agri-amp-wifi/webui.h.
//
//   GET  /              Dashboard (live refresh of /api/status + /api/dashboard)
//   GET  /config        Config form   POST /config -> NVS -> MQTT reconnect
//   GET  /ota           OTA page      POST /api/ota multipart firmware -> flash
//   GET  /about         About
//   GET  /api/status    JSON liveness (fw, ip, link, mqtt, ccm, probes, slots)
//   GET  /api/config    JSON config
//   GET  /api/dashboard sensor block HTML fragment
//   POST /api/scan      re-enumerate the 1-Wire bus now
//
// PARTIAL-POST SAFETY. The core WebUI has a long-standing footgun: it reads the
// "ccm_en" checkbox unconditionally, so any partial POST that omits it silently
// disables CCM. That has bitten this fleet twice (.27 and .165). Here the form
// carries a hidden marker field per checkbox group, and the checkbox is only
// consulted when its marker is present — so `curl -d "mqhost=..."` changes the
// host and nothing else.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "config.h"
#include "sensors.h"
#include "mqtt_pub.h"
#include "self_update.h"

inline WebServer &web() { static WebServer s(80); return s; }

inline const char *&webFwName() { static const char *n = "agri-temp-wifi"; return n; }
inline const char *&webFwVer()  { static const char *v = "0.0.0";          return v; }

// Minimal escaping for values placed inside single-quoted HTML attributes.
inline String esc(const char *v) {
  String s(v);
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  s.replace("'", "&#39;"); s.replace("\"", "&quot;");
  return s;
}

// ---- page chrome (same theme/nav as AgriWebUI) -----------------------------
inline String pageHead(const char *title) {
  String s; s.reserve(1000);
  s  = F("<!DOCTYPE html><html lang=en><head><meta charset=UTF-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
  s += title;
  s += F("</title><style>"
         "body{font-family:sans-serif;margin:16px;background:#0f1011;color:#f7f8f8}"
         "h2{color:#5e6ad2}h3{color:#d0d6e0}"
         ".sec{background:#191a1b;border-radius:6px;padding:12px;margin:8px 0}"
         "table{border-collapse:collapse;width:100%;margin:6px 0}"
         "th,td{border:1px solid #2e2e2e;padding:5px 8px}"
         "th{background:#191a1b;color:#d0d6e0}"
         "td.rom{font-family:monospace;font-size:12px}"
         ".warn{color:#e8a33d}.bad{color:#e05252}.ok{color:#3fb950}"
         "input,select{padding:5px;background:#1a1a1f;color:#eee;border:1px solid #3e3e44;border-radius:3px}"
         "input[type=submit],button{background:#1976d2;color:#fff;border:none;padding:8px 20px;cursor:pointer;border-radius:3px}"
         "a{color:#d0d6e0}"
         "</style></head><body>");
  s += "<h2>"; s += webFwName(); s += F("</h2>"
         "<p><a href='/'>Dashboard</a> | <a href='/config'>Config</a> | "
         "<a href='/ota'>OTA</a> | <a href='/about'>About</a></p>");
  return s;
}

// Sensor block shared by the dashboard initial render and /api/dashboard.
inline String renderSensors() {
  String s; s.reserve(1600);
  s = F("<h3>Temperature</h3><table>"
        "<tr><th>Slot</th><th>Label</th><th>ROM</th><th>Temp</th><th>Topic</th></tr>");
  bool anySlot = false;
  int  noTopic  = 0;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    if (!sl.rom[0]) continue;
    anySlot = true;
    s += "<tr><td>"; s += i; s += "</td><td>"; s += esc(sl.label);
    s += "</td><td class=rom>"; s += sl.rom; s += "</td><td>";
    if (g_slotOk[i]) { s += "<span class=ok>"; s += String(g_slotTemp[i], 2); s += " &deg;C</span>"; }
    else             { s += F("<span class=bad>-- (no reading)</span>"); }
    s += "</td><td>";
    if (sl.topic[0]) s += esc(sl.topic);
    else { s += F("<span class=warn>not set</span>"); noTopic++; }
    s += "</td></tr>";
  }
  if (!anySlot) s += F("<tr><td colspan=5 class=warn>No probe bound yet — see Config</td></tr>");
  s += F("</table>");

  // A bound probe with no topic is the expected state of a freshly flashed node:
  // there is deliberately no default topic (see config.h). Say so loudly, or the
  // node reads fine on this page while publishing nothing and looks broken.
  if (noTopic) {
    s += F("<p class=warn><b>");
    s += noTopic;
    s += F(" probe(s) have no MQTT topic — nothing is being published.</b><br>"
           "Set one in <a href='/config'>Config</a>. Use a qualified type name "
           "(e.g. <code>agriha/farm/sensor/WaterTempTank</code>), never a bare "
           "type and never an instance number — see mqtt-topics.md &sect;0.3.1.</p>");
  }

  // Probes physically on the bus that no slot claims: the thing you actually
  // want to see after adding or swapping a sensor.
  String un; int nun = 0;
  for (int i = 0; i < g_probeCount; i++) {
    if (g_probe[i].slot >= 0) continue;
    nun++;
    un += "<tr><td class=rom>"; un += g_probe[i].rom; un += "</td><td>";
    if (g_probe[i].ok) { un += String(g_probe[i].temp_c, 2); un += " &deg;C"; }
    else               un += F("<span class=bad>--</span>");
    un += "</td></tr>";
  }
  if (nun) {
    s += F("<h3 class=warn>Unassigned probes on the bus</h3>"
           "<table><tr><th>ROM</th><th>Temp</th></tr>");
    s += un;
    s += F("</table><p class=warn>Bind these to a slot in Config to publish them.</p>");
  }
  s += "<p>Bus: G"; s += g_cfg.ow_pin; s += ", ";
  s += g_probeCount; s += F(" probe(s) found");
  if (!g_busOk) s += F(" <span class=bad>— bus empty: check wiring / 4.7k pull-up</span>");
  s += F("</p>");
  return s;
}

inline String pageDashboard() {
  String s = pageHead("agri-temp-wifi");
  s += F("<div class=sec id=upd style='display:none'></div>");
  s += "<div class=sec id=sens>"; s += renderSensors(); s += "</div>";
  s += F("<div class=sec id=net>Loading…</div>"
         "<script>"
         "async function doUpdate(){"
         "if(!confirm('Flash the new firmware now? The node reboots.'))return;"
         "await fetch('/api/update',{method:'POST'});}"
         "async function tick(){"
         "try{let r=await fetch('/api/status');let d=await r.json();"
         // Semi-automatic self-update banner: visible only when GitHub has a
         // newer release, or while a flash is in flight.
         "let u=document.getElementById('upd'),o=d.ota||{};"
         "if(o.state=='running'){u.style.display='';"
         "u.innerHTML='<h3 class=warn>Updating to v'+(o.latest||'')+' — '+(o.progress||0)+'%</h3>"
         "<p>Do not power off.</p>';}"
         "else if(o.available){u.style.display='';"
         "u.innerHTML='<h3 class=warn>New firmware v'+o.latest+' available</h3>"
         "<p>Running v'+(d.fw_version||'')+'.</p>"
         "<button onclick=doUpdate()>Update now</button>';}"
         "else if(o.state=='failed'){u.style.display='';"
         "u.innerHTML='<h3 class=bad>Update failed</h3><p>'+(o.error||'')+'</p>';}"
         "else{u.style.display='none';}"
         "let n='<h3>Network</h3><table>'"
         "+'<tr><th>Firmware</th><td>'+(d.fw_name||'')+' '+(d.fw_version||'')+'</td></tr>'"
         "+'<tr><th>IP</th><td>'+d.ip+'</td></tr>'"
         "+'<tr><th>Link</th><td>'+d.link+' ('+d.rssi+'dBm)</td></tr>'"
         "+'<tr><th>MQTT</th><td>'+(d.mqtt_connected?'connected':d.mqtt_host?'not connected':'not configured')+'</td></tr>'"
         "+'<tr><th>CCM</th><td>'+(d.ccm_enabled?'enabled':'disabled')+'</td></tr>'"
         "+'<tr><th>Uptime</th><td>'+d.uptime_s+' s</td></tr></table>';"
         "document.getElementById('net').innerHTML=n;}catch(e){}"
         "try{let f=await fetch('/api/dashboard');"
         "if(f.ok)document.getElementById('sens').innerHTML=await f.text();}catch(e){}"
         "}tick();setInterval(tick,3000);"
         "</script></body></html>");
  return s;
}

// `ph` renders as a placeholder: grey sample text that shows the shape of a
// valid value, vanishes as soon as the operator types, and is NEVER submitted —
// so an empty field stays empty. Used for the slot topics, which have no
// default any more (config.h) and would otherwise be a blank box with no clue
// what belongs in it.
inline String webIn(const char *name, const String &val, const char *type = "text",
                    const char *ph = nullptr) {
  String s = "<input type=" + String(type) + " name='" + String(name) +
             "' value='" + val + "'";
  if (ph && *ph) { s += " placeholder='"; s += ph; s += "'"; }
  return s + ">";
}

// <select> of every ROM currently on the bus, plus whatever this slot already
// holds (so a temporarily unplugged probe is not lost by opening the page).
inline String romSelect(int i) {
  const char *cur = g_cfg.slot[i].rom;
  String s = "<select name='rom" + String(i) + "'>";
  s += "<option value=''"; if (!cur[0]) s += " selected"; s += ">(none)</option>";
  bool curListed = false;
  for (int k = 0; k < g_probeCount; k++) {
    const char *r = g_probe[k].rom;
    bool sel = (cur[0] && strcasecmp(cur, r) == 0);
    if (sel) curListed = true;
    s += "<option value='"; s += r; s += "'"; if (sel) s += " selected"; s += ">";
    s += r;
    if (g_probe[k].ok) { s += "  ("; s += String(g_probe[k].temp_c, 1); s += "C)"; }
    int owner = g_probe[k].slot;
    if (owner >= 0 && owner != i) { s += "  [slot "; s += owner; s += "]"; }
    s += "</option>";
  }
  if (cur[0] && !curListed) {
    s += "<option value='"; s += cur; s += "' selected>"; s += cur; s += "  (offline)</option>";
  }
  s += "</select>";
  return s;
}

inline String pageConfig() {
  String s = pageHead("Config");
  auto row = [&](const char *label, const String &input) {
    s += "<tr><th>"; s += label; s += "</th><td>"; s += input; s += "</td></tr>";
  };
  s += F("<div class=sec><form method=POST action='/config'>"
         "<input type=hidden name=form value=1>"
         "<h3>1-Wire bus</h3><table>");
  row("DATA pin (GPIO, reboots on change)", webIn("owpin", String(g_cfg.ow_pin), "number"));
  row("Resolution (9-12 bit)",              webIn("owres", String(g_cfg.resolution), "number"));
  row("Measure interval (s)",               webIn("msint", String(g_cfg.meas_interval_s), "number"));
  s += F("</table><h3>MQTT</h3><table>");
  // node_id is the MQTT client id AND the <prefix>/sys/<id>/online LWT topic.
  // Both are re-sent on the reconnect that webApplyForm() forces, so unlike the
  // hostname this takes effect without a reboot. 15 chars max (char[16]).
  row("Node ID (MQTT client id + sys topic, max 15)",
                                webIn("nodeid", esc(g_cfg.node_id)));
  row("Hostname (mDNS .local, needs a reboot)",
                                webIn("host",   esc(g_cfg.hostname)));
  row("MQTT Host",              webIn("mqhost", esc(g_cfg.mqtt_host)));
  row("MQTT Port",              webIn("mqport", String(g_cfg.mqtt_port), "number"));
  row("MQTT User",              webIn("mquser", esc(g_cfg.mqtt_user)));
  row("MQTT Pass",              webIn("mqpass", esc(g_cfg.mqtt_pass), "password"));
  row("sys/LWT prefix",         webIn("prefix", esc(g_cfg.sys_prefix)));
  row("MQTT interval (s)",      webIn("mqint",  String(g_cfg.mqtt_interval_s), "number"));
  s += F("</table><h3>UECS-CCM</h3><table>");
  row("CCM enabled", String("<input type=hidden name=ccmform value=1>"
                            "<input type=checkbox name=ccmen ") + (g_cfg.ccm_enabled ? "checked" : "") + ">");
  row("Node type (ノード種別)", webIn("cnt",    esc(g_cfg.ccm_ntype)));
  row("CCM interval (s)",       webIn("ccmint", String(g_cfg.ccm_interval_s), "number"));
  row("CCM priority",           webIn("cpri",   String(g_cfg.ccm_priority), "number"));
  s += F("</table>"
         "<h3>Slots</h3>"
         "<p>Bind a probe (ROM address) to a slot. Empty ROM = slot inactive. "
         "Empty Topic = <b>not published</b> — a slot with no topic is silent on "
         "purpose. Empty CCM identifier = no CCM for that slot.</p>"
         // There is deliberately no default topic, so the field starts blank.
         // A blank box with no clue is its own trap, hence this worked example
         // block outside the form: the real names currently in the fleet.
         "<p class=warn>MQTT topic has no default — name it when you install the "
         "node. Shape: <code>agriha/&lt;scope&gt;/&lt;category&gt;/"
         "&lt;Type&gt;&lt;descriptor&gt;</code></p>"
         "<table><tr><th>Example (in use today)</th><th>What it is</th></tr>"
         "<tr><td class=rom>agriha/farm/sensor/WaterTempTank</td>"
             "<td>supply tank shared by house 2 + 3</td></tr>"
         "<tr><td class=rom>agriha/1/sensor/WaterTempTap</td><td>house 1, tap</td></tr>"
         "<tr><td class=rom>agriha/2/sensor/WaterTempNear</td><td>house 2, near side</td></tr>"
         "<tr><td class=rom>agriha/2/sensor/WaterTempPump</td><td>house 2, pump</td></tr>"
         "<tr><td class=rom>agriha/3/sensor/WaterTempFar</td><td>house 3, far side</td></tr>"
         "</table>"
         "<p><code>scope</code> = the house number, or <code>farm</code> when two or "
         "more houses share the thing being measured. Always add a descriptor: "
         "never the bare type (<code>WaterTemp</code>) and never an instance number "
         "(<code>WaterTemp/2</code>) — see mqtt-topics.md &sect;0.3.1.</p>"
         "<table><tr><th>#</th><th>ROM</th><th>Label</th><th>MQTT topic</th>"
         "<th>CCM identifier</th><th>room</th><th>region</th><th>order</th><th>offset &deg;C</th></tr>");
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    s += "<tr><td>"; s += i; s += "</td><td>"; s += romSelect(i); s += "</td>";
    s += "<td>" + webIn(("lab" + String(i)).c_str(), esc(sl.label))    + "</td>";
    s += "<td>" + webIn(("top" + String(i)).c_str(), esc(sl.topic), "text",
                        "agriha/farm/sensor/WaterTempTank")               + "</td>";
    s += "<td>" + webIn(("typ" + String(i)).c_str(), esc(sl.ccm_type)) + "</td>";
    s += "<td>" + webIn(("rm"  + String(i)).c_str(), String(sl.ccm_room),   "number") + "</td>";
    s += "<td>" + webIn(("rg"  + String(i)).c_str(), String(sl.ccm_region), "number") + "</td>";
    s += "<td>" + webIn(("or"  + String(i)).c_str(), String(sl.ccm_order),  "number") + "</td>";
    s += "<td>" + webIn(("off" + String(i)).c_str(), String(sl.offset_c, 2)) + "</td></tr>";
  }
  s += F("</table><p><input type=submit value='Save'></p></form>"
         "<p><button onclick=\"fetch('/api/scan',{method:'POST'}).then(()=>location.reload())\">"
         "Rescan bus</button> "
         "<button onclick=\"if(confirm('Reboot the node now? It will be offline for ~10 s.'))"
         "fetch('/api/reboot',{method:'POST'}).then(()=>{document.body.textContent="
         "'rebooting - reconnect in ~10 s'});\">Reboot</button></p>"
         "<p>Hostname changes need a reboot; Node ID and the MQTT settings do not.</p>"
         "</div></body></html>");
  return s;
}

inline String pageOta() {
  String s = pageHead("OTA");
  // GitHub release self-update (semi-automatic: checks, never flashes by itself)
  s += F("<div class=sec><h3>GitHub release</h3><table>");
  s += "<tr><th>Running</th><td>";  s += webFwVer(); s += "</td></tr>";
  s += "<tr><th>Latest</th><td>";
  s += agri::OTA::latestVersion.length() ? agri::OTA::latestVersion : String("(not checked)");
  s += "</td></tr>";
  s += "<tr><th>State</th><td>"; s += agri::OTA::stateString();
  if (agri::OTA::error.length()) { s += " — "; s += esc(agri::OTA::error.c_str()); }
  s += F("</td></tr></table>"
         "<p><button onclick=\"fetch('/api/check',{method:'POST'}).then(()=>location.reload())\">"
         "Check now</button> ");
  if (agri::OTA::updateAvailable) {
    s += F("<button onclick=\"fetch('/api/update',{method:'POST'}).then(()=>"
           "alert('flashing — the node reboots when done'))\">Update now</button>");
  }
  s += F("</p></div>");

  s += F("<div class=sec><h3>Manual upload</h3>"
         "<p>Pick a <code>firmware.bin</code> and upload. The node reboots after a "
         "verified flash. Do not power off during the update.</p>"
         "<input type=file id=f accept='.bin'> <button onclick='up()'>Update</button>"
         "<p id=st></p>"
         "<script>"
         "async function up(){var st=document.getElementById('st');"
         "var f=document.getElementById('f').files[0];if(!f){st.textContent='choose a .bin first';return;}"
         "st.textContent='Uploading '+f.size+' bytes...';"
         "var fd=new FormData();fd.append('firmware',f,'firmware.bin');"
         "try{var r=await fetch('/api/ota',{method:'POST',body:fd});st.textContent=await r.text();}"
         "catch(e){st.textContent='done / device rebooting — reconnect in ~10s';}}"
         "</script></div></body></html>");
  return s;
}

inline String pageAbout() {
  String s = pageHead("About");
  s += F("<div class=sec><h3>About</h3><table>");
  s += "<tr><th>Firmware</th><td>"; s += webFwName(); s += " "; s += webFwVer(); s += "</td></tr>";
  s += "<tr><th>Node ID</th><td>";  s += g_cfg.node_id; s += "</td></tr>";
  s += "<tr><th>Hostname</th><td>"; s += g_cfg.hostname; s += ".local</td></tr>";
  s += "<tr><th>MAC</th><td>";      s += WiFi.macAddress(); s += "</td></tr>";
  s += "<tr><th>IP</th><td>";       s += WiFi.localIP().toString(); s += "</td></tr>";
  s += "<tr><th>Uptime</th><td>";   s += String(millis() / 1000); s += " s</td></tr>";
  s += F("</table></div></body></html>");
  return s;
}

// ---- form apply ------------------------------------------------------------
// Every field is optional: an absent key leaves the current value alone, so a
// partial POST is safe. Checkboxes are guarded by their marker field.
inline bool webApplyForm() {
  WebServer &w = web();
  auto str = [&](const char *k, char *dst, size_t n) {
    if (w.hasArg(k)) strlcpy(dst, w.arg(k).c_str(), n);
  };
  uint8_t oldPin = g_cfg.ow_pin;

  if (w.hasArg("owpin")) g_cfg.ow_pin = (uint8_t)w.arg("owpin").toInt();
  if (w.hasArg("owres")) {
    int r = w.arg("owres").toInt();
    g_cfg.resolution = (uint8_t)constrain(r, 9, 12);
  }
  if (w.hasArg("msint")) g_cfg.meas_interval_s = (uint16_t)max(1L, w.arg("msint").toInt());

  str("nodeid", g_cfg.node_id,    sizeof(g_cfg.node_id));
  str("host",   g_cfg.hostname,   sizeof(g_cfg.hostname));
  str("mqhost", g_cfg.mqtt_host,  sizeof(g_cfg.mqtt_host));
  if (w.hasArg("mqport")) g_cfg.mqtt_port = (uint16_t)w.arg("mqport").toInt();
  str("mquser", g_cfg.mqtt_user,  sizeof(g_cfg.mqtt_user));
  str("mqpass", g_cfg.mqtt_pass,  sizeof(g_cfg.mqtt_pass));
  str("prefix", g_cfg.sys_prefix, sizeof(g_cfg.sys_prefix));
  if (w.hasArg("mqint")) g_cfg.mqtt_interval_s = (uint16_t)w.arg("mqint").toInt();

  if (w.hasArg("ccmform")) g_cfg.ccm_enabled = w.hasArg("ccmen");
  str("cnt", g_cfg.ccm_ntype, sizeof(g_cfg.ccm_ntype));
  if (w.hasArg("ccmint")) g_cfg.ccm_interval_s = (uint16_t)w.arg("ccmint").toInt();
  if (w.hasArg("cpri"))   g_cfg.ccm_priority   = (int16_t) w.arg("cpri").toInt();

  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &sl = g_cfg.slot[i];
    String k;
    k = "rom" + String(i); if (w.hasArg(k.c_str())) strlcpy(sl.rom,      w.arg(k.c_str()).c_str(), sizeof(sl.rom));
    k = "lab" + String(i); if (w.hasArg(k.c_str())) strlcpy(sl.label,    w.arg(k.c_str()).c_str(), sizeof(sl.label));
    k = "top" + String(i); if (w.hasArg(k.c_str())) strlcpy(sl.topic,    w.arg(k.c_str()).c_str(), sizeof(sl.topic));
    k = "typ" + String(i); if (w.hasArg(k.c_str())) strlcpy(sl.ccm_type, w.arg(k.c_str()).c_str(), sizeof(sl.ccm_type));
    k = "rm"  + String(i); if (w.hasArg(k.c_str())) sl.ccm_room   = (int16_t)w.arg(k.c_str()).toInt();
    k = "rg"  + String(i); if (w.hasArg(k.c_str())) sl.ccm_region = (int16_t)w.arg(k.c_str()).toInt();
    k = "or"  + String(i); if (w.hasArg(k.c_str())) sl.ccm_order  = (int16_t)w.arg(k.c_str()).toInt();
    k = "off" + String(i); if (w.hasArg(k.c_str())) sl.offset_c   = w.arg(k.c_str()).toFloat();
  }

  saveConfig();
  mqttClient().disconnect();     // force reconnect with the new host/topics
  sensorsScan();                 // re-resolve ROM -> slot bindings immediately
  return g_cfg.ow_pin != oldPin; // caller reboots: the OneWire bus is pin-bound
}

// ---- JSON ------------------------------------------------------------------
inline void webConfigJson(String &out) {
  JsonDocument doc;
  JsonObject r = doc.to<JsonObject>();
  r["node_id"]  = g_cfg.node_id;
  r["hostname"] = g_cfg.hostname;
  JsonObject ow = r["onewire"].to<JsonObject>();
  ow["pin"] = g_cfg.ow_pin; ow["resolution"] = g_cfg.resolution;
  ow["interval_s"] = g_cfg.meas_interval_s;
  JsonObject m = r["mqtt"].to<JsonObject>();
  m["host"] = g_cfg.mqtt_host; m["port"] = g_cfg.mqtt_port;
  m["prefix"] = g_cfg.sys_prefix; m["interval_s"] = g_cfg.mqtt_interval_s;
  JsonObject c = r["ccm"].to<JsonObject>();
  c["enabled"] = g_cfg.ccm_enabled; c["ntype"] = g_cfg.ccm_ntype;
  c["interval_s"] = g_cfg.ccm_interval_s; c["priority"] = g_cfg.ccm_priority;
  JsonArray sl = r["slots"].to<JsonArray>();
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &s = g_cfg.slot[i];
    if (!s.rom[0]) continue;
    JsonObject o = sl.add<JsonObject>();
    o["slot"] = i; o["rom"] = s.rom; o["label"] = s.label; o["topic"] = s.topic;
    o["ccm_type"] = s.ccm_type; o["room"] = s.ccm_room;
    o["region"] = s.ccm_region; o["order"] = s.ccm_order; o["offset_c"] = s.offset_c;
  }
  serializeJson(doc, out);
}

inline void webStatusJson(String &out) {
  JsonDocument doc;
  JsonObject r = doc.to<JsonObject>();
  r["fw_name"]        = webFwName();
  r["fw_version"]     = webFwVer();
  r["uptime_s"]       = millis() / 1000;
  r["ip"]             = WiFi.localIP().toString();
  r["rssi"]           = WiFi.RSSI();
  r["link"]           = (WiFi.status() == WL_CONNECTED) ? "up" : "down";
  r["mqtt_host"]      = g_cfg.mqtt_host;
  r["mqtt_connected"] = mqttConnected();
  r["ccm_enabled"]    = g_cfg.ccm_enabled;
  r["ow_pin"]         = g_cfg.ow_pin;
  r["probe_count"]    = g_probeCount;

  JsonArray ps = r["probes"].to<JsonArray>();
  for (int i = 0; i < g_probeCount; i++) {
    JsonObject o = ps.add<JsonObject>();
    o["rom"]  = g_probe[i].rom;
    o["slot"] = g_probe[i].slot;
    if (g_probe[i].ok) o["temp_c"] = g_probe[i].temp_c; else o["temp_c"] = nullptr;
  }
  JsonArray ss = r["slots"].to<JsonArray>();
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    if (!g_cfg.slot[i].rom[0]) continue;
    JsonObject o = ss.add<JsonObject>();
    o["slot"] = i; o["label"] = g_cfg.slot[i].label; o["topic"] = g_cfg.slot[i].topic;
    if (g_slotOk[i]) o["temp_c"] = g_slotTemp[i]; else o["temp_c"] = nullptr;
  }
  JsonObject ota = r["ota"].to<JsonObject>();
  ota["state"]     = agri::OTA::stateString();
  ota["progress"]  = agri::OTA::progress;
  ota["available"] = agri::OTA::updateAvailable;
  ota["latest"]    = agri::OTA::latestVersion;
  ota["error"]     = agri::OTA::error;
  serializeJson(doc, out);
}

// ---- HTTP OTA (multipart firmware upload, WebServer 2.x style) --------------
inline void onOtaUpload() {
  HTTPUpload &up = web().upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] upload %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.printf("[OTA] %u bytes OK\n", (unsigned)up.totalSize);
    else                  Update.printError(Serial);
  }
}

inline void webBegin(const char *fw_name, const char *fw_version) {
  webFwName() = fw_name;
  webFwVer()  = fw_version;
  WebServer &w = web();
  w.on("/",       HTTP_GET,  [] { web().send(200, "text/html", pageDashboard()); });
  w.on("/config", HTTP_GET,  [] { web().send(200, "text/html", pageConfig()); });
  w.on("/config", HTTP_POST, [] {
    bool reboot = webApplyForm();
    web().sendHeader("Location", "/config");
    web().send(303, "text/plain", reboot ? "saved — rebooting for new 1-Wire pin\n" : "saved");
    if (reboot) { delay(300); ESP.restart(); }
  });
  w.on("/ota",   HTTP_GET, [] { web().send(200, "text/html", pageOta()); });
  w.on("/about", HTTP_GET, [] { web().send(200, "text/html", pageAbout()); });
  w.on("/api/ota", HTTP_POST,
       [] {
         bool ok = !Update.hasError();
         web().send(200, "text/plain", ok ? "OK — flashed, rebooting\n" : "OTA failed\n");
         delay(300);
         if (ok) ESP.restart();
       },
       onOtaUpload);
  // Manual reboot. mDNS and ArduinoOTA bind the hostname once in setup(), so a
  // hostname change only takes effect after a restart — and until now the only
  // way to get one remotely was to re-flash the same image over /api/ota.
  // POST-only on purpose: a GET would let a link prefetch or a crawler reboot
  // the node.
  w.on("/api/reboot", HTTP_POST, [] {
    Serial.println("[WEB] reboot requested");
    web().send(200, "text/plain", "rebooting\n");
    delay(300);            // let the response reach the client before EN drops
    ESP.restart();
  });
  w.on("/api/scan", HTTP_POST, [] {
    sensorsScan();
    String s; webStatusJson(s);
    web().send(200, "application/json", s);
  });
  // Self-update: /api/check re-polls GitHub now, /api/update arms the flash.
  // The flash itself happens in loop() via agri::OTA::poll(), so the HTTP
  // response is sent before the device goes busy writing the OTA partition.
  w.on("/api/check", HTTP_POST, [] {
    agri::OTA::checkLatest();
    String s; webStatusJson(s);
    web().send(200, "application/json", s);
  });
  w.on("/api/update", HTTP_POST, [] {
    if (!agri::OTA::updateAvailable) {
      web().send(409, "text/plain", "no update available\n");
      return;
    }
    agri::OTA::schedule();
    web().send(200, "text/plain", "scheduled — flashing, the node will reboot\n");
  });
  w.on("/api/status",    HTTP_GET, [] { String s; webStatusJson(s); web().send(200, "application/json", s); });
  w.on("/api/config",    HTTP_GET, [] { String s; webConfigJson(s); web().send(200, "application/json", s); });
  w.on("/api/dashboard", HTTP_GET, [] { web().send(200, "text/html", renderSensors()); });
  w.begin();
  Serial.println("[WEB] http server on :80");
}

inline void webHandle() { web().handleClient(); }
