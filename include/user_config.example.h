#pragma once
// Example of include/user_config.h: where the radar is centred, the time zone and the reference airport.
//
// Normally you do not write this by hand: tools/set_location.py creates include/user_config.h for you
// (and builds the map around it):
//   python -I tools/set_location.py --lat 52.3731 --lon 4.8922 --tz "CET-1CEST,M3.5.0,M10.5.0/3"
// To do it by hand instead, copy this file to include/user_config.h and edit it. user_config.h is
// git-ignored on purpose: it holds your position, so keep it out of anything you publish.
//
// The values below are the example (central Amsterdam), they are not a default: replace them.

constexpr float HOME_LAT = 52.3731f;  // decimal degrees, south is negative
constexpr float HOME_LON = 4.8922f;   // decimal degrees, west is negative
#define HOME_NAME "AMSTERDAM"         // shown in the title bar, ASCII only
#define SHOW_HOME_COORDS 0            // 1 = also print the coordinates in the title bar

// Reference airport: drawn on the radar, and used for the arrival / departure colours and the approach
// lines of the ATC theme. Delete these three lines if you do not want one.
#define REF_AIRPORT_CODE "AMS"
#define REF_AIRPORT_LAT 52.3105f
#define REF_AIRPORT_LON 4.7683f

// POSIX time zone string for the clock, for example:
//   "UTC0"   "<+03>-3"   "CET-1CEST,M3.5.0,M10.5.0/3"   "GMT0BST,M3.5.0/1,M10.5.0"   "EST5EDT,M3.2.0,M11.1.0"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
