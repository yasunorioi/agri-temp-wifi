// config.h — agri-temp-wifi NVS-backed config ("temp-cfg").
//
// WiFi member of the agri-* family (ATOM U has no PoE base). Provisioning of
// the WiFi credentials is WiFiManager's captive portal; everything below lives
// in the always-on WebUI and is persisted in NVS.
//
// SLOT MODEL
// ----------
// A 1-Wire bus carries N DS18B20s, each with a unique 64-bit ROM address. The
// bus enumerates in ROM order, which is arbitrary and NOT the order you wired
// them in — so an index-based mapping would silently swap probes whenever one
// is replaced. Instead the config binds a ROM address to a *slot*, and the slot
// owns the MQTT topic / UECS type / calibration. Swap a probe, rebind one field
// in /config, and every downstream series keeps its meaning.
//
// A slot with an empty rom is inactive. A slot with an empty topic is not
// published to MQTT; a slot with an empty ccm_type is not sent as CCM. That
// mirrors the family convention established in agri-env-poe v0.10.0 (empty
// CCM identifier = that datum is disabled).

#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Default 1-Wire DATA pin, set per board in platformio.ini: G25 on the ATOM U
// (hand-wired), G1 on the AtomS3 Lite (Grove white). Only the value a blank
// NVS starts from — /config overrides it at runtime. See src/board.h.
#ifndef OW_DEFAULT_PIN
#define OW_DEFAULT_PIN 25
#endif

static const int CFG_MAX_SLOTS = 8;

struct SlotConfig {
  char    rom[17];        // 16 uppercase hex chars, "" = unbound/inactive
  char    label[16];      // human label shown in the WebUI (e.g. "供給")
  char    topic[64];      // MQTT topic, "" = do not publish
  char    ccm_type[24];   // UECS type name (CCM identifier), "" = no CCM
  int16_t ccm_room;
  int16_t ccm_region;     // maps to a house on the yasu-hp bridge — see README
  int16_t ccm_order;
  float   offset_c;       // per-probe calibration offset added to the reading
};

struct AppConfig {
  // identity
  char     node_id[16];
  char     hostname[32];

  // 1-Wire bus
  uint8_t  ow_pin;                  // DATA pin (board default: OW_DEFAULT_PIN)
  uint8_t  resolution;              // 9..12 bits (12 = 0.0625 C, 750 ms)
  uint16_t meas_interval_s;         // bus poll cadence

  // MQTT (empty host disables MQTT)
  char     mqtt_host[64];
  uint16_t mqtt_port;
  char     mqtt_user[32];
  char     mqtt_pass[32];
  char     sys_prefix[64];          // LWT/sys scope: <prefix>/sys/<id>/online
  uint16_t mqtt_interval_s;

  // UECS-CCM envelope (default OFF — see the region caveat in the README)
  bool     ccm_enabled;
  uint16_t ccm_interval_s;
  int16_t  ccm_priority;
  char     ccm_ntype[8];            // "<type>.<ntype>" suffix (ArSprout node type)

  SlotConfig slot[CFG_MAX_SLOTS];
};

extern AppConfig g_cfg;

// NVS key for slot i, e.g. key("rom", 3) -> "s3rom". Keys must stay <= 15 chars.
inline const char *slotKey(const char *field, int i) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "s%d%s", i, field);
  return buf;
}

inline void setDefaults() {
  strlcpy(g_cfg.node_id,  "temp_node_01", sizeof(g_cfg.node_id));
  strlcpy(g_cfg.hostname, "agri-temp-01", sizeof(g_cfg.hostname));

  g_cfg.ow_pin          = OW_DEFAULT_PIN;   // G25 on ATOM U, G1 (Grove) on AtomS3
  g_cfg.resolution      = 12;
  g_cfg.meas_interval_s = 10;

  strlcpy(g_cfg.mqtt_host, "yasu-hp.local", sizeof(g_cfg.mqtt_host));
  g_cfg.mqtt_port = 1883;
  strlcpy(g_cfg.mqtt_user, "", sizeof(g_cfg.mqtt_user));
  strlcpy(g_cfg.mqtt_pass, "", sizeof(g_cfg.mqtt_pass));
  strlcpy(g_cfg.sys_prefix, "agriha/2", sizeof(g_cfg.sys_prefix));
  g_cfg.mqtt_interval_s = 30;

  g_cfg.ccm_enabled    = false;     // family convention: off until region confirmed
  g_cfg.ccm_interval_s = 30;
  g_cfg.ccm_priority   = 29;
  strlcpy(g_cfg.ccm_ntype, "cMC", sizeof(g_cfg.ccm_ntype));

  // NO DEFAULT TOPIC — on purpose. An empty topic means "do not publish".
  //
  // This used to default to agriha/2/sensor/WaterTemp (and .../WaterTemp/N for
  // the rest), which made every freshly flashed node land on the same generic
  // name. That name is not a routing choice, it is the family's "not configured
  // yet" placeholder: every temp node in the fleet passed through it and then
  // moved to a qualified type name (WaterTempTap / Near / Pump / Far / Tank).
  // Nodes that lingered there wrote into each other's history series — one
  // series ended up holding samples from two different physical probes, and the
  // whole set (566 samples across four series) had to be deleted by hand on
  // 2026-09-20.
  //
  // Publishing nothing until an operator names the topic costs one commissioning
  // step and makes that class of collision impossible. The dashboard flags any
  // bound probe that has no topic, so a silent node is visibly silent on purpose
  // rather than mysteriously absent from the broker.
  //
  // Naming rules: mqtt-topics.md 0.3.1 (qualified type name, never a bare type,
  // never an instance number) and 0.6 (topic lifecycle).
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &s = g_cfg.slot[i];
    s.rom[0] = '\0';
    snprintf(s.label, sizeof(s.label), "probe%d", i + 1);
    s.topic[0] = '\0';                 // = not published until configured
    strlcpy(s.ccm_type, "WaterTemp", sizeof(s.ccm_type));
    s.ccm_room   = 1;
    s.ccm_region = 13;
    s.ccm_order  = i + 1;
    s.offset_c   = 0.0f;
  }
}

