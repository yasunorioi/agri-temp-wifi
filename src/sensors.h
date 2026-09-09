// sensors.h — 1-Wire bus scan + DS18B20 readout, resolved into config slots.
//
// Replaces the hand-rolled OneWire scratchpad decoding of the original sketch
// (Documents/Arduino/M5Atom-ds18b20_influxdb) with DallasTemperature, which
// handles multi-drop enumeration, resolution and the DS18S20/DS1822 variants.
//
// Conversions are asynchronous: sensorsRequest() kicks all probes off at once
// (one broadcast SKIP ROM CONVERT T) and sensorsCollect() reads them ~750 ms
// later, so the main loop never blocks on the bus. That matters because the
// WebUI and MQTT keepalive share this loop.

#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

static const int  MAX_PROBES     = 16;
static const long CONVERT_MS     = 900;    // 12-bit needs 750 ms; margin for slow buses
static const float TEMP_INVALID  = -127.0f; // DallasTemperature's disconnected marker

struct Probe {
  char  rom[17];      // 16 uppercase hex chars
  float temp_c;
  bool  ok;
  int   slot;         // config slot this ROM is bound to, or -1
};

extern Probe g_probe[MAX_PROBES];
extern int   g_probeCount;
extern float g_slotTemp[CFG_MAX_SLOTS];   // calibrated value per slot
extern bool  g_slotOk[CFG_MAX_SLOTS];
extern bool  g_busOk;

inline OneWire *&owBus() { static OneWire *p = nullptr; return p; }
inline DallasTemperature *&owDs() { static DallasTemperature *p = nullptr; return p; }

// 8-byte ROM -> "28FF641E8C1A0334" (uppercase, no separators)
inline void romToStr(const uint8_t *addr, char *out /*[17]*/) {
  static const char *hex = "0123456789ABCDEF";
  for (int i = 0; i < 8; i++) {
    out[i * 2]     = hex[(addr[i] >> 4) & 0x0F];
    out[i * 2 + 1] = hex[addr[i] & 0x0F];
  }
  out[16] = '\0';
}

inline bool strToRom(const char *s, uint8_t *addr /*[8]*/) {
  if (!s || strlen(s) != 16) return false;
  for (int i = 0; i < 8; i++) {
    char b[3] = { s[i * 2], s[i * 2 + 1], '\0' };
    char *end = nullptr;
    long v = strtol(b, &end, 16);
    if (end != b + 2) return false;
    addr[i] = (uint8_t)v;
  }
  return true;
}

// Which slot owns this ROM string, or -1.
inline int slotForRom(const char *rom) {
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    if (g_cfg.slot[i].rom[0] && strcasecmp(g_cfg.slot[i].rom, rom) == 0) return i;
  }
  return -1;
}

// Enumerate the bus into g_probe[]. Cheap enough to run on every poll, which
// means a probe that is unplugged and plugged back in reappears on its own.
inline void sensorsScan() {
  g_probeCount = 0;
  DallasTemperature *ds = owDs();
  if (!ds) { g_busOk = false; return; }

  ds->begin();                     // re-enumerates the bus
  int n = ds->getDeviceCount();
  g_busOk = (n > 0);
  for (int i = 0; i < n && g_probeCount < MAX_PROBES; i++) {
    DeviceAddress addr;
    if (!ds->getAddress(addr, i)) continue;
    Probe &p = g_probe[g_probeCount];
    romToStr(addr, p.rom);
    p.temp_c = NAN;
    p.ok     = false;
    p.slot   = slotForRom(p.rom);
    g_probeCount++;
  }
  ds->setResolution(g_cfg.resolution);
}

