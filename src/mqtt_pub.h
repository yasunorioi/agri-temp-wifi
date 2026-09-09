// mqtt_pub.h — one slot, one topic, one value.
//
// Standalone MQTT (agri-node-poe-core is ETH-bound): WiFiClient + PubSubClient
// with the family's LWT convention "<prefix>/sys/<id>/online" (1/0 retained).
//
// Payload is the canonical agriha per-type form established by agri-env-poe:
//     {"value":21.44,"unit":"C","ts":1788334103}
// retained, one physical quantity per topic (mqtt-topics.md §0.2). Deliberately
// NOT the older multi-field blob that agri-drain/agri-flow still publish — the
// logger splits blobs into "<topic>#field" series, which is exactly the mess
// that makes agriha/1/sensor/Drain awkward to query today.

#pragma once

#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include "config.h"
#include "sensors.h"

inline WiFiClient   &mqttNet()    { static WiFiClient c; return c; }
inline PubSubClient &mqttClient() { static PubSubClient m; return m; }

inline void mqttBegin() {
  PubSubClient &m = mqttClient();
  m.setClient(mqttNet());
  m.setBufferSize(384);
  m.setKeepAlive(30);
}

inline bool mqttHasHost()   { return g_cfg.mqtt_host[0] != '\0'; }
inline bool mqttConnected() { return mqttClient().connected(); }
inline void mqttLoop()      { mqttClient().loop(); }

// Epoch seconds if SNTP has synced (configTime called in setup), else 0.
inline uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? (uint32_t)t : 0;
}

inline bool mqttReconnect() {
  if (!mqttHasHost() || WiFi.status() != WL_CONNECTED) return false;
  PubSubClient &m = mqttClient();
  if (m.connected()) return true;
  m.setServer(g_cfg.mqtt_host, g_cfg.mqtt_port);

  char will[128];
  snprintf(will, sizeof(will), "%s/sys/%s/online", g_cfg.sys_prefix, g_cfg.node_id);

  bool ok;
  if (g_cfg.mqtt_user[0]) {
    ok = m.connect(g_cfg.node_id, g_cfg.mqtt_user, g_cfg.mqtt_pass, will, 0, true, "0");
  } else {
    ok = m.connect(g_cfg.node_id, nullptr, nullptr, will, 0, true, "0");
  }
  Serial.printf("[MQTT] connect(%s:%u) = %s\n",
                g_cfg.mqtt_host, g_cfg.mqtt_port, ok ? "OK" : "FAIL");
  if (ok) m.publish(will, "1", true);
  return ok;
}

inline bool mqttPublishSlot(int i) {
  const SlotConfig &sl = g_cfg.slot[i];
  if (!mqttConnected() || !sl.rom[0] || !sl.topic[0] || !g_slotOk[i]) return false;

  char payload[128];
  int n = snprintf(payload, sizeof(payload),
                   "{\"value\":%.2f,\"unit\":\"C\",\"ts\":%lu}",
                   g_slotTemp[i], (unsigned long)nowEpoch());
  bool ok = mqttClient().publish(sl.topic, (const uint8_t *)payload, n, true);
  Serial.printf("[MQTT] slot%d %s %s %.2fC\n", i, sl.topic, ok ? "OK" : "FAIL", g_slotTemp[i]);
  return ok;
}

inline bool mqttPublishAll() {
  bool any = false;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) any |= mqttPublishSlot(i);
  return any;
}
