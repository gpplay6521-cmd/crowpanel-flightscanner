#include "traffic.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <string.h>

#include "route.h"
#include "secrets.h"

namespace {

SemaphoreHandle_t lock;
TrafficSnapshot shared;  // guarded by lock
volatile bool refreshRequested = true;
volatile bool paused = false;
uint32_t lastGoodMs = 0;

void setError(const char* fmt, ...) {
  char msg[sizeof shared.lastError];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(shared.lastError, msg, sizeof shared.lastError);
  xSemaphoreGive(lock);
  Serial.printf("[net] %s\n", msg);
}

void copyTrimmed(char* dst, size_t n, const char* src) {
  while (*src == ' ') ++src;
  strlcpy(dst, src, n);
  for (size_t i = strlen(dst); i > 0 && dst[i - 1] == ' '; --i) dst[i - 1] = 0;
}

const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL: return "SSID not found";
    case WL_CONNECT_FAILED: return "connect failed (password?)";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    case WL_IDLE_STATUS: return "idle";
    default: return "unknown";
  }
}

void setWifiInfo(const char* text) {
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(shared.wifiInfo, text, sizeof shared.wifiInfo);
  xSemaphoreGive(lock);
}

// After a failed connect: list what the radio can actually see. Catches a
// misspelled SSID (e.g. "-" vs "_") or a network that is 5 GHz only.
void reportVisibleNetworks() {
  int n = WiFi.scanNetworks();
  char info[sizeof shared.wifiInfo];
  if (n <= 0) {
    strlcpy(info, n == 0 ? "Scan saw no networks (2.4 GHz only)" : "WiFi scan failed", sizeof info);
  } else {
    strlcpy(info, "Seen:", sizeof info);
    size_t len = strlen(info);
    for (int i = 0; i < n; ++i) {
      Serial.printf("[net] scan: \"%s\" ch%d %d dBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
      if (i < 6 && len < sizeof info - 1) len += snprintf(info + len, sizeof info - len, " %.22s(%d)", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
  setWifiInfo(info);
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (strlen(WIFI_SSID) == 0) {
    setError("Set WIFI_SSID in include/secrets.h");
    return false;
  }
  setError("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) vTaskDelay(pdMS_TO_TICKS(250));
  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t st = WiFi.status();
    WiFi.disconnect();
    setError("WiFi: %s (%d)", wifiStatusName(st), (int)st);
    reportVisibleNetworks();
    return false;
  }
  setWifiInfo("");
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  Serial.printf("[net] WiFi connected, %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// Parses the readsb-style "v2" JSON (adsb.fi calls the list "aircraft", adsb.lol
// "ac") from a Stream or a String and keeps only the fields we draw.
template <typename Input>
bool parseTraffic(Input& in, Aircraft* out, int& count) {
  JsonDocument filter;
  for (const char* list : {"ac", "aircraft"}) {
    JsonObject f = filter[list].add<JsonObject>();
    for (const char* k : {"hex", "flight", "r", "t", "desc", "lat", "lon", "alt_baro", "alt_geom", "gs", "track"}) f[k] = true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, in, DeserializationOption::Filter(filter));
  if (err) {
    setError("JSON: %s", err.c_str());
    return false;
  }
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) ac = doc["aircraft"].as<JsonArray>();
  if (ac.isNull()) {
    setError("API returned no aircraft list");
    return false;
  }

  const float maxKm = RANGES_KM[RANGE_COUNT - 1];
  count = 0;
  for (JsonObject a : ac) {
    if (a["lat"].isNull() || a["lon"].isNull()) continue;
    JsonVariant altBaro = a["alt_baro"];
    if (altBaro.is<const char*>()) continue;  // "ground"

    Aircraft q = {};
    toKm(a["lat"].as<float>(), a["lon"].as<float>(), q.xKm, q.yKm);
    q.km = sqrtf(q.xKm * q.xKm + q.yKm * q.yKm);
    if (q.km > maxKm) continue;

    bool hasTrack = a["track"].is<float>();
    q.track = hasTrack ? a["track"].as<float>() : 0.0f;
    q.gs = hasTrack ? (a["gs"] | 0.0f) : 0.0f;  // no heading, no dead reckoning

    copyTrimmed(q.call, sizeof q.call, a["flight"] | "");
    q.hasFlight = q.call[0] != 0;
    if (!q.call[0]) copyTrimmed(q.call, sizeof q.call, a["r"] | "");
    if (!q.call[0]) copyTrimmed(q.call, sizeof q.call, a["hex"] | "?");
    copyTrimmed(q.reg, sizeof q.reg, a["r"] | "");
    copyTrimmed(q.type, sizeof q.type, a["t"] | "");
    copyTrimmed(q.desc, sizeof q.desc, a["desc"] | "");
    copyTrimmed(q.hex, sizeof q.hex, a["hex"] | "");

    if (altBaro.is<float>()) {
      q.alt = altBaro.as<float>();
      q.hasAlt = true;
    } else if (a["alt_geom"].is<float>()) {
      q.alt = a["alt_geom"].as<float>();
      q.hasAlt = true;
    }

    if (count < MAX_AC) {
      out[count++] = q;
    } else {  // full: replace the farthest if this one is closer
      int far = 0;
      for (int i = 1; i < count; ++i)
        if (out[i].km > out[far].km) far = i;
      if (q.km < out[far].km) out[far] = q;
    }
  }

  for (int i = 1; i < count; ++i) {  // nearest first
    Aircraft key = out[i];
    int j = i - 1;
    while (j >= 0 && out[j].km > key.km) {
      out[j + 1] = out[j];
      --j;
    }
    out[j + 1] = key;
  }
  return true;
}

void publish(const Aircraft* src, int n) {
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(shared.planes, src, n * sizeof(Aircraft));
  shared.count = n;
  shared.haveData = true;
  shared.failures = 0;
  shared.fetchMs = millis();
  shared.lastError[0] = 0;
  xSemaphoreGive(lock);
  lastGoodMs = millis();
}

bool fetchOnce() {
  if (!ensureWiFi()) return false;

  // Free community feeds. adsb.fi had noticeably better coverage in testing;
  // airplanes.live and adsb.one answer 403 unless you contact them first.
  struct Source {
    const char* host;
    const char* path;  // printf(lat, lon, nautical miles)
  };
  static const Source SOURCES[] = {
      {"opendata.adsb.fi", "/api/v2/lat/%.4f/lon/%.4f/dist/%d"},
      {"api.adsb.lol", "/v2/point/%.4f/%.4f/%d"},
  };
  static Aircraft fresh[MAX_AC];

  for (const Source& src : SOURCES) {
    const char* host = src.host;
    for (int attempt = 1; attempt <= 2; ++attempt) {
      char path[96], url[160];
      snprintf(path, sizeof path, src.path, HOME_LAT, HOME_LON, FETCH_NM);
      snprintf(url, sizeof url, "https://%s%s", host, path);

      WiFiClientSecure client;
      client.setInsecure();  // public read-only data; no certificate bundle to maintain
      client.setHandshakeTimeout(15);
      HTTPClient http;
      http.setConnectTimeout(10000);
      http.setTimeout(15000);
      http.setReuse(false);
      http.setUserAgent("CrowPanel-FlightScanner/1.0");

      uint32_t t0 = millis();
      Serial.printf("[adsb] %s (try %d)\n", host, attempt);
      if (http.begin(client, url)) {
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
          int n = 0;
          bool ok;
          if (http.getSize() > 0) {
            ok = parseTraffic(http.getStream(), fresh, n);  // Content-Length known: parse the socket directly
          } else {
            String body = http.getString();  // chunked: HTTPClient decodes it for us
            ok = parseTraffic(body, fresh, n);
          }
          http.end();
          Serial.printf("[adsb] %s: %d aircraft in range, %u ms, heap %u KB\n", ok ? "ok" : "parse failed", n,
                        (unsigned)(millis() - t0), (unsigned)(ESP.getFreeHeap() / 1024));
          if (ok) {
            publish(fresh, n);
            return true;
          }
        } else {
          http.end();
          if (code < 0) setError("%s: %s", host, HTTPClient::errorToString(code).c_str());
          else setError("%s HTTP %d", host, code);
        }
      } else {
        setError("HTTPS begin failed: %s", host);
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
  return false;
}

void netTask(void*) {
  uint32_t lastAttempt = 0;
  bool first = true;
  for (;;) {
    uint32_t now = millis();
    bool due = first || now - lastAttempt >= FETCH_INTERVAL_MS || (refreshRequested && now - lastAttempt >= 3000);
    if (due && !paused) {
      first = false;
      refreshRequested = false;
      if (!fetchOnce()) {
        xSemaphoreTake(lock, portMAX_DELAY);
        ++shared.failures;
        xSemaphoreGive(lock);
      }
      lastAttempt = millis();
    }
    if (lastGoodMs && !paused && millis() - lastGoodMs > RESTART_AFTER_MS) {
      Serial.println("[net] no data for 15 min, restarting");
      ESP.restart();
    }
    // Flight routes are looked up between position fetches; a fetch that falls due waits at most one lookup.
    bool worked = !paused && lastGoodMs && WiFi.status() == WL_CONNECTED && routeWork();
    vTaskDelay(pdMS_TO_TICKS(worked ? 10 : 100));
  }
}

}  // namespace

void trafficBegin() {
  lock = xSemaphoreCreateMutex();
  routeBegin();
  strlcpy(shared.lastError, "Starting...", sizeof shared.lastError);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void trafficRequestRefresh() { refreshRequested = true; }

void trafficPause(bool p) { paused = p; }

void trafficGet(TrafficSnapshot& out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = shared;
  xSemaphoreGive(lock);
}
