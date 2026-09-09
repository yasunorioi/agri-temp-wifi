// self_update.h — GitHub release self-update for the WiFi build.
//
// A port of agri-node-poe-core/AgriOTA.h. That header cannot be used here: it
// is built on arduino-esp32 3.x's NetworkClientSecure and pulls in the ETH
// bound core. This file keeps the SAME namespace and the SAME function names
// (agri::OTA::begin / checkLatest / schedule / poll / state) on 2.x's
// WiFiClientSecure, so if this node ever moves to 3.x the whole file can be
// deleted and replaced by `#include <AgriOTA.h>` with no call-site changes.
// agri-display-atom carries the same port for the same reason.
//
// Semi-automatic by design — it never flashes on its own:
//   boot        begin(repo, bin, version) + checkLatest()
//   loop        poll()  -> daily re-check, and flashes only when PENDING
//   dashboard   a yellow "Update to vX.Y.Z" button appears when newer
//   click       POST /api/update -> schedule() -> next poll() runs it
//
// URL convention (must match how the release is published):
//   GET https://api.github.com/repos/<repo>/releases/latest      -> tag_name
//   GET https://github.com/<repo>/releases/download/v<ver>/<bin>
// so the tag must be vX.Y.Z and the asset must be named exactly FW_BIN_NAME.
//
// TLS is setInsecure() — no cert pinning, same as the rest of the fleet.

#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>

namespace agri {
namespace OTA {

enum State { IDLE, PENDING, RUNNING, DONE, FAILED };

inline volatile State state           = IDLE;
inline String         latestVersion;
inline String         releaseUrl;
inline String         error;
inline bool           updateAvailable = false;
inline int            progress        = 0;
inline const char    *repo            = nullptr;
inline const char    *binName         = nullptr;
inline const char    *currentVersion  = nullptr;

inline uint32_t       recheckIntervalMs = 24UL * 60 * 60 * 1000;   // 0 disables
inline uint32_t       lastCheckMs       = 0;

inline const char *stateString() {
  switch (state) {
    case PENDING: return "pending";
    case RUNNING: return "running";
    case DONE:    return "done";
    case FAILED:  return "failed";
    default:      return "idle";
  }
}

inline bool semverNewer(const String &a, const String &b) {
  int ai[3] = {0, 0, 0}, bi[3] = {0, 0, 0};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i = 0; i < 3; i++) {
    if (ai[i] > bi[i]) return true;
    if (ai[i] < bi[i]) return false;
  }
  return false;
}

inline String extractJsonString(const String &body, const char *key) {
  String needle = String("\"") + key + "\":\"";
  int p = body.indexOf(needle);
  if (p < 0) return "";
  p += needle.length();
  int e = body.indexOf('"', p);
  if (e <= p) return "";
  return body.substring(p, e);
}

inline void begin(const char *repo_, const char *binName_, const char *currentVersion_) {
  repo           = repo_;
  binName        = binName_;
  currentVersion = currentVersion_;
}

inline void checkLatest() {
  if (!repo || !currentVersion) return;
  if (WiFi.status() != WL_CONNECTED) return;
  lastCheckMs = millis();          // stamp up-front so a failed check waits a full interval

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  String url = String("https://api.github.com/repos/") + repo + "/releases/latest";
  if (!http.begin(client, url)) {
    Serial.println("[OTA] http.begin failed");
    return;
  }
  http.addHeader("User-Agent", "agri-temp-wifi");
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    String tag  = extractJsonString(body, "tag_name");
    String html = extractJsonString(body, "html_url");
    if (tag.length() > 0) {
      String ver = (tag.startsWith("v") || tag.startsWith("V")) ? tag.substring(1) : tag;
      latestVersion   = ver;
      releaseUrl      = html;
      updateAvailable = semverNewer(ver, currentVersion);
      Serial.printf("[OTA] latest=%s current=%s update=%d\n",
                    ver.c_str(), currentVersion, updateAvailable ? 1 : 0);
    } else {
      Serial.println("[OTA] tag_name not found");
    }
  } else {
    Serial.printf("[OTA] check HTTP %d\n", code);
  }
  http.end();
}

inline void schedule() {
  if (state == IDLE || state == FAILED) state = PENDING;
}

inline void run() {
  if (!repo || !binName || !latestVersion.length()) {
    error = "not configured";
    state = FAILED;
    return;
  }
  state    = RUNNING;
  progress = 0;
  error    = "";

  String url = String("https://github.com/") + repo +
               "/releases/download/v" + latestVersion + "/" + binName;
  Serial.printf("[OTA] GET %s\n", url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    error = "http.begin failed";
    state = FAILED;
    return;
  }
  http.addHeader("User-Agent", "agri-temp-wifi-OTA");
  int code = http.GET();
  if (code != 200) {
    error = "HTTP " + String(code);
    state = FAILED;
    http.end();
    return;
  }
  int total = http.getSize();
  if (total <= 0) {
    error = "no content-length";
    state = FAILED;
    http.end();
    return;
  }
  if (!Update.begin((size_t)total)) {
    error = String("Update.begin: ") + Update.errorString();
    state = FAILED;
    http.end();
    return;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  int written = 0;
  uint32_t lastYield = millis();
  while (http.connected() && written < total) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail > sizeof(buf) ? sizeof(buf) : avail;
      int r = stream->readBytes(buf, toRead);
      if (r <= 0) break;
      if (Update.write(buf, r) != (size_t)r) {
        error = String("write: ") + Update.errorString();
        Update.abort();
        state = FAILED;
        http.end();
        return;
      }
      written += r;
      progress = (int)((int64_t)written * 100 / total);
    } else {
      delay(1);
    }
    if (millis() - lastYield > 50) { yield(); lastYield = millis(); }
  }
  http.end();
  if (written != total) {
    error = "short read";
    Update.abort();
    state = FAILED;
    return;
  }
  if (!Update.end(true)) {
    error = String("Update.end: ") + Update.errorString();
    state = FAILED;
    return;
  }
  Serial.println("[OTA] success, restarting");
  state = DONE;
  delay(500);
  ESP.restart();
}

inline void poll() {
  if (state == PENDING) { run(); return; }
  if (state == RUNNING) return;
  if (repo && currentVersion && recheckIntervalMs &&
      (uint32_t)(millis() - lastCheckMs) >= recheckIntervalMs) {
    checkLatest();
  }
}

}  // namespace OTA
}  // namespace agri
