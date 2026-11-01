#include <Arduino.h>

#include "board.h"
#include "debug.h"
#include "monitor.h"
#include "traffic.h"
#include "ui.h"

static TrafficSnapshot snapshot;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] CrowPanel FlightScanner");
  Serial.printf("[boot] heap %u KB, PSRAM %u KB free\n", (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getFreePsram() / 1024));

  board::begin();
  ui::begin();
  trafficBegin();
  monitorBegin();
}

// Returns true when the screen needs redrawing now.
static bool handleTap(int x, int y) {
  switch (ui::tap(x, y)) {
    case ui::Action::Refresh:
      trafficRequestRefresh();
      return true;
    case ui::Action::Redraw:
      return true;
    default:
      return false;
  }
}

void loop() {
  static uint32_t lastDraw = 0;
  static bool wasDown = false;
  bool redraw = false;

  int x = 0, y = 0;
  bool down = board::readTouch(x, y);
  if (down && !wasDown) redraw |= handleTap(x, y);
  wasDown = down;

  if (ui::takeQueuedTap(x, y)) redraw |= handleTap(x, y);  // from the remote monitor

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'u') trafficRequestRefresh();
    if (c == 'i') {
      Serial.printf("[status] planes=%d haveData=%d fails=%d heap=%u KB psram=%u KB up=%lu s\n", snapshot.count,
                    snapshot.haveData, snapshot.failures, (unsigned)(ESP.getFreeHeap() / 1024),
                    (unsigned)(ESP.getFreePsram() / 1024), millis() / 1000);
      board::ScanoutStats d = board::scanoutStats();
      Serial.printf("[status] scanout vsync=%u bad=%u last=%u resync=%d\n", (unsigned)d.vsyncs, (unsigned)d.badFrames,
                    (unsigned)d.lastBadCount, d.resyncActive);
    }
  }

  bool force = dbg::drawNow;
  dbg::drawNow = false;
  if (force || (!dbg::redrawPaused && (redraw || millis() - lastDraw >= 1000))) {
    trafficGet(snapshot);
    ui::draw(snapshot);
    lastDraw = millis();
  }
  if (!dbg::redrawPaused) ui::tick();
  delay(15);
}
