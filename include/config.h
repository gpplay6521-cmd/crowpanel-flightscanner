#pragma once
#include <stdint.h>
#include <math.h>

// Where the radar sits, the time zone and the reference airport are NOT part of the repository: they
// live in include/user_config.h, which is git-ignored so your position never gets committed. Create it
// (and the map around it) with
//   python -I tools/set_location.py --lat <degrees> --lon <degrees> --name <LABEL> --tz "<POSIX tz>"
// or copy include/user_config.example.h to include/user_config.h and edit it.
#if __has_include("user_config.h")
#include "user_config.h"
#else
#error "include/user_config.h is missing: choose where the radar sits with  python -I tools/set_location.py --lat <deg> --lon <deg>  (see the README)"
#endif

static_assert(HOME_LAT != 0.0f || HOME_LON != 0.0f, "Set HOME_LAT / HOME_LON in include/user_config.h");

// Optional settings a hand-written user_config.h may leave out.
#ifndef SHOW_HOME_COORDS
#define SHOW_HOME_COORDS 0
#endif
#ifndef TZ_INFO
#define TZ_INFO "UTC0"
#endif
#ifdef REF_AIRPORT_CODE
constexpr bool HAS_REF_AIRPORT = true;
#else
constexpr bool HAS_REF_AIRPORT = false;
#define REF_AIRPORT_CODE "---"
#define REF_AIRPORT_LAT HOME_LAT
#define REF_AIRPORT_LON HOME_LON
#endif

// Selectable radar ranges. The largest one also sets the API query radius.
constexpr float RANGES_KM[] = {25.0f, 50.0f, 100.0f};
constexpr float RING_STEP_KM[] = {5.0f, 10.0f, 25.0f};  // range-ring spacing for each range
constexpr int RANGE_COUNT = sizeof(RANGES_KM) / sizeof(RANGES_KM[0]);
constexpr int DEFAULT_RANGE_IDX = 2;
constexpr int FETCH_NM = 54;  // 54 nm = 100 km

constexpr uint32_t FETCH_INTERVAL_MS = 15UL * 1000UL;
constexpr uint32_t RESTART_AFTER_MS = 15UL * 60UL * 1000UL;  // no good data for this long -> reboot
constexpr int MAX_AC = 80;

// Flat-earth offset from home in km. Good to well under 1 km inside 250 km.
inline void toKm(float lat, float lon, float& east, float& north) {
  north = (lat - HOME_LAT) * 111.195f;
  east = (lon - HOME_LON) * 111.195f * cosf(HOME_LAT * 0.01745329f);
}
