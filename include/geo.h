#pragma once
#include <math.h>

#include "config.h"

// Inverse of toKm(): km east / north of home -> latitude, longitude.
inline void fromKm(float east, float north, float& lat, float& lon) {
  lat = HOME_LAT + north / 111.195f;
  lon = HOME_LON + east / (111.195f * cosf(HOME_LAT * 0.01745329f));
}

// Great-circle distance in km (haversine).
inline float gcDistKm(float lat1, float lon1, float lat2, float lon2) {
  const float r = 0.01745329f;
  float dLat = (lat2 - lat1) * r, dLon = (lon2 - lon1) * r;
  float a = sinf(dLat / 2) * sinf(dLat / 2) + cosf(lat1 * r) * cosf(lat2 * r) * sinf(dLon / 2) * sinf(dLon / 2);
  return 12742.0f * asinf(fminf(1.0f, sqrtf(a)));
}

// Initial great-circle bearing from point 1 to point 2, degrees true 0..360.
inline float gcBearingDeg(float lat1, float lon1, float lat2, float lon2) {
  const float r = 0.01745329f;
  float dLon = (lon2 - lon1) * r;
  float y = sinf(dLon) * cosf(lat2 * r);
  float x = cosf(lat1 * r) * sinf(lat2 * r) - sinf(lat1 * r) * cosf(lat2 * r) * cosf(dLon);
  float b = atan2f(y, x) / r;
  return b < 0 ? b + 360.0f : b;
}

// Azimuthal equidistant projection around HOME: km east / north of home for any point on earth.
// Unlike toKm() it stays right for far-away airports, so flight routes are drawn with it.
inline void toKmAz(float lat, float lon, float& east, float& north) {
  float d = gcDistKm(HOME_LAT, HOME_LON, lat, lon);
  float b = gcBearingDeg(HOME_LAT, HOME_LON, lat, lon) * 0.01745329f;
  east = d * sinf(b);
  north = d * cosf(b);
}
