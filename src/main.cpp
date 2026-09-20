/*
 * agri-temp-wifi — multi-point 1-Wire temperature node (DS18B20 xN), WiFi
 *
 * Builds for two boards from one source tree; everything that differs between
 * them lives in board.h (status LED, button, default 1-Wire pin):
 *     [env:m5atoms3-wifi]  M5Stack AtomS3 Lite (ESP32-S3)   <- default
 *     [env:m5atomu-wifi]   M5Stack ATOM U      (ESP32-PICO-D4)
 *
 * agri-* family member, WiFi flavour (neither board carries a PoE base here, so
 * agri-node-poe-core — which is W5500/ETH-bound — cannot be used; MQTT, CCM
 * and the WebUI are reimplemented locally with the same conventions, exactly
 * as in agri-amp-wifi). The PoE sibling of this node is agri-temp-poe.
 *
 *   - MQTT to agriha, one slot = one topic, {value,unit,ts} retained  mqtt_pub.h
 *   - optional UECS-CCM broadcast, one <DATA> per packet              ccm_pub.h
 *   - ROM-address -> slot binding, so probes never silently swap      config.h
 *
 * Provisioning is WiFiManager's captive portal (hold the Atom button at boot
 * to force it). mDNS + ArduinoOTA give "agri-temp-01.local" + wireless flash;
 * HTTP OTA at POST /api/ota is the reliable path on Windows.
 */
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include "board.h"
#include "config.h"
#include "sensors.h"
#include "ccm_pub.h"
#include "mqtt_pub.h"
#include "self_update.h"
#include "webui.h"

#define FW_NAME     "agri-temp-wifi"
#define FW_VERSION  "0.3.5"
// GitHub release self-update. The tag must be vX.Y.Z and the release asset
// must be named exactly FW_BIN_NAME, or the device will find the tag but 404
// on the download.
#define FW_REPO     "yasunorioi/agri-temp-wifi"
// Per-board asset name, set in platformio.ini — the two images are different
// architectures and must never be downloaded into each other.
#define FW_BIN_NAME FW_BIN_NAME_STR

// ---- globals declared extern in the headers --------------------------------
AppConfig g_cfg;
Probe     g_probe[MAX_PROBES];
int       g_probeCount = 0;
float     g_slotTemp[CFG_MAX_SLOTS];
bool      g_slotOk[CFG_MAX_SLOTS];
bool      g_busOk = false;

// ---- WiFiManager: WiFi credentials only ------------------------------------
// Node config (1-Wire/MQTT/CCM/slots) lives in the always-on WebUI (webui.h),
// reachable at http://<hostname>.local/ once connected — single source of truth.
void setupWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  board::update();
  bool forcePortal = board::buttonPressed();  // hold button at boot to reconfigure WiFi

  bool ok;
  if (forcePortal) {
    board::pixel(0xffff00);                // yellow = portal open
    Serial.println("[WiFi] button held -> config portal");
    ok = wm.startConfigPortal("agri-temp-setup");
  } else {
    board::pixel(0xffaa00);
    ok = wm.autoConnect("agri-temp-setup");
  }
  if (!ok) { Serial.println("[WiFi] failed -> restart"); delay(1000); ESP.restart(); }
  Serial.printf("[WiFi] connected %s\n", WiFi.localIP().toString().c_str());
}

// ---- setup / loop ----------------------------------------------------------
void setup() {
  board::begin();
  delay(50);
  board::pixel(0xff0000);

  for (int i = 0; i < CFG_MAX_SLOTS; i++) { g_slotTemp[i] = NAN; g_slotOk[i] = false; }

  loadConfig();
  sensorsBegin();

  setupWiFi();
  configTime(0, 0, "ntp.nict.jp", "pool.ntp.org");   // SNTP for the MQTT ts field

  MDNS.begin(g_cfg.hostname);
  ArduinoOTA.setHostname(g_cfg.hostname);
  ArduinoOTA.onStart([]{ Serial.println("[OTA] start"); });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] err %u\n", e); });
  ArduinoOTA.begin();

  ccmBegin();
  mqttBegin();
  webBegin(FW_NAME, FW_VERSION);

  agri::OTA::begin(FW_REPO, FW_BIN_NAME, FW_VERSION);
  agri::OTA::checkLatest();     // once at boot; poll() re-checks daily

  Serial.printf("[BOOT] %s %s on %s  ip=%s  probes=%d  mqtt=%s  ccm=%s\n",
                FW_NAME, FW_VERSION, board::NAME,
                WiFi.localIP().toString().c_str(), g_probeCount,
                g_cfg.mqtt_host[0] ? g_cfg.mqtt_host : "(none)",
                g_cfg.ccm_enabled ? "on" : "off");
}

// Conversion is asynchronous, so a measurement is two timed steps:
//   REQUEST -> (CONVERT_MS) -> COLLECT -> (meas_interval_s) -> REQUEST ...
enum MeasState { MEAS_IDLE, MEAS_CONVERTING };
static MeasState measState  = MEAS_IDLE;
static uint32_t  lastMeasMs = 0, convertStartMs = 0;
static uint32_t  lastMqttMs = 0, lastCcmMs = 0, lastMqttTryMs = 0, lastScanMs = 0;

void loop() {
  board::update();
  ArduinoOTA.handle();
  webHandle();
  agri::OTA::poll();     // daily re-check; flashes only when /api/update armed it

  // MQTT keepalive / reconnect (non-blocking, retry every 5 s)
  if (mqttHasHost()) {
    if (!mqttConnected() && millis() - lastMqttTryMs > 5000) {
      lastMqttTryMs = millis();
      mqttReconnect();
    }
    mqttLoop();
  }

  // Re-enumerate every 60 s so a probe added or replaced in the field shows up
  // without a reboot. Cheap: a bus search, not a conversion.
  if (millis() - lastScanMs > 60000) {
    lastScanMs = millis();
    sensorsScan();
  }

  switch (measState) {
    case MEAS_IDLE:
      if (millis() - lastMeasMs < (uint32_t)g_cfg.meas_interval_s * 1000) break;
      lastMeasMs = millis();
      board::pixel(0x00ff00);                // green while converting
      sensorsRequest();
      convertStartMs = millis();
      measState = MEAS_CONVERTING;
      break;

    case MEAS_CONVERTING:
      if (millis() - convertStartMs < (uint32_t)CONVERT_MS) break;
      sensorsCollect();
      measState = MEAS_IDLE;

      for (int i = 0; i < CFG_MAX_SLOTS; i++) {
        if (g_cfg.slot[i].rom[0] && g_slotOk[i]) {
          Serial.printf("[TEMP] slot%d %-8s %.2f C\n", i, g_cfg.slot[i].label, g_slotTemp[i]);
        }
      }

      if (mqttConnected() &&
          millis() - lastMqttMs >= (uint32_t)g_cfg.mqtt_interval_s * 1000) {
        lastMqttMs = millis();
        mqttPublishAll();
      }
      if (g_cfg.ccm_enabled &&
          millis() - lastCcmMs >= (uint32_t)g_cfg.ccm_interval_s * 1000) {
        lastCcmMs = millis();
        ccmPublish();
      }
      board::pixel(WiFi.status() == WL_CONNECTED ? 0x0000ff : 0xff0000);
      break;
  }
}
