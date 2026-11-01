#include "mapdata.h"

// The map around your position is generated into include/map_local.inc by tools/set_location.py (it is
// git-ignored, it shows where you are). Without it the radar simply has no coastline, border or runways.
#if __has_include("map_local.inc")
#include "map_local.inc"
#else
const int16_t MAP_PTS[] = {0, 0};
const MapLine MAP_LINES[] = {{0, 0, 0}};
const int MAP_LINE_COUNT = 0;
#endif
