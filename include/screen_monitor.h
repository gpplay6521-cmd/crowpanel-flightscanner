// Live screen monitoring + remote control for any ESP32 (Arduino) project, a few lines to add.
//
// Why Wi-Fi: on most ESP32 dev boards the native USB (USB-Serial/JTAG) is unreliable for live
// monitoring, so this serves a screenshot (BMP) and a status page over HTTP instead.
//
// The server has NO authentication. Anyone on the same network can read the screen, send taps
// and reboot the board, so only use it on a network you trust.
//
// As a fallback for when Wi-Fi does not work (first setup, network problems), the screenshot can
// also be written to flash, to a region that esptool can read without Wi-Fi or serial logs
// (see Config::flashOffset).
#pragma once
#include <Arduino.h>

namespace ScreenMonitor {

// Writes the current screen as a 24 bpp BMP into 'out'. Returns true on success and fills outLen.
// Your project implements this for its own display driver. For RGB565 / GFXcanvas16 based
// displays, Bmp565::encode() in bmp565.h does the work directly.
typedef bool (*CaptureBmpFn)(uint8_t *out, size_t cap, size_t &outLen);

// Short status line (optional), appended to the /status output.
typedef String (*StatusLineFn)();

// Runs on a "/action?name=X&value=Y" GET request (optional), for custom remote controls such as
// simulated button presses.
typedef void (*RemoteActionFn)(const String &name, const String &value);

struct Config {
    const char *wifiSsid = nullptr;  // leave null if Wi-Fi is already connected elsewhere
    const char *wifiPass = nullptr;
    const char *mdnsHostname = "screenmon";  // http://<name>.local/ (not always reliable, prefer the IP)
    unsigned long wifiConnectTimeoutMs = 10000;

    CaptureBmpFn captureBmp = nullptr;  // required
    size_t bmpBufCapacity = 0;          // required: the largest BMP captureBmp writes (allocated from PSRAM)

    StatusLineFn statusLine = nullptr;        // optional
    RemoteActionFn onRemoteAction = nullptr;  // optional

    // Flash backup: even when Wi-Fi is down, the screenshot can be read with
    // `esptool.py --port COMx read_flash <flashOffset> <flashRegionSize> out.bin`.
    // 0 = off.
    //
    // WARNING: make sure this address is NOT used by your partition table (partitions.csv),
    // or you can corrupt your application or file system (SPIFFS/LittleFS/NVS). Add up all
    // offset+size ranges of the partition file and compare with the flash size: flashOffset
    // must lie beyond that total, in free space.
    uint32_t flashOffset = 0;
    uint32_t flashRegionSize = 0;                  // must fit the BMP plus a 108 byte header
    unsigned long flashWriteMinIntervalMs = 2000;  // limits flash wear
};

// Connects to Wi-Fi if credentials are given (on failure it scans and logs the networks it can
// see, which catches misspelled or 5 GHz only SSIDs) and starts the HTTP server. Call it at the
// very end of setup(), after the heavy initialisation, because connecting takes a few seconds.
void begin(const Config &cfg);

// Call on every loop() iteration. It is cheap and throttled internally.
void handle();

bool isConnected();
String connectionInfo();  // an IP address such as "192.168.1.50", or "NOWIFI(1)" (wl_status_t code)

}  // namespace ScreenMonitor
