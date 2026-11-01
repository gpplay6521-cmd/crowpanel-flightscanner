#pragma once
#include <stdint.h>

#include "config.h"

struct Aircraft {
  char call[10];
  char reg[10];
  char type[6];
  char desc[28];  // make and model as the feed spells it, e.g. "BOEING 737-800"
  char hex[8];
  float xKm, yKm;  // east / north of home when fetched
  float km;        // distance from home when fetched
  float track;     // degrees true
  float alt;       // ft
  float gs;        // knots
  bool hasAlt;
  bool hasFlight;  // `call` is a real flight callsign (routes can be looked up), not a registration or hex
};

struct TrafficSnapshot {
  Aircraft planes[MAX_AC];  // nearest first
  int count;
  bool haveData;
  uint32_t fetchMs;  // millis() of the last good fetch
  int failures;      // consecutive failed cycles
  char lastError[64];
  char wifiInfo[200];  // networks the scan saw after a failed Wi-Fi connect
};

// Starts the network task (Wi-Fi, NTP, ADS-B polling) on core 0.
void trafficBegin();
// Ask for a fetch as soon as possible.
void trafficRequestRefresh();
// Debugging aid: while paused the network task makes no requests (Wi-Fi stays up).
void trafficPause(bool paused);
// Thread-safe copy of the latest data.
void trafficGet(TrafficSnapshot& out);
