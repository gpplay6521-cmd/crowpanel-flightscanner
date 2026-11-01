#pragma once
#include <stdint.h>

struct Airport {
  char iata[4];
  char icao[5];
  char country[24];  // short English name: "Cyprus", "United Kingdom"
  char city[26];
  char name[20];     // shortened airport name: "Gatwick", "Larnaca Airport"
  float lat, lon;
};

// Where a flight (identified by its callsign) departs from and lands.
struct RouteInfo {
  enum class State : uint8_t { Loading, Found, NotFound };
  State state;
  char airline[30];
  Airport from, to;
};

// Cached route for a flight callsign. A callsign seen for the first time returns
// State::Loading and queues a lookup; `urgent` moves it to the front (the aircraft the
// user selected). Safe to call from any task.
RouteInfo routeLookup(const char* callsign, bool urgent);

void routeBegin();

// Network task only: resolves queued lookups (a few per call, over one TLS connection).
// Returns true if it did any work.
bool routeWork();
