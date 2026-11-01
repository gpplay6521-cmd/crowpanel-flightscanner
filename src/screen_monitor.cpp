#include "screen_monitor.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"

namespace ScreenMonitor {
namespace {

Config gCfg;
WebServer server(80);
WebServer *serverPtr = nullptr;

uint8_t *bmpBuf = nullptr;
String gConnInfo = "WIFI?";
bool gConnected = false;

RTC_DATA_ATTR uint32_t gBootCount = 0;
String gResetReason = "?";

unsigned long gLastFlashWriteAt = 0;

const char *resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "PWRON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INTWDT";
        case ESP_RST_TASK_WDT:  return "TASKWDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                 return "UNKNOWN";
    }
}

void handleRoot() {
    server.send(200, "text/plain",
        "Screen monitor\n"
        "GET /screenshot.bmp - screenshot\n"
        "GET /status - status\n"
        "GET /action?name=X&value=Y - remote control (project specific)\n"
        "GET /reboot - restart the device\n");
}

void handleScreenshot() {
    if (!bmpBuf || !gCfg.captureBmp) {
        server.send(500, "text/plain", "captureBmp not set");
        return;
    }
    size_t len = 0;
    if (!gCfg.captureBmp(bmpBuf, gCfg.bmpBufCapacity, len)) {
        server.send(500, "text/plain", "capture failed");
        return;
    }
    WiFiClient client = server.client();
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                  (unsigned)len);
    client.write(bmpBuf, len);
}

void handleStatus() {
    String s;
    s += "boot=" + String(gBootCount) + "\n";
    s += "reset=" + gResetReason + "\n";
    s += "wifi=" + gConnInfo + "\n";
    s += "heap_free=" + String(ESP.getFreeHeap()) + "\n";
    s += "psram_free=" + String(ESP.getFreePsram()) + "\n";
    s += "uptime_ms=" + String(millis()) + "\n";
    if (gCfg.statusLine) s += "app=" + gCfg.statusLine() + "\n";
    server.send(200, "text/plain", s);
}

void handleAction() {
    if (gCfg.onRemoteAction) {
        String name = server.hasArg("name") ? server.arg("name") : "";
        String value = server.hasArg("value") ? server.arg("value") : "";
        gCfg.onRemoteAction(name, value);
        server.send(200, "text/plain", "ok");
    } else {
        server.send(501, "text/plain", "onRemoteAction not set");
    }
}

void handleReboot() {
    server.send(200, "text/plain", "restarting...");
    delay(200);
    ESP.restart();
}

// Flash backup layout (starting at gCfg.flashOffset):
//   [0..3]   = "SSHT" (validity signature)
//   [4..7]   = uint32 LE, BMP length
//   [8..107] = status text, 100 bytes, zero padded
//   [108..]  = the BMP file
void writeFlashSnapshotIfDue() {
    if (gCfg.flashOffset == 0 || gCfg.flashRegionSize == 0) return;
    if (!bmpBuf) return;
    if (millis() - gLastFlashWriteAt < gCfg.flashWriteMinIntervalMs) return;
    if (!gCfg.captureBmp) return;

    size_t bmpLen = 0;
    if (!gCfg.captureBmp(bmpBuf, gCfg.bmpBufCapacity, bmpLen)) return;

    String status = gConnInfo + " b" + String(gBootCount) + " " + gResetReason;
    if (gCfg.statusLine) status += " " + gCfg.statusLine();

    uint8_t header[108] = {0};
    header[0] = 'S'; header[1] = 'S'; header[2] = 'H'; header[3] = 'T';
    uint32_t bmpLen32 = (uint32_t)bmpLen;
    memcpy(header + 4, &bmpLen32, sizeof(uint32_t));
    strncpy((char *)(header + 8), status.c_str(), 99);

    size_t total = sizeof(header) + bmpLen;
    size_t eraseSize = (total + 4095) & ~((size_t)4095);
    if (eraseSize > gCfg.flashRegionSize) eraseSize = gCfg.flashRegionSize;

    if (esp_flash_erase_region(NULL, gCfg.flashOffset, eraseSize) != ESP_OK) return;
    esp_flash_write(NULL, header, gCfg.flashOffset, sizeof(header));
    esp_flash_write(NULL, bmpBuf, gCfg.flashOffset + sizeof(header), bmpLen);

    gLastFlashWriteAt = millis();
}

}  // namespace

void begin(const Config &cfg) {
    gCfg = cfg;
    serverPtr = &server;

    gBootCount++;
    gResetReason = resetReasonStr(esp_reset_reason());

    if (gCfg.bmpBufCapacity > 0) {
        bmpBuf = (uint8_t *)heap_caps_malloc(gCfg.bmpBufCapacity, MALLOC_CAP_SPIRAM);
    }

    if (gCfg.wifiSsid && gCfg.wifiPass) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(gCfg.wifiSsid, gCfg.wifiPass);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < gCfg.wifiConnectTimeoutMs) delay(200);

        if (WiFi.status() == WL_CONNECTED) {
            gConnected = true;
            gConnInfo = WiFi.localIP().toString();
            if (gCfg.mdnsHostname) MDNS.begin(gCfg.mdnsHostname);
        } else {
            // If the SSID was not found (WL_NO_SSID_AVAIL = 1), scan and log the networks around:
            // this catches typos (a "-" instead of a "_") and 5 GHz only networks.
            String scanList;
            if (WiFi.status() == WL_NO_SSID_AVAIL) {
                int n = WiFi.scanNetworks();
                scanList = " seen(" + String(n) + "):";
                for (int i = 0; i < n && i < 6; i++) {
                    scanList += " " + WiFi.SSID(i) + "(" + String(WiFi.RSSI(i)) + ")";
                }
            }
            gConnInfo = "NOWIFI(" + String((int)WiFi.status()) + ")" + scanList;
        }
    }

    server.on("/", handleRoot);
    server.on("/screenshot.bmp", handleScreenshot);
    server.on("/status", handleStatus);
    server.on("/action", handleAction);
    server.on("/reboot", handleReboot);
    server.begin();
}

void handle() {
    if (serverPtr) server.handleClient();
    writeFlashSnapshotIfDue();
}

bool isConnected() { return gConnected; }
String connectionInfo() { return gConnInfo; }

}  // namespace ScreenMonitor
