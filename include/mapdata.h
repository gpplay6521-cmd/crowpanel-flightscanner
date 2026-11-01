#pragma once
#include <stdint.h>

// Map polylines around HOME, made by tools/set_location.py into include/map_local.inc from OpenStreetMap
// data (c) OpenStreetMap contributors, ODbL. Points are (east, north) pairs in MAP_UNIT_KM steps
// from HOME, MAP_LINES[i] covers `count` of them from pair `first`.
//   MAP_COAST   coastline, drawn as a line
//   MAP_BORDER  border of the region (OSM admin level 4) the position is in
//   MAP_RUNWAY  runways of the reference airport
//   MAP_LAND    closed land outlines (coast joined along a 125 km circle), only used for filling
constexpr float MAP_UNIT_KM = 0.05f;
enum MapKind : uint8_t { MAP_COAST = 0, MAP_BORDER = 1, MAP_RUNWAY = 2, MAP_LAND = 3 };

struct MapLine {
  uint16_t first;
  uint16_t count;
  uint8_t kind;
};

extern const int16_t MAP_PTS[];
extern const MapLine MAP_LINES[];
extern const int MAP_LINE_COUNT;
