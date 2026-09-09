// ccm_pub.h — optional UECS-CCM publisher, one slot per packet.
//
// Wire format is byte-identical to agri-node-poe-core/AgriCCM.h, but that
// header is ETH/W5500-bound (ETH.localIP() + NetworkUDP), so the WiFi node
// re-implements the same packet over WiFiUDP, exactly as agri-amp-wifi does:
//
//   <?xml version="1.0"?><UECS ver="1.00-E10">
//     <DATA type="WaterTemp.cMC" room="1" region="13" order="1" priority="29">21.44</DATA>
//     <IP>192.168.x.y</IP>
//   </UECS>
//
// Two hard-won rules from the fleet, both encoded here:
//   1. ONE <DATA> per packet. UECS allows several per envelope, but ArSprout's
//      receiver keeps only the last one and silently drops the rest. This cost
//      agri-flow and agri-amp weeks of "why is ArSprout ignoring us" (2026-06-21).
//   2. Send to the LIMITED BROADCAST address as well as the multicast group —
//      ArSprout listens on 255.255.255.255 and ignores 224.0.0.1 (2026-06-10).

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "config.h"
#include "sensors.h"

inline WiFiUDP &ccmSocket() { static WiFiUDP u; return u; }

static const uint16_t  CCM_PORT = 16520;
static const IPAddress CCM_BROADCAST(255, 255, 255, 255);
static const IPAddress CCM_MULTICAST(224, 0, 0, 1);
static const char     *CCM_UECS_VER = "1.00-E10";

inline void ccmBegin() { ccmSocket().begin(0); }   // send-only ephemeral socket

inline String ccmDatum(const String &type, int room, int region, int order,
                       int priority, const String &value) {
  String s; s.reserve(160);
  s  = "<DATA type=\""; s += type;
  s += "\" room=\"";    s += room;
  s += "\" region=\"";  s += region;
  s += "\" order=\"";   s += order;
  s += "\" priority=\"";s += priority;
  s += "\">";           s += value;
  s += "</DATA>";
  return s;
}

inline bool ccmSendTo(const IPAddress &dest, const String &xml) {
  WiFiUDP &u = ccmSocket();
  if (!u.beginPacket(dest, CCM_PORT)) return false;
  u.write((const uint8_t *)xml.c_str(), xml.length());
  return u.endPacket();
}

inline bool ccmPublish() {
  if (!g_cfg.ccm_enabled || WiFi.status() != WL_CONNECTED) return false;

  bool any = false;
  for (int i = 0; i < CFG_MAX_SLOTS; i++) {
    const SlotConfig &sl = g_cfg.slot[i];
    if (!sl.rom[0] || !sl.ccm_type[0] || !g_slotOk[i]) continue;

    // "<type>.<ntype>" (empty ntype = bare type).
    String type = sl.ccm_type;
    if (g_cfg.ccm_ntype[0]) { type += '.'; type += g_cfg.ccm_ntype; }

    String xml; xml.reserve(220);
    xml  = "<?xml version=\"1.0\"?><UECS ver=\""; xml += CCM_UECS_VER; xml += "\">";
    xml += ccmDatum(type, sl.ccm_room, sl.ccm_region, sl.ccm_order,
                    g_cfg.ccm_priority, String(g_slotTemp[i], 2));
    xml += "<IP>"; xml += WiFi.localIP().toString(); xml += "</IP></UECS>";

    bool bc = ccmSendTo(CCM_BROADCAST, xml);
    bool mc = ccmSendTo(CCM_MULTICAST, xml);
    Serial.printf("[CCM] slot%d %s region=%d order=%d TX %u bytes (bc=%d mc=%d)\n",
                  i, type.c_str(), sl.ccm_region, sl.ccm_order,
                  (unsigned)xml.length(), bc, mc);
    any |= (bc || mc);
  }
  return any;
}