// First-boot convenience: if no slot has a ROM bound yet, bind the probes we
// just found to slots 0..N-1 in bus order and persist it. Bus order is ROM
// order, i.e. arbitrary — the README tells you to confirm which is which by
// warming one probe by hand and watching the dashboard.
inline bool sensorsAutoBind() {
  for (int i = 0; i < CFG_MAX_SLOTS; i++) if (g_cfg.slot[i].rom[0]) return false;
  if (g_probeCount == 0) return false;

  int n = min(g_probeCount, CFG_MAX_SLOTS);
  for (int i = 0; i < n; i++) {
    strlcpy(g_cfg.slot[i].rom, g_probe[i].rom, sizeof(g_cfg.slot[i].rom));
    g_probe[i].slot = i;
  }
  saveConfig();
  Serial.printf("[1W] auto-bound %d probe(s) to slots 0..%d\n", n, n - 1);
  return true;
}

// Field diagnostic: when the configured pin enumerates nothing, walk the pins
// the ATOM U can actually expose and report which one (if any) has devices on
// it. Purely advisory — it never rewrites the config, it just turns "probes=0"
// from a dead end into "your bus is on G25". Runs once at boot.
//
// The candidate list deliberately excludes GPIO6-11 and 16/17 (the ESP32-PICO-
// D4's embedded flash) and 27 (the on-board SK6812 status LED).
static const uint8_t OW_CANDIDATE_PINS[] = { 26, 32, 25, 33, 21, 22, 19, 23 };

inline int owCountOnPin(uint8_t pin) {
  OneWire probe(pin);
  uint8_t addr[8];
  int n = 0;
  probe.reset_search();
  while (probe.search(addr)) {
    if (OneWire::crc8(addr, 7) == addr[7] && addr[0] != 0x00) n++;
    if (n > MAX_PROBES) break;
  }
  return n;
}

inline void sensorsHuntBus() {
  Serial.println(F("[1W] no device on the configured pin — scanning candidates"));
  for (size_t i = 0; i < sizeof(OW_CANDIDATE_PINS); i++) {
    uint8_t p = OW_CANDIDATE_PINS[i];
    int n = owCountOnPin(p);
    Serial.printf("[1W]   G%-2u : %d device(s)%s\n", p, n,
                  (n > 0 && p != g_cfg.ow_pin) ? "   <-- set DATA pin to this in /config" : "");
  }
  Serial.println(F("[1W] all zero => check power (3.3V/GND) and the 4.7k pull-up"));
}

inline void sensorsBegin() {
  owBus() = new OneWire(g_cfg.ow_pin);
  owDs()  = new DallasTemperature(owBus());
  owDs()->begin();
  owDs()->setWaitForConversion(false);   // async: request now, read later
  owDs()->setResolution(g_cfg.resolution);
  sensorsScan();
  sensorsAutoBind();
  Serial.printf("[1W] pin=G%u res=%u bit  probes=%d\n",
                g_cfg.ow_pin, g_cfg.resolution, g_probeCount);
  for (int i = 0; i < g_probeCount; i++) {
    Serial.printf("[1W]   %s -> slot %d\n", g_probe[i].rom, g_probe[i].slot);
  }
  if (g_probeCount == 0) sensorsHuntBus();
}

inline void sensorsRequest() {
  if (owDs()) owDs()->requestTemperatures();   // returns immediately (async)
}

inline void sensorsCollect() {
  DallasTemperature *ds = owDs();
  for (int i = 0; i < CFG_MAX_SLOTS; i++) { g_slotOk[i] = false; g_slotTemp[i] = NAN; }
  if (!ds) return;

  for (int i = 0; i < g_probeCount; i++) {
    Probe &p = g_probe[i];
    DeviceAddress addr;
    if (!strToRom(p.rom, addr)) { p.ok = false; continue; }
    float t = ds->getTempC(addr);
    p.ok     = (t > TEMP_INVALID + 0.5f) && !isnan(t);
    p.temp_c = p.ok ? t : NAN;
    p.slot   = slotForRom(p.rom);
    if (p.ok && p.slot >= 0) {
      g_slotTemp[p.slot] = t + g_cfg.slot[p.slot].offset_c;
      g_slotOk[p.slot]   = true;
    }
  }
}