inline void loadConfig() {
  setDefaults();
  Preferences p;
  if (!p.begin("temp-cfg", true)) return;
  String s;
  s = p.getString("node_id",  g_cfg.node_id);   strlcpy(g_cfg.node_id,  s.c_str(), sizeof(g_cfg.node_id));
  s = p.getString("hostname", g_cfg.hostname);  strlcpy(g_cfg.hostname, s.c_str(), sizeof(g_cfg.hostname));

  g_cfg.ow_pin          = p.getUChar ("ow_pin",  g_cfg.ow_pin);
  g_cfg.resolution      = p.getUChar ("ow_res",  g_cfg.resolution);
  g_cfg.meas_interval_s = p.getUShort("ms_int",  g_cfg.meas_interval_s);

  s = p.getString("mq_host", g_cfg.mqtt_host);  strlcpy(g_cfg.mqtt_host, s.c_str(), sizeof(g_cfg.mqtt_host));
  g_cfg.mqtt_port = p.getUShort("mq_port", g_cfg.mqtt_port);
  s = p.getString("mq_user", g_cfg.mqtt_user);  strlcpy(g_cfg.mqtt_user, s.c_str(), sizeof(g_cfg.mqtt_user));
  s = p.getString("mq_pass", g_cfg.mqtt_pass);  strlcpy(g_cfg.mqtt_pass, s.c_str(), sizeof(g_cfg.mqtt_pass));
  s = p.getString("mq_pfx",  g_cfg.sys_prefix); strlcpy(g_cfg.sys_prefix, s.c_str(), sizeof(g_cfg.sys_prefix));
  g_cfg.mqtt_interval_s = p.getUShort("mq_int", g_cfg.mqtt_interval_s);

  g_cfg.ccm_enabled    = p.getBool  ("ccm_en",  g_cfg.ccm_enabled);
  g_cfg.ccm_interval_s = p.getUShort("ccm_int", g_cfg.ccm_interval_s);
  g_cfg.ccm_priority   = p.getShort ("ccm_pri", g_cfg.ccm_priority);
  s = p.getString("ccm_nt", g_cfg.ccm_ntype);   strlcpy(g_cfg.ccm_ntype, s.c_str(), sizeof(g_cfg.ccm_ntype));

  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    SlotConfig &sl = g_cfg.slot[i];
    s = p.getString(slotKey("rom", i), sl.rom);      strlcpy(sl.rom,      s.c_str(), sizeof(sl.rom));
    s = p.getString(slotKey("lab", i), sl.label);    strlcpy(sl.label,    s.c_str(), sizeof(sl.label));
    s = p.getString(slotKey("top", i), sl.topic);    strlcpy(sl.topic,    s.c_str(), sizeof(sl.topic));
    s = p.getString(slotKey("typ", i), sl.ccm_type); strlcpy(sl.ccm_type, s.c_str(), sizeof(sl.ccm_type));
    sl.ccm_room   = p.getShort(slotKey("rm",  i), sl.ccm_room);
    sl.ccm_region = p.getShort(slotKey("rg",  i), sl.ccm_region);
    sl.ccm_order  = p.getShort(slotKey("or",  i), sl.ccm_order);
    sl.offset_c   = p.getFloat(slotKey("off", i), sl.offset_c);
  }
  p.end();
}

inline bool saveConfig() {
  Preferences p;
  if (!p.begin("temp-cfg", false)) return false;
  p.putString("node_id",  g_cfg.node_id);
  p.putString("hostname", g_cfg.hostname);
  p.putUChar ("ow_pin",   g_cfg.ow_pin);
  p.putUChar ("ow_res",   g_cfg.resolution);
  p.putUShort("ms_int",   g_cfg.meas_interval_s);
  p.putString("mq_host",  g_cfg.mqtt_host);
  p.putUShort("mq_port",  g_cfg.mqtt_port);
  p.putString("mq_user",  g_cfg.mqtt_user);
  p.putString("mq_pass",  g_cfg.mqtt_pass);
  p.putString("mq_pfx",   g_cfg.sys_prefix);
  p.putUShort("mq_int",   g_cfg.mqtt_interval_s);
  p.putBool  ("ccm_en",   g_cfg.ccm_enabled);
  p.putUShort("ccm_int",  g_cfg.ccm_interval_s);
  p.putShort ("ccm_pri",  g_cfg.ccm_priority);
  p.putString("ccm_nt",   g_cfg.ccm_ntype);
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    p.putString(slotKey("rom", i), sl.rom);
    p.putString(slotKey("lab", i), sl.label);
    p.putString(slotKey("top", i), sl.topic);
    p.putString(slotKey("typ", i), sl.ccm_type);
    p.putShort (slotKey("rm",  i), sl.ccm_room);
    p.putShort (slotKey("rg",  i), sl.ccm_region);
    p.putShort (slotKey("or",  i), sl.ccm_order);
    p.putFloat (slotKey("off", i), sl.offset_c);
  }
  p.end();
  return true;
}
