#include "monitor.h"

#include <Arduino.h>
#include <WiFi.h>

#include "bmp565.h"
#include "board.h"
#include "debug.h"
#include "screen_monitor.h"
#include "traffic.h"
#include "ui.h"

namespace dbg {
volatile bool redrawPaused = false;
volatile bool drawNow = false;
}  // namespace dbg

namespace {

bool captureBmp(uint8_t* out, size_t cap, size_t& len) {
  // The framebuffer on screen is the one we are not drawing into.
  const uint16_t* front = board::frameBuffer(board::backIndex() ^ 1);
  return Bmp565::encode(front, board::WIDTH, board::HEIGHT, out, cap, len);
}

String statusLine() {
  static TrafficSnapshot snap;  // only used from the monitor task
  trafficGet(snap);
  String s = "planes=" + String(snap.count) + " haveData=" + String(snap.haveData) + " fails=" + String(snap.failures);
  if (snap.lastError[0]) s += " err=" + String(snap.lastError);
  if (snap.wifiInfo[0]) s += " wifi=" + String(snap.wifiInfo);
  board::ScanoutStats d = board::scanoutStats();
  uint32_t frameMs, frameMax;
  ui::frameTimes(frameMs, frameMax);
  s += " frame_ms=" + String(frameMs) + "/" + String(frameMax);
  s += " theme=" + String(ui::themeIndex()) + "(" + ui::themeName(ui::themeIndex()) + ")";
  s += " perf=" + String(ui::perfSummary());
  s += " scanout=vsync:" + String(d.vsyncs) + ",bad:" + String(d.badFrames) + ",last:" + String(d.lastBadCount) +
       ",resync:" + String(d.resyncActive);
  return s;
}

// /action?name=dbg&value=<cmd>[:<arg>], for isolating display glitches:
//   redraw:0|1   stop/resume the 1 Hz screen redraw (the last frame stays on screen)
//   net:0|1      stop/resume ADS-B requests (Wi-Fi stays connected)
//   pclk:<hz>    pixel clock, e.g. 12000000 (boot value is 14000000)
//   txp:<n>      Wi-Fi TX power in 0.25 dBm steps, 8 = 2 dBm, 78 = 19.5 dBm (default)
//   label:<text> text shown at the right end of the footer
void onDebug(const String& cmd) {
  int colon = cmd.indexOf(':');
  String key = colon < 0 ? cmd : cmd.substring(0, colon);
  String arg = colon < 0 ? "" : cmd.substring(colon + 1);
  if (key == "redraw") {
    dbg::redrawPaused = arg == "0";
  } else if (key == "net") {
    trafficPause(arg == "0");
  } else if (key == "pclk") {
    Serial.printf("[dbg] pclk %s Hz: %s\n", arg.c_str(), board::setPixelClock(arg.toInt()) ? "ok" : "failed");
  } else if (key == "txp") {
    WiFi.setTxPower(static_cast<wifi_power_t>(arg.toInt()));
    Serial.printf("[dbg] tx power set to %d (0.25 dBm)\n", (int)WiFi.getTxPower());
  } else if (key == "label") {
    ui::setDebugLabel(arg.c_str());
  }
  dbg::drawNow = true;
}

void onAction(const String& name, const String& value) {
  if (name == "tap") {
    int comma = value.indexOf(',');
    if (comma > 0) ui::queueTap(value.substring(0, comma).toInt(), value.substring(comma + 1).toInt());
  } else if (name == "refresh") {
    trafficRequestRefresh();
  } else if (name == "theme") {  // /action?name=theme&value=0..4
    ui::requestTheme(value.toInt());
    dbg::drawNow = true;
  } else if (name == "dbg") {
    onDebug(value);
  }
}

void monitorTask(void*) {
  while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(500));

  ScreenMonitor::Config cfg;  // no SSID here: Wi-Fi is already managed by traffic.cpp
  cfg.mdnsHostname = "flightradar";
  cfg.captureBmp = captureBmp;
  cfg.bmpBufCapacity = Bmp565::bmpSizeFor(board::WIDTH, board::HEIGHT);
  cfg.statusLine = statusLine;
  cfg.onRemoteAction = onAction;
  ScreenMonitor::begin(cfg);
  Serial.printf("[monitor] http://%s/screenshot.bmp\n", WiFi.localIP().toString().c_str());

  for (;;) {
    ScreenMonitor::handle();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

void monitorBegin() { xTaskCreatePinnedToCore(monitorTask, "monitor", 8192, nullptr, 1, nullptr, 0); }
