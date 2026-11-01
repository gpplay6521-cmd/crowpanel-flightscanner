#define LGFX_USE_V1
#include "ui.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "config.h"
#include "geo.h"
#include "mapdata.h"
#include "route.h"

namespace ui {
namespace {

namespace fnt = lgfx::fonts;
using lgfx::IFont;

// Text datums (TFT_eSPI numbering): top-left, top-right, middle-left/center/right.
constexpr uint8_t DT_TL = 0, DT_TR = 2, DT_ML = 4, DT_MC = 5, DT_MR = 6;

constexpr int W = board::WIDTH;
constexpr int H = board::HEIGHT;
constexpr int BAR_H = 40;
constexpr int FOOT_Y = 458;
constexpr int RCX = 236, RCY = 252, RR = 184;  // radar centre and radius
constexpr int PX = 474, PW = 318;              // right-hand panel
constexpr int TABLE_Y = 46, ROW_Y0 = 84, ROW_H = 24, ROWS = 9;
constexpr int CARD_Y = 310, CARD_H = 86;
constexpr int BTN_Y = 404, BTN_H = 42;
constexpr int TITLE_TAP_X = 250;   // top bar, left of this x cycles the theme
constexpr int BRIGHT_TAP_X = 560;  // top bar, right of this x cycles the brightness

// The static part of the radar (disc, map, rings, compass) is rendered once per range and theme into
// a square sprite and copied under every frame; the margin holds the tick marks and N/E/S/W.
constexpr int BG_MARGIN = 24;
constexpr int BG_SIZE = 2 * (RR + BG_MARGIN);
constexpr int BGX = RCX - RR - BG_MARGIN, BGY = RCY - RR - BG_MARGIN;

// The disc itself, for the sweep animation.
constexpr int DISC_X = RCX - RR, DISC_Y = RCY - RR, DISC_D = 2 * RR;

struct Btn {
  int x, w;
};
// One button per radar range, then REFRESH.
constexpr Btn BTNS[] = {{474, 68}, {548, 68}, {622, 68}, {696, 96}};
static_assert(sizeof(BTNS) / sizeof(BTNS[0]) == RANGE_COUNT + 1, "one button per range plus refresh");

constexpr float DEG2RAD = 0.01745329f;
constexpr float KNOT_KM_PER_S = 1.852f / 3600.0f;

constexpr uint16_t RGB(uint32_t hex) { return lgfx::color565((hex >> 16) & 255, (hex >> 8) & 255, hex & 255); }

// a * (256 - f) + b * f, per channel; f 0..256.
constexpr uint16_t mix(uint16_t a, uint16_t b, int f) {
  return (uint16_t)((((((a >> 11) & 31) * (256 - f) + ((b >> 11) & 31) * f) >> 8) << 11) |
                    (((((a >> 5) & 63) * (256 - f) + ((b >> 5) & 63) * f) >> 8) << 5) |
                    ((((a & 31) * (256 - f) + (b & 31) * f) >> 8)));
}

constexpr uint16_t C_UNK = RGB(0x969696);
constexpr uint16_t C_ERR = RGB(0xff5050);

// ---------------------------------------------------------------------------------------------
// Themes

struct Fonts {
  const IFont *small, *bold, *big, *huge;
  bool mono;
};
const Fonts SANS = {&fnt::FreeSans9pt7b, &fnt::FreeSansBold9pt7b, &fnt::FreeSansBold12pt7b, &fnt::FreeSansBold18pt7b, false};
const Fonts MONO = {&fnt::FreeMono9pt7b, &fnt::FreeMonoBold9pt7b, &fnt::FreeMonoBold12pt7b, &fnt::FreeMonoBold18pt7b, true};

enum class Style : uint8_t {
  Arrow,  // arrow head, callsign and flight level beside it
  Tag,    // arrow head with a trail and a callsign / level / route tag (flight tracker look)
  Atc     // diamond with speed vector, trail and a data block on a leader line (controller look)
};
enum class Legend : uint8_t { Bands, Ramp, Roles };
enum Role : uint8_t { ROLE_OVER = 0, ROLE_ARR = 1, ROLE_DEP = 2 };

struct Theme {
  const char* name;
  const Fonts* f;
  uint16_t bg, panel, panelLine, disc, land, line, ring, text, dim, accent, ok, warn, sel;
  uint16_t alt[3];  // aircraft colours below 10k ft, 10..25k ft, above (Style::Arrow)
  uint16_t coast, border, route, routeDim, runway, btnText, btnBorder, sweepColor;
  uint8_t borderOn, borderOff;  // province border dashes, 0 = solid
  uint8_t ringOn, ringOff;      // range ring dashes, 0 = solid
  uint8_t coastW;               // px
  uint8_t numbers;              // compass numbers every n degrees, 0 = none
  bool hasLand;                 // fill land (needs `land`), the disc colour is the sea
  bool outline;                 // outlined panels
  bool light;                   // light background: dark outline on the aircraft symbols
  bool sweep, crt, spokes, cardinal;
  Style style;
  Legend legend;
};

const Theme THEMES[] = {
    {.name = "A  Phosphor",
     .f = &MONO,
     .bg = RGB(0x010905),
     .panel = RGB(0x04140a),
     .panelLine = RGB(0x0e5a2d),
     .disc = RGB(0x021a0b),
     .land = 0,
     .line = RGB(0x1f9a4f),
     .ring = RGB(0x0e5a2d),
     .text = RGB(0xc8ffd8),
     .dim = RGB(0x52c27f),
     .accent = RGB(0x5dff9a),
     .ok = RGB(0x5dff9a),
     .warn = RGB(0xfff08a),
     .sel = mix(RGB(0x04140a), RGB(0x5dff9a), 48),
     .alt = {RGB(0x3fd070), RGB(0x7dffa8), RGB(0xe2ffec)},
     .coast = RGB(0x4dff94),
     .border = RGB(0x25a85a),
     .route = RGB(0xe8ff9a),
     .routeDim = RGB(0x62b85a),
     .runway = RGB(0x9dffc0),
     .btnText = RGB(0x01200c),
     .btnBorder = RGB(0x1f9a4f),
     .sweepColor = RGB(0x46ff8c),
     .borderOn = 2,
     .borderOff = 3,
     .ringOn = 0,
     .ringOff = 0,
     .coastW = 1,
     .numbers = 10,
     .hasLand = false,
     .outline = false,
     .light = false,
     .sweep = true,
     .crt = true,
     .spokes = true,
     .cardinal = true,
     .style = Style::Arrow,
     .legend = Legend::Bands},
    {.name = "B  Flight map",
     .f = &SANS,
     .bg = RGB(0x070d14),
     .panel = RGB(0x0d1520),
     .panelLine = RGB(0x1d2d40),
     .disc = RGB(0x0c2740),
     .land = RGB(0x25382b),
     .line = RGB(0x2d4a66),
     .ring = mix(RGB(0x0c2740), RGB(0xa0c8eb), 44),
     .text = RGB(0xeaf1f8),
     .dim = RGB(0x8aa0b6),
     .accent = RGB(0xffc107),
     .ok = RGB(0x3ddc84),
     .warn = RGB(0xffb020),
     .sel = mix(RGB(0x0d1520), RGB(0xffc107), 40),
     .alt = {RGB(0x3ddc84), RGB(0x38d6ff), RGB(0xffffff)},
     .coast = RGB(0x6fb39a),
     .border = RGB(0x8294a8),
     .route = RGB(0xffd54a),
     .routeDim = RGB(0x6ea8ff),
     .runway = RGB(0xc8d3df),
     .btnText = RGB(0x1b1300),
     .btnBorder = RGB(0x2d4a66),
     .sweepColor = 0,
     .borderOn = 5,
     .borderOff = 4,
     .ringOn = 3,
     .ringOff = 5,
     .coastW = 1,
     .numbers = 0,
     .hasLand = true,
     .outline = true,
     .light = false,
     .sweep = false,
     .crt = false,
     .spokes = false,
     .cardinal = true,
     .style = Style::Tag,
     .legend = Legend::Ramp},
    {.name = "C  Glass cockpit",
     .f = &SANS,
     .bg = RGB(0x04070b),
     .panel = RGB(0x0b121a),
     .panelLine = RGB(0x243445),
     .disc = RGB(0x04070b),
     .land = 0,
     .line = RGB(0xaab6c2),
     .ring = RGB(0x2a3a4a),
     .text = RGB(0xf3f7fb),
     .dim = RGB(0x8ea0b2),
     .accent = RGB(0x38d6ff),
     .ok = RGB(0x43f06f),
     .warn = RGB(0xffb020),
     .sel = mix(RGB(0x0b121a), RGB(0x38d6ff), 40),
     .alt = {RGB(0x43f06f), RGB(0x38d6ff), RGB(0xffffff)},
     .coast = RGB(0x6f8aa3),
     .border = RGB(0x47637f),
     .route = RGB(0xff58e0),
     .routeDim = RGB(0xff58e0),
     .runway = RGB(0xaab6c2),
     .btnText = RGB(0x021822),
     .btnBorder = RGB(0x2a3a4a),
     .sweepColor = 0,
     .borderOn = 5,
     .borderOff = 4,
     .ringOn = 4,
     .ringOff = 5,
     .coastW = 1,
     .numbers = 30,
     .hasLand = false,
     .outline = false,
     .light = false,
     .sweep = false,
     .crt = false,
     .spokes = false,
     .cardinal = false,
     .style = Style::Arrow,
     .legend = Legend::Bands},
    {.name = "D  Day chart",
     .f = &SANS,
     .bg = RGB(0xdfe7ec),
     .panel = RGB(0xf7f9fb),
     .panelLine = RGB(0xc3d1db),
     .disc = RGB(0xeaf2f6),
     .land = 0,
     .line = RGB(0x5d7f99),
     .ring = RGB(0xa9c0d0),
     .text = RGB(0x12283b),
     .dim = RGB(0x5b7184),
     .accent = RGB(0x0b5f8f),
     .ok = RGB(0x1f9d55),
     .warn = RGB(0xc77a00),
     .sel = mix(RGB(0xf7f9fb), RGB(0x0b5f8f), 40),
     .alt = {RGB(0x1f9d55), RGB(0x0b6fb8), RGB(0xd9531e)},
     .coast = RGB(0x165a8a),
     .border = RGB(0xb0268f),
     .route = RGB(0xc2189a),
     .routeDim = RGB(0x7c1f66),
     .runway = RGB(0x12283b),
     .btnText = RGB(0xffffff),
     .btnBorder = RGB(0x9fb5c5),
     .sweepColor = 0,
     .borderOn = 5,
     .borderOff = 4,
     .ringOn = 0,
     .ringOff = 0,
     .coastW = 2,
     .numbers = 30,
     .hasLand = false,
     .outline = true,
     .light = true,
     .sweep = false,
     .crt = false,
     .spokes = false,
     .cardinal = true,
     .style = Style::Arrow,
     .legend = Legend::Bands},
    {.name = "E  ATC console",
     .f = &MONO,
     .bg = RGB(0x05080b),
     .panel = RGB(0x10171e),
     .panelLine = RGB(0x243241),
     .disc = RGB(0x0a141d),
     .land = RGB(0x243629),
     .line = RGB(0x5b7083),
     .ring = RGB(0x2f3f4e),
     .text = RGB(0xdfe7ee),
     .dim = RGB(0x7f93a6),
     .accent = RGB(0x38d6ff),
     .ok = RGB(0x5be37d),
     .warn = RGB(0xffd23f),
     .sel = mix(RGB(0x10171e), RGB(0x38d6ff), 40),
     .alt = {RGB(0x5be37d), RGB(0x38d6ff), RGB(0xffffff)},
     .coast = RGB(0x58786a),
     .border = RGB(0xa06bd6),
     .route = RGB(0xffd23f),
     .routeDim = RGB(0x38d6ff),
     .runway = RGB(0xcfd8e3),
     .btnText = RGB(0x031620),
     .btnBorder = RGB(0x2f3f4e),
     .sweepColor = 0,
     .borderOn = 6,
     .borderOff = 4,
     .ringOn = 4,
     .ringOff = 5,
     .coastW = 1,
     .numbers = 30,
     .hasLand = true,
     .outline = true,
     .light = false,
     .sweep = false,
     .crt = false,
     .spokes = false,
     .cardinal = true,
     .style = Style::Atc,
     .legend = Legend::Roles},
};
constexpr int THEME_COUNT = sizeof(THEMES) / sizeof(THEMES[0]);

const Theme* T = &THEMES[0];
int themeIdx = 0;
volatile int pendingTheme = -1;
uint32_t toastUntil = 0;  // footer shows the theme name until then

constexpr uint16_t ROLE_COLOR[3] = {RGB(0xe6edf3), RGB(0x38d6ff), RGB(0x5be37d)};

// Altitude ramp of the flight-tracker theme: 0 ft orange, through yellow, green, cyan, blue, to purple at 40k ft.
constexpr struct {
  int alt;
  uint8_t r, g, b;
} RAMP[] = {{0, 255, 106, 0},     {2000, 255, 179, 0},  {5000, 212, 230, 0},  {10000, 46, 204, 64},
            {20000, 0, 194, 199}, {30000, 47, 123, 255}, {40000, 180, 76, 255}};

uint16_t rampColor(float alt) {
  constexpr int N = sizeof(RAMP) / sizeof(RAMP[0]);
  if (alt <= 0) return lgfx::color565(RAMP[0].r, RAMP[0].g, RAMP[0].b);
  for (int i = 1; i < N; ++i) {
    if (alt > RAMP[i].alt) continue;
    float f = (alt - RAMP[i - 1].alt) / (float)(RAMP[i].alt - RAMP[i - 1].alt);
    auto L = [&](uint8_t a, uint8_t b) { return (uint8_t)lroundf(a + (b - a) * f); };
    return lgfx::color565(L(RAMP[i - 1].r, RAMP[i].r), L(RAMP[i - 1].g, RAMP[i].g), L(RAMP[i - 1].b, RAMP[i].b));
  }
  return lgfx::color565(RAMP[N - 1].r, RAMP[N - 1].g, RAMP[N - 1].b);
}

// ---------------------------------------------------------------------------------------------

constexpr uint8_t BRIGHT[] = {100, 50, 15};

lgfx::LGFX_Sprite spr[2];
lgfx::LGFX_Sprite* cv = nullptr;  // surface for the frame being drawn
lgfx::LGFX_Sprite bg;             // cached static radar, see BG_SIZE
bool bgOk = false;
int bgRange = -1;                 // range index the cache was rendered for
int bgTheme = -1;                 // theme it was rendered for

char debugLabel[40] = "";

int rangeIdx = DEFAULT_RANGE_IDX;
int brightIdx = 0;
char selectedHex[8] = "";

// Hit-test data filled while drawing, used by tap().
struct Marker {
  int16_t x, y;
  char hex[8];
};
Marker markers[MAX_AC];
int markerCount = 0;
char rowHex[ROWS][8];
int rowCount = 0;

// Where each aircraft is right now (dead-reckoned from the last fix).
struct Live {
  float x, y, d;  // km east / north of home, distance
};
Live live[MAX_AC];
int order[MAX_AC];  // indices of aircraft inside the current range, nearest first
int orderCount = 0;

// Per-frame results of refreshRoutes().
int selIdx = -1;  // planes[] index of the selected aircraft, -1 when none (or it left the range)
RouteInfo selRoute;
char rowRoute[ROWS][12];        // "FRA>AMS" for each table row
char planeRoute[MAX_AC][12];    // the same, by planes[] index (only the table rows and the selected one are filled)
uint8_t planeRole[MAX_AC];      // Role by planes[] index
bool routeTouchesRef = false;

volatile uint32_t lastFrameMs = 0, maxFrameMs = 0;
volatile uint32_t perfMs[6];  // last frame: prepare, radar, panel, chrome, finish, present
volatile uint32_t sweepUs = 0;
uint32_t lastSweepMs = 0;
void sweepPump();  // phosphor theme, see below

void put(const char* s, int x, int y, const IFont* font, uint16_t color, uint8_t datum = DT_TL) {
  sweepPump();
  cv->setFont(font);
  cv->setTextColor(color);
  cv->setTextDatum(datum);
  cv->drawString(s, x, y);
}

int textW(const IFont* font, const char* s) {
  cv->setFont(font);
  return cv->textWidth(s);
}

// Centred text broken at spaces into lines of at most maxChars.
void putWrapped(const char* s, int cx, int y, int maxChars, int lineH, const IFont* font, uint16_t color) {
  char line[64];
  while (*s) {
    int len = (int)strlen(s);
    int cut = len <= maxChars ? len : maxChars;
    if (len > maxChars) {
      for (int i = maxChars; i > 0; --i)
        if (s[i] == ' ') {
          cut = i;
          break;
        }
    }
    if (cut >= (int)sizeof line) cut = sizeof line - 1;
    memcpy(line, s, cut);
    line[cut] = 0;
    put(line, cx, y, font, color, DT_MC);
    y += lineH;
    s += cut;
    while (*s == ' ') ++s;
  }
}

bool isSelected(const Aircraft& a) { return selectedHex[0] && strcmp(a.hex, selectedHex) == 0; }

// Colour of one aircraft in the current theme (planes[] index i).
uint16_t planeColor(int i, const Aircraft& a) {
  if (T->style == Style::Atc) return ROLE_COLOR[planeRole[i]];
  if (!a.hasAlt) return C_UNK;
  if (T->style == Style::Tag) return rampColor(a.alt);
  return a.alt < 10000 ? T->alt[0] : (a.alt <= 25000 ? T->alt[1] : T->alt[2]);
}

// Arrow-head aircraft symbol pointing along its track; `outline` < 0 for none.
void dart(float x, float y, float deg, uint16_t c, float scale = 1.0f, int32_t outline = -1) {
  float r = deg * DEG2RAD, s = sinf(r), co = cosf(r);
  auto X = [&](float f, float sd) { return (int)lroundf(x + (f * s + sd * co) * scale); };
  auto Y = [&](float f, float sd) { return (int)lroundf(y + (-f * co + sd * s) * scale); };
  int nx = X(11, 0), ny = Y(11, 0);
  int lx = X(-8, -7), ly = Y(-8, -7);
  int rx = X(-8, 7), ry = Y(-8, 7);
  int kx = X(-4, 0), ky = Y(-4, 0);
  cv->fillTriangle(nx, ny, lx, ly, kx, ky, c);
  cv->fillTriangle(nx, ny, kx, ky, rx, ry, c);
  if (outline >= 0) {
    cv->drawLine(nx, ny, lx, ly, (uint16_t)outline);
    cv->drawLine(lx, ly, kx, ky, (uint16_t)outline);
    cv->drawLine(kx, ky, rx, ry, (uint16_t)outline);
    cv->drawLine(rx, ry, nx, ny, (uint16_t)outline);
  }
}

void computeLive(const TrafficSnapshot& t) {
  float age = t.haveData ? fminf((millis() - t.fetchMs) / 1000.0f, 90.0f) : 0.0f;
  float rangeKm = RANGES_KM[rangeIdx];
  orderCount = 0;
  for (int i = 0; i < t.count; ++i) {
    const Aircraft& a = t.planes[i];
    float dist = a.gs * KNOT_KM_PER_S * age, r = a.track * DEG2RAD;
    live[i].x = a.xKm + dist * sinf(r);
    live[i].y = a.yKm + dist * cosf(r);
    live[i].d = hypotf(live[i].x, live[i].y);
    if (live[i].d <= rangeKm) order[orderCount++] = i;
  }
  for (int i = 1; i < orderCount; ++i) {
    int key = order[i], j = i - 1;
    while (j >= 0 && live[order[j]].d > live[key].d) {
      order[j + 1] = order[j];
      --j;
    }
    order[j + 1] = key;
  }
}

// Last few fixes of every aircraft (km east / north of home), for the trails of the tag and ATC themes.
constexpr int TRAIL_N = 10;
struct Trail {
  char hex[8];
  uint8_t n;
  uint32_t seenMs;
  float x[TRAIL_N], y[TRAIL_N];
};
Trail trails[MAX_AC];
uint32_t trailFetchMs = 0;

Trail* findTrail(const char* hex) {
  for (Trail& tr : trails)
    if (tr.hex[0] && strcmp(tr.hex, hex) == 0) return &tr;
  return nullptr;
}

void updateTrails(const TrafficSnapshot& t) {
  if (!t.haveData || t.fetchMs == trailFetchMs) return;
  trailFetchMs = t.fetchMs;
  for (Trail& tr : trails)
    if (tr.hex[0] && (int32_t)(t.fetchMs - tr.seenMs) > 120000) tr.hex[0] = 0;  // gone for two minutes
  for (int i = 0; i < t.count; ++i) {
    const Aircraft& a = t.planes[i];
    Trail* tr = findTrail(a.hex);
    if (!tr) {
      for (Trail& s : trails)
        if (!s.hex[0]) {
          tr = &s;
          break;
        }
      if (!tr) continue;
      memset(tr, 0, sizeof *tr);
      strlcpy(tr->hex, a.hex, sizeof tr->hex);
    }
    if (tr->n == TRAIL_N) {
      memmove(tr->x, tr->x + 1, (TRAIL_N - 1) * sizeof tr->x[0]);
      memmove(tr->y, tr->y + 1, (TRAIL_N - 1) * sizeof tr->y[0]);
      --tr->n;
    }
    tr->x[tr->n] = a.xKm;
    tr->y[tr->n] = a.yKm;
    ++tr->n;
    tr->seenMs = t.fetchMs;
  }
}

// Single line that shrinks from the 12pt to the 9pt bold font, then loses its tail, to fit maxW.
void putFit(const char* s, int x, int y, int maxW, uint16_t color) {
  const IFont* f = T->f->big;
  cv->setFont(f);
  if (cv->textWidth(s) > maxW) {
    f = T->f->bold;
    cv->setFont(f);
  }
  char b[40];
  strlcpy(b, s, sizeof b);
  for (size_t n = strlen(b); n > 1 && cv->textWidth(b) > maxW; --n) b[n - 1] = 0;
  put(b, x, y, f, color, DT_ML);
}

// Single line cut at the end to fit maxW pixels.
void putClip(const char* s, int x, int y, int maxW, const IFont* font, uint16_t color, uint8_t datum) {
  char b[40];
  strlcpy(b, s, sizeof b);
  cv->setFont(font);
  for (size_t n = strlen(b); n > 1 && cv->textWidth(b) > maxW; --n) b[n - 1] = 0;
  put(b, x, y, font, color, datum);
}

// The first of the candidate texts that fits maxW, centred.
void putFirstFit(const char* const* options, int count, int cx, int y, int maxW, const IFont* font, uint16_t color) {
  const char* s = options[count - 1];
  for (int i = 0; i < count; ++i)
    if (textW(font, options[i]) <= maxW) {
      s = options[i];
      break;
    }
  put(s, cx, y, font, color, DT_MC);
}

const char* airportCode(const Airport& a) { return a.iata[0] ? a.iata : a.icao; }

// Clips a segment (pixels relative to the radar centre) to the disc of radius r; false if none of it is inside.
bool clipSeg(float& x0, float& y0, float& x1, float& y1, float r) {
  float dx = x1 - x0, dy = y1 - y0, r2 = r * r;
  bool in0 = x0 * x0 + y0 * y0 <= r2, in1 = x1 * x1 + y1 * y1 <= r2;
  if (in0 && in1) return true;
  float a = dx * dx + dy * dy;
  if (a < 1e-9f) return false;
  float b = x0 * dx + y0 * dy, c = x0 * x0 + y0 * y0 - r2;
  float disc = b * b - a * c;
  if (disc <= 0.0f) return false;
  float s = sqrtf(disc);
  float lo = fmaxf(0.0f, (-b - s) / a), hi = fminf(1.0f, (-b + s) / a);
  if (lo >= hi) return false;
  float nx0 = x0 + lo * dx, ny0 = y0 + lo * dy, nx1 = x0 + hi * dx, ny1 = y0 + hi * dy;
  x0 = nx0;
  y0 = ny0;
  x1 = nx1;
  y1 = ny1;
  return true;
}

// One segment, given in pixels relative to (cx, cy) and already clipped: `width` px wide, dashed (`on` px
// drawn, `off` px skipped) unless on == 0. `phase` carries the dash pattern from segment to segment.
void strokeSeg(int cx, int cy, float x0, float y0, float x1, float y1, uint16_t c, int width, int on, int off, float& phase) {
  const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
  auto line = [&](float ax, float ay, float bx, float by) {
    for (int w = 0; w < width; ++w) {
      int o = w - (width - 1) / 2, ox = steep ? o : 0, oy = steep ? 0 : o;
      cv->drawLine(cx + (int)lroundf(ax) + ox, cy + (int)lroundf(ay) + oy, cx + (int)lroundf(bx) + ox, cy + (int)lroundf(by) + oy, c);
    }
  };
  if (on == 0) {
    line(x0, y0, x1, y1);
    return;
  }
  const float cycle = on + off;
  float len = hypotf(x1 - x0, y1 - y0), pos = 0.0f;
  while (pos < len) {
    float cyc = fmodf(phase, cycle), step;
    if (cyc < on) {
      step = fminf(on - cyc, len - pos);
      line(x0 + (x1 - x0) * pos / len, y0 + (y1 - y0) * pos / len, x0 + (x1 - x0) * (pos + step) / len,
           y0 + (y1 - y0) * (pos + step) / len);
    } else {
      step = fminf(cycle - cyc, len - pos);
    }
    pos += step;
    phase += step;
  }
}

void dashedCircle(int cx, int cy, int r, uint16_t c, int on, int off) {
  if (on == 0) {
    cv->drawCircle(cx, cy, r, c);
    return;
  }
  const float circ = 6.2831853f * r, cycle = on + off;
  for (float pos = 0; pos < circ; pos += 1.0f) {
    if (fmodf(pos, cycle) >= on) continue;
    float a = pos / r;
    cv->drawPixel(cx + (int)lroundf(r * sinf(a)), cy - (int)lroundf(r * cosf(a)), c);
  }
}

// ---- land fill -------------------------------------------------------------------------------

// The land outlines (MAP_LAND rings) are filled with the even-odd rule, one pixel row at a time: every
// ring edge adds its crossing to the rows it spans, then each row's sorted crossings give the spans.
constexpr int LAND_MAXC = 96;  // crossings kept per row
int16_t* landX = nullptr;
uint8_t landN[DISC_D];

void fillLand(int cx, int cy, float rangeKm) {
  if (!landX) landX = (int16_t*)heap_caps_malloc(DISC_D * LAND_MAXC * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!landX) return;
  memset(landN, 0, sizeof landN);
  const float k = MAP_UNIT_KM / rangeKm * RR;
  const int top = cy - RR;
  for (int l = 0; l < MAP_LINE_COUNT; ++l) {
    const MapLine& ln = MAP_LINES[l];
    if (ln.kind != MAP_LAND) continue;
    const int16_t* p = MAP_PTS + 2 * ln.first;
    for (int i = 1; i < ln.count; ++i) {
      float x0 = cx + p[2 * i - 2] * k, y0 = cy - p[2 * i - 1] * k, x1 = cx + p[2 * i] * k, y1 = cy - p[2 * i + 1] * k;
      float lo = fminf(y0, y1), hi = fmaxf(y0, y1);
      int r0 = max(0, (int)floorf(lo - top - 0.5f)), r1 = min(DISC_D - 1, (int)ceilf(hi - top - 0.5f));
      for (int r = r0; r <= r1; ++r) {
        float yc = top + r + 0.5f;
        if ((y0 <= yc) == (y1 <= yc)) continue;
        float x = x0 + (yc - y0) * (x1 - x0) / (y1 - y0);
        if (x < -30000.0f || x > 30000.0f || landN[r] >= LAND_MAXC) continue;
        landX[r * LAND_MAXC + landN[r]++] = (int16_t)lroundf(x);
      }
    }
  }
  for (int r = 0; r < DISC_D; ++r) {
    int16_t* xs = landX + r * LAND_MAXC;
    int n = landN[r];
    for (int i = 1; i < n; ++i) {  // insertion sort
      int16_t key = xs[i];
      int j = i - 1;
      while (j >= 0 && xs[j] > key) {
        xs[j + 1] = xs[j];
        --j;
      }
      xs[j + 1] = key;
    }
    float dy = top + r + 0.5f - cy;
    int hw = (int)sqrtf(fmaxf(0.0f, (float)RR * RR - dy * dy));
    for (int i = 0; i + 1 < n; i += 2) {
      int xa = max((int)xs[i], cx - hw), xb = min((int)xs[i + 1], cx + hw);
      if (xb > xa) cv->drawFastHLine(xa, top + r, xb - xa, T->land);
    }
  }
}

// ---- map -------------------------------------------------------------------------------------

// Province border, runways and coastline from mapdata.cpp, clipped to the radar disc.
void drawMap(int cx, int cy, float rangeKm) {
  const float k = MAP_UNIT_KM / rangeKm * RR;  // pixels per stored unit
  const uint8_t kinds[] = {MAP_BORDER, MAP_RUNWAY, MAP_COAST};  // coast last, so it stays on top
  for (uint8_t kind : kinds) {
    const uint16_t c = kind == MAP_COAST ? T->coast : (kind == MAP_BORDER ? T->border : T->runway);
    const int width = kind == MAP_COAST ? T->coastW : (kind == MAP_RUNWAY && T->style == Style::Atc ? 3 : 1);
    const int on = kind == MAP_BORDER ? T->borderOn : 0, off = kind == MAP_BORDER ? T->borderOff : 0;
    for (int l = 0; l < MAP_LINE_COUNT; ++l) {
      const MapLine& ln = MAP_LINES[l];
      if (ln.kind != kind) continue;
      const int16_t* p = MAP_PTS + 2 * ln.first;
      float phase = 0.0f;
      for (int i = 1; i < ln.count; ++i) {
        float x0 = p[2 * i - 2] * k, y0 = -p[2 * i - 1] * k, x1 = p[2 * i] * k, y1 = -p[2 * i + 1] * k;
        if (clipSeg(x0, y0, x1, y1, RR - 1)) strokeSeg(cx, cy, x0, y0, x1, y1, c, width, on, off, phase);
      }
    }
  }
}

// The longest runway from the map data, as its two end points (km east / north of home).
struct Runway {
  float ax, ay, bx, by;
  bool ok;
} rwy = {};

void findRunway() {
  float best = 0.0f;
  for (int l = 0; l < MAP_LINE_COUNT; ++l) {
    const MapLine& ln = MAP_LINES[l];
    if (ln.kind != MAP_RUNWAY || ln.count < 2) continue;
    const int16_t* p = MAP_PTS + 2 * ln.first;
    float ax = p[0] * MAP_UNIT_KM, ay = p[1] * MAP_UNIT_KM, bx = p[2 * ln.count - 2] * MAP_UNIT_KM, by = p[2 * ln.count - 1] * MAP_UNIT_KM;
    float len = hypotf(bx - ax, by - ay);
    if (len > best) {
      best = len;
      rwy = {ax, ay, bx, by, true};
    }
  }
}

// Extended centre lines of the main runway: dashed, 32 km long, a tick every 10 km (ATC theme).
void drawApproach(int cx, int cy, float rangeKm) {
  if (!rwy.ok) return;
  const float k = RR / rangeKm;
  float ux = rwy.bx - rwy.ax, uy = rwy.by - rwy.ay, len = hypotf(ux, uy);
  if (len < 0.1f) return;
  ux /= len;
  uy /= len;
  const uint16_t c = mix(T->disc, T->accent, 150);
  for (int end = 0; end < 2; ++end) {
    float sx = end ? rwy.ax : rwy.bx, sy = end ? rwy.ay : rwy.by, dx = end ? -ux : ux, dy = end ? -uy : uy;
    float x0 = sx * k, y0 = -sy * k, x1 = (sx + dx * 32.0f) * k, y1 = -(sy + dy * 32.0f) * k, phase = 0.0f;
    if (clipSeg(x0, y0, x1, y1, RR - 1)) strokeSeg(cx, cy, x0, y0, x1, y1, c, 1, 7, 5, phase);
    for (int d = 10; d <= 30; d += 10) {
      float tx = (sx + dx * d) * k, ty = -(sy + dy * d) * k;
      if (tx * tx + ty * ty > (RR - 6) * (RR - 6)) continue;
      const float nx = dy, ny = dx;  // perpendicular to the line (km north is up, screen y is down)
      cv->drawLine(cx + (int)lroundf(tx - nx * 4), cy + (int)lroundf(ty - ny * 4), cx + (int)lroundf(tx + nx * 4),
                   cy + (int)lroundf(ty + ny * 4), c);
    }
  }
}

// Everything on the radar that does not move: disc, map, range rings, crosshair, compass, home.
void drawBackground(int cx, int cy) {
  const float rangeKm = RANGES_KM[rangeIdx];
  const Fonts& F = *T->f;

  cv->fillCircle(cx, cy, RR, T->disc);
  if (T->hasLand) fillLand(cx, cy, rangeKm);
  drawMap(cx, cy, rangeKm);
  for (float km = RING_STEP_KM[rangeIdx]; km <= rangeKm + 0.1f; km += RING_STEP_KM[rangeIdx]) {
    int rr = (int)lroundf(km / rangeKm * RR);
    dashedCircle(cx, cy, rr, T->ring, T->ringOn, T->ringOff);
    char b[8];
    snprintf(b, sizeof b, "%.0f", km);
    put(b, cx + 6, cy - rr + 3, F.small, T->dim, DT_TL);
  }
  cv->drawFastHLine(cx - RR, cy, 2 * RR, T->ring);
  cv->drawFastVLine(cx, cy - RR, 2 * RR, T->ring);
  if (T->spokes) {
    for (int d = 10; d < 360; d += 10) {
      if (d % 90 == 0) continue;
      float a = d * DEG2RAD;
      cv->drawLine(cx, cy, cx + (int)lroundf(RR * sinf(a)), cy - (int)lroundf(RR * cosf(a)),
                   d % 30 == 0 ? T->ring : mix(T->disc, T->ring, 140));
    }
  }
  if (T->style == Style::Atc) drawApproach(cx, cy, rangeKm);
  cv->drawCircle(cx, cy, RR, T->line);
  for (int d = 0; d < 360; d += 10) {
    float a = d * DEG2RAD;
    int inner = (d % 30 == 0) ? RR - 12 : RR - 6;
    cv->drawLine(cx + (int)lroundf(inner * sinf(a)), cy - (int)lroundf(inner * cosf(a)),
                 cx + (int)lroundf((RR + 4) * sinf(a)), cy - (int)lroundf((RR + 4) * cosf(a)), T->line);
  }
  if (T->numbers) {
    const IFont* nf = T->numbers == 10 ? (const IFont*)&fnt::Font0 : (const IFont*)&fnt::DejaVu9;
    for (int d = 0; d < 360; d += T->numbers) {
      if (T->cardinal && d % 90 == 0) continue;
      float a = d * DEG2RAD;
      char b[6];
      snprintf(b, sizeof b, "%03d", d);
      put(b, cx + (int)lroundf((RR + 13) * sinf(a)), cy - (int)lroundf((RR + 13) * cosf(a)), nf, T->dim, DT_MC);
    }
  }
  if (T->cardinal) {
    put("N", cx, cy - RR - 12, F.bold, T->accent, DT_MC);
    put("S", cx, cy + RR + 12, F.bold, T->dim, DT_MC);
    put("E", cx + RR + 14, cy, F.bold, T->dim, DT_MC);
    put("W", cx - RR - 14, cy, F.bold, T->dim, DT_MC);
  }

  cv->drawCircle(cx, cy, 10, T->accent);
  cv->fillCircle(cx, cy, 3, T->accent);
}

// Re-renders the cached background when the range or theme changed; false if there is no cache.
bool ensureBackground() {
  if (!bgOk) return false;
  if (bgRange != rangeIdx || bgTheme != themeIdx) {
    bgRange = rangeIdx;
    bgTheme = themeIdx;
    lgfx::LGFX_Sprite* frame = cv;
    cv = &bg;
    bg.fillScreen(T->bg);
    drawBackground(BG_SIZE / 2, BG_SIZE / 2);
    cv = frame;
  }
  return true;
}

// True when the point is at the reference airport (within 3 km). Always false without a reference airport.
bool nearRef(float lat, float lon) {
  return HAS_REF_AIRPORT && gcDistKm(lat, lon, REF_AIRPORT_LAT, REF_AIRPORT_LON) < 3.0f;
}

// Looks up the route of the selected aircraft (urgently) and of the aircraft in the table rows,
// and fills selIdx / selRoute / rowRoute / planeRoute / planeRole for this frame.
void refreshRoutes(const TrafficSnapshot& t) {
  selIdx = -1;
  for (int k = 0; k < orderCount; ++k)
    if (isSelected(t.planes[order[k]])) selIdx = order[k];

  selRoute = {};
  selRoute.state = RouteInfo::State::NotFound;
  if (selIdx >= 0 && t.planes[selIdx].hasFlight) selRoute = routeLookup(t.planes[selIdx].call, true);

  auto near = [](const Airport& a) { return nearRef(a.lat, a.lon); };
  auto describe = [&](int idx, const RouteInfo& r, bool hasFlight) {
    planeRole[idx] = ROLE_OVER;
    if (!hasFlight) {
      strlcpy(planeRoute[idx], "--", sizeof planeRoute[idx]);
    } else if (r.state == RouteInfo::State::Found) {
      snprintf(planeRoute[idx], sizeof planeRoute[idx], "%s>%s", airportCode(r.from), airportCode(r.to));
      planeRole[idx] = near(r.to) ? ROLE_ARR : (near(r.from) ? ROLE_DEP : ROLE_OVER);
    } else {
      strlcpy(planeRoute[idx], r.state == RouteInfo::State::Loading ? "..." : "--", sizeof planeRoute[idx]);
    }
  };

  for (int i = 0; i < ROWS; ++i) {
    rowRoute[i][0] = 0;
    if (i >= orderCount) continue;
    const Aircraft& a = t.planes[order[i]];
    describe(order[i], a.hasFlight ? routeLookup(a.call, false) : RouteInfo{}, a.hasFlight);
    strlcpy(rowRoute[i], planeRoute[order[i]], sizeof rowRoute[i]);
  }
  if (selIdx >= 0) describe(selIdx, selRoute, t.planes[selIdx].hasFlight);
}

// Path of the selected flight: origin to aircraft solid, aircraft to destination dashed. Each end is
// marked with its airport code, or, when it is outside the radar, where the path leaves the disc.
void drawRoute(float rangeKm, const Live& ac) {
  constexpr int MAXN = 300;
  static float px[MAXN + 1], py[MAXN + 1];  // km east / north of home along the path
  const Airport &A = selRoute.from, &B = selRoute.to;
  const float total = gcDistKm(A.lat, A.lon, B.lat, B.lon);
  if (total < 5.0f) return;

  // The path runs origin -> aircraft -> destination, each leg a great circle. The scheduled great
  // circle itself can be far from where the aircraft really flies (airways, detours), so it is
  // anchored on the aircraft: what is drawn always starts at the symbol.
  auto unit = [](float lat, float lon, float* v) {
    v[0] = cosf(lat * DEG2RAD) * cosf(lon * DEG2RAD);
    v[1] = cosf(lat * DEG2RAD) * sinf(lon * DEG2RAD);
    v[2] = sinf(lat * DEG2RAD);
  };
  auto sample = [&](float lat0, float lon0, float lat1, float lon1, int first, int cnt) {  // fills first..first+cnt
    float om = gcDistKm(lat0, lon0, lat1, lon1) / 6371.0f, so = sinf(om);
    float va[3], vb[3];
    unit(lat0, lon0, va);
    unit(lat1, lon1, vb);
    for (int i = 0; i <= cnt; ++i) {
      float f = (float)i / cnt, wa = 1.0f - f, wb = f;
      if (so > 1e-4f) {  // spherical interpolation (plain blend for a degenerate leg)
        wa = sinf((1.0f - f) * om) / so;
        wb = sinf(f * om) / so;
      }
      float x = wa * va[0] + wb * vb[0], y = wa * va[1] + wb * vb[1], z = wa * va[2] + wb * vb[2];
      toKmAz(asinf(fminf(1.0f, fmaxf(-1.0f, z))) / DEG2RAD, atan2f(y, x) / DEG2RAD, px[first + i], py[first + i]);
    }
  };
  float acLat, acLon;
  fromKm(ac.x, ac.y, acLat, acLon);
  const float d1 = gcDistKm(A.lat, A.lon, acLat, acLon), d2 = gcDistKm(acLat, acLon, B.lat, B.lon);
  const int n1 = max(2, min(MAXN / 2, (int)(d1 / 10.0f))), n2 = max(2, min(MAXN / 2, (int)(d2 / 10.0f)));
  sample(A.lat, A.lon, acLat, acLon, 0, n1);
  sample(acLat, acLon, B.lat, B.lon, n1, n2);
  const int n = n1 + n2, s = n1;  // sample s is the aircraft
  px[s] = ac.x;
  py[s] = ac.y;
  const float sx = ac.x, sy = ac.y;

  const float k = RR / rangeKm;  // pixels per km
  float phase = 0.0f;
  auto seg = [&](float x0, float y0, float x1, float y1, uint16_t c, bool dashed) {
    x0 *= k; y0 *= -k; x1 *= k; y1 *= -k;
    if (!clipSeg(x0, y0, x1, y1, RR - 1)) return;
    strokeSeg(RCX, RCY, x0, y0, x1, y1, c, 2, dashed ? 9 : 0, 6, phase);
  };

  for (int i = 0; i < s; ++i) seg(px[i], py[i], px[i + 1], py[i + 1], T->routeDim, false);
  for (int i = s; i < n; ++i) seg(px[i], py[i], px[i + 1], py[i + 1], T->route, true);

  routeTouchesRef = nearRef(A.lat, A.lon) || nearRef(B.lat, B.lon);

  // Ends of the route: walk away from the aircraft until the path leaves the disc.
  auto markEnd = [&](int from, int step, int last, const Airport& ap) {
    float cx0 = sx, cy0 = sy;
    for (int i = from; i != last + step; i += step) {
      if (hypotf(px[i], py[i]) > rangeKm) {  // crossing lies between the previous point and this one
        float x0 = cx0 * k, y0 = -cy0 * k, x1 = px[i] * k, y1 = -py[i] * k;
        if (!clipSeg(x0, y0, x1, y1, RR)) return;
        float len = hypotf(x1, y1), f = len > 20.0f ? (len - 18.0f) / len : 0.0f;
        cv->fillCircle(RCX + (int)lroundf(x1), RCY + (int)lroundf(y1), 3, T->route);
        put(airportCode(ap), RCX + (int)lroundf(x1 * f), RCY + (int)lroundf(y1 * f), T->f->bold, T->route, DT_MC);
        return;
      }
      cx0 = px[i];
      cy0 = py[i];
    }
    if (nearRef(ap.lat, ap.lon)) return;  // the reference airport marker is drawn separately
    int ax = RCX + (int)lroundf(cx0 * k), ay = RCY - (int)lroundf(cy0 * k);
    cv->drawCircle(ax, ay, 5, T->route);
    put(airportCode(ap), ax + 9, ay, T->f->bold, T->route, DT_ML);
  };
  markEnd(s + 1, 1, n, B);
  markEnd(s - 1, -1, 0, A);
}

void drawTopBar(const TrafficSnapshot& t) {
  const Fonts& F = *T->f;
  cv->fillRect(0, 0, W, BAR_H, T->panel);
  cv->drawFastHLine(0, BAR_H, W, T->line);

  cv->fillCircle(18, 20, 6, !t.haveData ? C_ERR : (t.failures ? T->warn : T->ok));
  put("FLIGHTSCANNER", 34, 20, F.big, T->accent, DT_ML);

  char b[64];
#if SHOW_HOME_COORDS
  if (F.mono) snprintf(b, sizeof b, "%s %.2fN %.2fE", HOME_NAME, HOME_LAT, HOME_LON);
  else snprintf(b, sizeof b, "%s  %.4fN %.4fE", HOME_NAME, HOME_LAT, HOME_LON);
#else
  snprintf(b, sizeof b, "%s", HOME_NAME);
#endif
  put(b, 250, 20, F.small, T->dim, DT_ML);

  snprintf(b, sizeof b, "%d AC", orderCount);
  put(b, 484, 20, F.bold, T->text, DT_ML);

  snprintf(b, sizeof b, "BRT %d%%", BRIGHT[brightIdx]);
  put(b, 648, 20, F.small, T->dim, DT_MR);

  char clk[12] = "--:--:--";
  struct tm tm;
  if (getLocalTime(&tm, 0)) strftime(clk, sizeof clk, "%H:%M:%S", &tm);
  put(clk, W - 14, 20, F.big, T->text, DT_MR);
}

// ---- aircraft on the radar -------------------------------------------------------------------

struct Rect {
  int16_t x, y, w, h;
};

int overlapArea(const Rect& a, const Rect& b) {
  int w = min(a.x + a.w, b.x + b.w) - max(a.x, b.x), h = min(a.y + a.h, b.y + b.h) - max(a.y, b.y);
  return w > 0 && h > 0 ? w * h : 0;
}

constexpr int PLACED_MAX = 48;
Rect placed[PLACED_MAX];
int placedCount = 0;

void addPlaced(int x, int y, int w, int h) {
  if (placedCount < PLACED_MAX) placed[placedCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

// Spot for a w x h box beside an aircraft at (x, y): the first of eight candidate positions that stays clear
// of everything placed so far (else the one with the least overlap). The box is recorded as placed.
Rect placeBox(int w, int h, int x, int y, int gap) {
  const int cand[8][2] = {{gap, -h / 2},         {gap, -h - 2},        {gap, 2},           {-gap - w, -h / 2},
                          {-gap - w, -h - 2},    {-gap - w, 2},        {-w / 2, -h - gap}, {-w / 2, gap}};
  Rect best = {};
  int bestScore = 1 << 30;
  for (const auto& c : cand) {
    Rect r = {(int16_t)(x + c[0]), (int16_t)(y + c[1]), (int16_t)w, (int16_t)h};
    Rect probe = {(int16_t)(r.x - 2), (int16_t)(r.y - 2), (int16_t)(w + 4), (int16_t)(h + 4)};
    int score = 0;
    if (r.x < 6 || r.x + w > PX - 4 || r.y < TABLE_Y || r.y + h > FOOT_Y - 6) score += 5000;
    for (int i = 0; i < placedCount; ++i) score += overlapArea(probe, placed[i]);
    if (score < bestScore) {
      bestScore = score;
      best = r;
    }
  }
  addPlaced(best.x, best.y, best.w, best.h);
  return best;
}

// Older fixes of one aircraft as fading dots behind it.
void drawTrail(const Aircraft& a, uint16_t col, float rangeKm) {
  const Trail* tr = findTrail(a.hex);
  if (!tr) return;
  const float k = RR / rangeKm;
  for (int i = tr->n - 1, age = 0; i >= 0; --i, ++age) {
    int x = RCX + (int)lroundf(tr->x[i] * k), y = RCY - (int)lroundf(tr->y[i] * k);
    if ((x - RCX) * (x - RCX) + (y - RCY) * (y - RCY) > (RR - 4) * (RR - 4)) continue;
    uint16_t c = mix(col, T->disc, age * 200 / TRAIL_N);
    if (T->style == Style::Tag) cv->fillCircle(x, y, age < 5 ? 2 : 1, c);
    else cv->fillRect(x - 1, y - 1, 2, 2, c);
  }
}

void tagText(const Aircraft& a, int idx, char* sub, size_t cap) {
  const char* r = planeRoute[idx];
  bool haveRoute = r[0] && strcmp(r, "--") != 0 && strcmp(r, "...") != 0;
  if (a.hasAlt) snprintf(sub, cap, haveRoute ? "FL%03d  %s" : "FL%03d", (int)(a.alt / 100.0f), r);
  else snprintf(sub, cap, "FL---");
}

// Flight-tracker tag: callsign on top, level and route below, a bar in the aircraft colour on its left.
void drawTag(const Aircraft& a, int idx, const Rect& r, uint16_t col) {
  char sub[28];
  tagText(a, idx, sub, sizeof sub);
  cv->fillRoundRect(r.x, r.y, r.w, r.h, 4, RGB(0x08101c));
  cv->drawRoundRect(r.x, r.y, r.w, r.h, 4, RGB(0x3a4350));
  cv->fillRect(r.x, r.y + 3, 3, r.h - 6, col);
  put(a.call, r.x + 10, r.y + 11, T->f->bold, RGB(0xffffff), DT_ML);
  put(sub, r.x + 10, r.y + 26, T->f->small, T->dim, DT_ML);
}

// Controller data block: callsign, level and speed, route, on a leader line from the symbol.
void drawBlock(const Aircraft& a, int idx, const Rect& r, int x, int y, uint16_t col, bool sel) {
  const char* route = planeRoute[idx];
  bool haveRoute = route[0] && strcmp(route, "--") != 0 && strcmp(route, "...") != 0;
  char l2[24];
  if (a.hasAlt) snprintf(l2, sizeof l2, "FL%03d %d", (int)(a.alt / 100.0f), (int)a.gs);
  else snprintf(l2, sizeof l2, "FL--- %d", (int)a.gs);
  int lx = max((int)r.x, min(x, r.x + r.w)), ly = max((int)r.y, min(y, r.y + r.h));
  if (hypotf(lx - x, ly - y) > 8.0f) cv->drawLine(x, y, lx, ly, mix(T->disc, col, 150));
  cv->fillRect(r.x, r.y, r.w, r.h, RGB(0x080e12));
  if (sel) cv->drawRect(r.x, r.y, r.w, r.h, col);
  put(a.call, r.x + 4, r.y + 10, T->f->bold, col, DT_ML);
  put(l2, r.x + 4, r.y + 25, T->f->small, T->text, DT_ML);
  if (haveRoute) put(route, r.x + 4, r.y + 40, T->f->small, T->dim, DT_ML);
}

void drawRadar(const TrafficSnapshot& t) {
  const float rangeKm = RANGES_KM[rangeIdx];
  const Fonts& F = *T->f;

  if (ensureBackground()) bg.pushSprite(cv, BGX, BGY);
  else drawBackground(RCX, RCY);
  sweepPump();

  routeTouchesRef = false;
  if (selIdx >= 0 && selRoute.state == RouteInfo::State::Found) drawRoute(rangeKm, live[selIdx]);

  placedCount = 0;
  float ex, ny;
  toKm(REF_AIRPORT_LAT, REF_AIRPORT_LON, ex, ny);
  if (HAS_REF_AIRPORT && hypotf(ex, ny) <= rangeKm) {
    int ax = RCX + (int)lroundf(ex / rangeKm * RR), ay = RCY - (int)lroundf(ny / rangeKm * RR);
    const bool atc = T->style == Style::Atc;
    uint16_t c = routeTouchesRef ? T->route : (atc ? T->text : T->dim);
    cv->drawRect(ax - 4, ay - 4, 8, 8, c);
    put(REF_AIRPORT_CODE, ax + 9, ay + (atc ? 12 : 0), routeTouchesRef ? F.bold : F.small, c, DT_ML);
    addPlaced(ax - 6, ay - 6, 12 + (int)strlen(REF_AIRPORT_CODE) * (F.mono ? 11 : 8), atc ? 26 : 14);
  }
  addPlaced(RCX - 12, RCY - 12, 24, 24);
  addPlaced(RCX + RR + 4, RCY - 9, 20, 18);

  static int16_t sx[MAX_AC], sy[MAX_AC];
  markerCount = 0;
  for (int k = orderCount - 1; k >= 0; --k) {  // far ones first so near ones stay on top
    const Aircraft& a = t.planes[order[k]];
    const Live& p = live[order[k]];
    int px = RCX + (int)lroundf(p.x / rangeKm * RR);
    int py = RCY - (int)lroundf(p.y / rangeKm * RR);
    sx[k] = px;
    sy[k] = py;
    bool sel = isSelected(a);
    uint16_t col = planeColor(order[k], a);

    if (T->style != Style::Arrow) drawTrail(a, col, rangeKm);
    if (sel) {
      uint16_t rc = T->style == Style::Atc ? T->warn : T->text;
      cv->drawCircle(px, py, 17, rc);
      cv->drawCircle(px, py, 18, rc);
      if (T->style == Style::Atc) col = T->warn;
    }
    switch (T->style) {
      case Style::Arrow:
        dart(px, py, a.track, col, 1.0f, T->light ? (int32_t)RGB(0x0b1b29) : -1);
        break;
      case Style::Tag:
        dart(px, py, a.track, col, 1.2f, RGB(0x06101c));
        break;
      case Style::Atc: {  // diamond with a one-minute speed vector
        float len = a.gs * KNOT_KM_PER_S * 60.0f / rangeKm * RR, r = a.track * DEG2RAD;
        cv->drawLine(px, py, px + (int)lroundf(len * sinf(r)), py - (int)lroundf(len * cosf(r)), col);
        cv->fillTriangle(px, py - 5, px + 5, py, px, py + 5, col);
        cv->fillTriangle(px, py - 5, px - 5, py, px, py + 5, col);
        break;
      }
    }

    if (markerCount < MAX_AC) {
      markers[markerCount] = {(int16_t)px, (int16_t)py, {}};
      strlcpy(markers[markerCount].hex, a.hex, sizeof markers[markerCount].hex);
      ++markerCount;
    }
  }

  if (T->style == Style::Arrow) {
    // Labels: nearest first, and a label that would land on one already placed is
    // skipped (the table still lists every aircraft). The selected one always shows.
    struct Box {
      int16_t x0, y0, x1, y1;
    } boxes[8];
    int boxCount = 0;
    cv->setFont(F.small);
    for (int k = 0; k < orderCount && boxCount < 8; ++k) {
      const Aircraft& a = t.planes[order[k]];
      bool sel = isSelected(a);
      if (k >= 6 && !sel) continue;

      int px = sx[k], py = sy[k];
      bool left = px > RCX + RR - (F.mono ? 100 : 80);
      int lx = left ? px - 15 : px + 15;
      int w = max((int)cv->textWidth(a.call), a.hasAlt ? 46 : 0) + 2;
      Box box = {(int16_t)(left ? lx - w : lx), (int16_t)(py - 15), (int16_t)(left ? lx : lx + w), (int16_t)(py + 16)};

      bool clash = false;
      for (int i = 0; i < boxCount && !clash; ++i)
        clash = box.x0 < boxes[i].x1 && box.x1 > boxes[i].x0 && box.y0 < boxes[i].y1 && box.y1 > boxes[i].y0;
      if (clash && !sel) continue;
      boxes[boxCount++] = box;

      put(a.call, lx, py - 7, F.small, T->text, left ? DT_MR : DT_ML);
      if (a.hasAlt) {
        char b[10];
        snprintf(b, sizeof b, "FL%03d", (int)(a.alt / 100.0f));
        put(b, lx, py + 8, F.small, T->dim, left ? DT_MR : DT_ML);
      }
    }
  } else {
    // Every aircraft keeps its marker out of the way of the boxes, then the selected one is placed
    // first, and the nearest six after it.
    for (int k = 0; k < orderCount; ++k) addPlaced(sx[k] - 9, sy[k] - 9, 18, 18);
    int labelled = 0;
    for (int pass = 0; pass < 2; ++pass) {
      for (int k = 0; k < orderCount; ++k) {
        const Aircraft& a = t.planes[order[k]];
        bool sel = isSelected(a);
        if (pass == 0 ? !sel : (sel || labelled >= 6)) continue;
        if (pass == 1) ++labelled;
        uint16_t col = sel && T->style == Style::Atc ? T->warn : planeColor(order[k], a);
        if (T->style == Style::Tag) {
          char sub[28];
          tagText(a, order[k], sub, sizeof sub);
          int w = max(textW(F.bold, a.call), textW(F.small, sub)) + 18;
          drawTag(a, order[k], placeBox(w, 36, sx[k], sy[k], 14), col);
        } else {
          const char* route = planeRoute[order[k]];
          bool haveRoute = route[0] && strcmp(route, "--") != 0 && strcmp(route, "...") != 0;
          char l2[24];
          snprintf(l2, sizeof l2, "FL%03d %d", a.hasAlt ? (int)(a.alt / 100.0f) : 0, (int)a.gs);
          int w = max(max(textW(F.bold, a.call), textW(F.small, l2)), haveRoute ? textW(F.small, route) : 0) + 8;
          drawBlock(a, order[k], placeBox(w, haveRoute ? 49 : 34, sx[k], sy[k], 22), sx[k], sy[k], col, sel);
        }
      }
    }
  }

  if (!t.haveData) {
    put("No flight data", RCX, RCY - 60, F.big, T->warn, DT_MC);
    put(t.lastError, RCX, RCY - 32, F.small, T->text, DT_MC);
    putWrapped(t.wifiInfo, RCX, RCY - 6, F.mono ? 28 : 34, 20, F.small, T->dim);
  }
}

void panelBox(int x, int y, int w, int h) {
  cv->fillRoundRect(x, y, w, h, 6, T->panel);
  if (T->outline) cv->drawRoundRect(x, y, w, h, 6, T->panelLine);
}

void drawTable(const TrafficSnapshot& t) {
  const Fonts& F = *T->f;
  panelBox(PX, TABLE_Y, PW, ROW_Y0 + ROWS * ROW_H - TABLE_Y + 4);
  put("CALL", PX + 22, 62, F.small, T->dim, DT_ML);
  put("ROUTE", PX + 112, 62, F.small, T->dim, DT_ML);
  put("KM", PX + 246, 62, F.small, T->dim, DT_MR);
  put("ALT", PX + 310, 62, F.small, T->dim, DT_MR);
  cv->drawFastHLine(PX + 6, 75, PW - 12, T->line);

  rowCount = orderCount < ROWS ? orderCount : ROWS;
  for (int i = 0; i < rowCount; ++i) {
    const Aircraft& a = t.planes[order[i]];
    float dist = live[order[i]].d;
    int cy = ROW_Y0 + i * ROW_H + ROW_H / 2;
    uint16_t col = planeColor(order[i], a);
    if (isSelected(a)) cv->fillRoundRect(PX + 3, cy - ROW_H / 2 + 1, PW - 6, ROW_H - 2, 4, T->sel);
    if (T->style == Style::Atc) cv->fillRect(PX + 7, cy - 9, 3, 18, col);
    else cv->fillRect(PX + 8, cy - 4, 8, 8, col);
    put(a.call, PX + 22, cy, F.bold, T->style == Style::Atc ? col : T->text, DT_ML);
    put(rowRoute[i], PX + 112, cy, F.small, strchr(rowRoute[i], '>') ? T->text : T->dim, DT_ML);
    char b[12];
    snprintf(b, sizeof b, dist < 100.0f ? "%.1f" : "%.0f", dist);
    put(b, PX + 246, cy, F.small, T->text, DT_MR);
    if (a.hasAlt) snprintf(b, sizeof b, "%d", (int)a.alt);
    else strlcpy(b, "--", sizeof b);
    put(b, PX + 310, cy, F.small, T->text, DT_MR);
    strlcpy(rowHex[i], a.hex, sizeof rowHex[i]);
  }
  if (t.haveData && orderCount == 0) put("No traffic in range", PX + PW / 2, 170, F.small, T->dim, DT_MC);
}

// Colour legend, shown while no aircraft is selected.
void drawCard() {
  const Fonts& F = *T->f;
  panelBox(PX, CARD_Y, PW, CARD_H);
  const int xs[] = {PX + 8, PX + 108, PX + 218};
  if (T->legend == Legend::Ramp) {
    const int x0 = PX + 14, w = PW - 28;
    for (int i = 0; i < w; ++i) cv->fillRect(x0 + i, CARD_Y + 14, 1, 10, rampColor(40000.0f * i / (w - 1)));
    put("0", x0, CARD_Y + 38, F.small, T->dim, DT_ML);
    put("10k", x0 + w / 4, CARD_Y + 38, F.small, T->dim, DT_MC);
    put("20k", x0 + w / 2, CARD_Y + 38, F.small, T->dim, DT_MC);
    put("30k", x0 + w * 3 / 4, CARD_Y + 38, F.small, T->dim, DT_MC);
    put("40k ft", x0 + w, CARD_Y + 38, F.small, T->dim, DT_MR);
  } else if (T->legend == Legend::Roles) {
    char arr[24], dep[24];
    snprintf(arr, sizeof arr, F.mono ? "ARR %s" : "arrives %s", REF_AIRPORT_CODE);
    snprintf(dep, sizeof dep, F.mono ? "DEP %s" : "leaves %s", REF_AIRPORT_CODE);
    const char* ls[] = {arr, dep, F.mono ? "OTHER" : "other"};
    const uint16_t cs[] = {ROLE_COLOR[ROLE_ARR], ROLE_COLOR[ROLE_DEP], ROLE_COLOR[ROLE_OVER]};
    for (int i = HAS_REF_AIRPORT ? 0 : 2; i < 3; ++i) {  // without a reference airport only "other" exists
      const int x = HAS_REF_AIRPORT ? xs[i] : xs[0];
      cv->fillRect(x, CARD_Y + 20, 10, 10, cs[i]);
      put(ls[i], x + 16, CARD_Y + 25, F.small, T->text, DT_ML);
    }
  } else {
    const uint16_t cs[] = {T->alt[0], T->alt[1], T->alt[2]};
    const char* ls[] = {F.mono ? "<10k" : "< 10k ft", F.mono ? "10-25k" : "10-25k ft", F.mono ? ">25k" : "> 25k ft"};
    for (int i = 0; i < 3; ++i) {
      cv->fillRect(xs[i], CARD_Y + 20, 10, 10, cs[i]);
      put(ls[i], xs[i] + 16, CARD_Y + 25, F.small, T->text, DT_ML);
    }
  }
  const char* hint[] = {"Tap an aircraft for route and model", "Tap a plane for route/model", "Tap for route/model"};
  putFirstFit(hint, 3, PX + PW / 2, CARD_Y + 62, PW - 12, F.small, T->dim);
}

// Replaces the table and legend while an aircraft is selected: make/model, route with progress,
// and the numbers.
void drawDetail(const TrafficSnapshot& t) {
  const Aircraft& a = t.planes[selIdx];
  const Live& p = live[selIdx];
  const Fonts& F = *T->f;
  const IFont* small = F.small;
  const IFont* bold = F.bold;
  const int x0 = PX + 12, x1 = PX + PW - 12;
  char b[64];

  panelBox(PX, TABLE_Y, PW, CARD_Y + CARD_H - TABLE_Y);

  put(a.call, x0, 66, F.huge, planeColor(selIdx, a), DT_ML);
  if (selRoute.airline[0]) putClip(selRoute.airline, x0, 92, x1 - x0, small, T->dim, DT_ML);
  else if (a.reg[0]) put(a.reg, x0, 92, small, T->dim, DT_ML);
  cv->drawFastHLine(PX + 6, 106, PW - 12, T->line);

  put("AIRCRAFT", x0, 120, small, T->dim, DT_ML);
  putFit(a.desc[0] ? a.desc : (a.type[0] ? a.type : "Unknown"), x0, 142, x1 - x0, T->text);
  snprintf(b, sizeof b, "Type %s    Reg %s", a.type[0] ? a.type : "--", a.reg[0] ? a.reg : "--");
  putClip(b, x0, 164, x1 - x0, small, T->dim, DT_ML);
  cv->drawFastHLine(PX + 6, 178, PW - 12, T->line);

  // Route: FROM -- plane -- TO, with the cities underneath and distance / ETA below that.
  float lat, lon;
  fromKm(p.x, p.y, lat, lon);
  if (selRoute.state == RouteInfo::State::Found) {
    const Airport &A = selRoute.from, &B = selRoute.to;
    put(airportCode(A), x0, 202, F.huge, T->text, DT_ML);
    put(airportCode(B), x1, 202, F.huge, T->text, DT_MR);
    // Under each code, in this order: country, city, short airport name. Each side may use what the
    // other leaves (a long country name in a monospaced font needs more than half).
    struct Place {
      char country[24], city[26], name[20];
    } PA, PB;
    auto fill = [](Place& p, const Airport& ap) {
      strlcpy(p.country, ap.country, sizeof p.country);
      strlcpy(p.city, ap.city, sizeof p.city);
      strlcpy(p.name, ap.name, sizeof p.name);
    };
    fill(PA, A);
    fill(PB, B);
    auto widest = [&](const Place& p) { return max(textW(small, p.country), max(textW(bold, p.city), textW(small, p.name))); };
    const int avail = x1 - x0 - 8;
    auto tooWide = [&]() { return widest(PA) + widest(PB) > avail; };
    // Two long places side by side (a monospaced font needs 11 px a letter): "Airport" becomes "Apt", then
    // goes, then long country names get their short form.
    auto trimName = [](char* n, bool drop) {
      size_t len = strlen(n);
      if (len > 8 && strcmp(n + len - 8, " Airport") == 0) {
        if (drop) n[len - 8] = 0;
        else strcpy(n + len - 8, " Apt");
      } else if (drop && len > 4 && strcmp(n + len - 4, " Apt") == 0) {
        n[len - 4] = 0;
      }
    };
    auto shortCountry = [](char* c) {
      static const char* const LONG_SHORT[][2] = {{"United Kingdom", "UK"},         {"United States", "USA"},
                                                  {"United Arab Emirates", "UAE"}, {"Czech Republic", "Czechia"},
                                                  {"North Macedonia", "N. Macedonia"}, {"Bosnia and Herzegovina", "Bosnia"}};
      for (const auto& e : LONG_SHORT)
        if (strcmp(c, e[0]) == 0) strcpy(c, e[1]);
    };
    if (tooWide()) {
      trimName(PA.name, false);
      trimName(PB.name, false);
    }
    if (tooWide()) {
      trimName(PA.name, true);
      trimName(PB.name, true);
    }
    if (tooWide()) {
      shortCountry(PA.country);
      shortCountry(PB.country);
    }
    const int wl = widest(PA), wr = widest(PB);
    auto cap = [&](int own, int other) { return own + other <= avail ? own : max(avail / 2, avail - other); };
    auto where = [&](const Place& p, bool right, int colW) {
      const int x = right ? x1 : x0;
      const uint8_t datum = right ? DT_MR : DT_ML;
      putClip(p.country, x, 228, colW, small, T->dim, datum);
      putClip(p.city, x, 246, colW, bold, T->text, datum);
      putClip(p.name, x, 264, colW, small, T->accent, datum);
    };
    where(PA, false, cap(wl, wr));
    where(PB, true, cap(wr, wl));

    float done = gcDistKm(A.lat, A.lon, lat, lon), left = gcDistKm(lat, lon, B.lat, B.lon);
    float f = done + left > 1.0f ? done / (done + left) : 0.0f;
    // A callsign's listed route is not always today's flight (reused callsigns, diversions): if the
    // aircraft is far off that route, say so instead of showing a confident ETA.
    bool offRoute = done + left > 1.25f * gcDistKm(A.lat, A.lon, B.lat, B.lon) + 60.0f;
    const int tx0 = PX + 96, tx1 = PX + PW - 96, ty = 202, fx = tx0 + (int)lroundf(f * (tx1 - tx0));
    cv->drawFastHLine(tx0, ty, tx1 - tx0, T->line);
    cv->drawFastHLine(tx0, ty + 1, tx1 - tx0, T->line);
    cv->fillRect(tx0, ty, fx - tx0, 2, T->route);
    cv->fillCircle(tx0, ty, 3, T->route);
    cv->drawCircle(tx1, ty, 3, T->line);
    dart(fx, ty, 90.0f, T->route);

    snprintf(b, sizeof b, "%s%d km to go", offRoute ? "~" : "", (int)left);
    put(b, x0, 286, bold, T->text, DT_ML);
    if (offRoute) {
      put("route unsure", x1, 286, bold, T->warn, DT_MR);
    } else if (a.gs >= 80.0f) {
      float mins = left / (a.gs * 1.852f) * 60.0f;
      struct tm tm;
      if (getLocalTime(&tm, 0)) {
        time_t when = mktime(&tm) + (time_t)(mins * 60.0f);
        struct tm at;
        localtime_r(&when, &at);
        snprintf(b, sizeof b, "ETA %02d:%02d", at.tm_hour, at.tm_min);
        put(b, x1, 286, bold, T->route, DT_MR);
      }
    }
  } else {
    const char* msg = !a.hasFlight ? "No flight number broadcast"
                      : selRoute.state == RouteInfo::State::Loading ? "Looking up route..."
                                                                     : "No route on record";
    put(msg, PX + PW / 2, 236, small, T->dim, DT_MC);
  }
  cv->drawFastHLine(PX + 6, 300, PW - 12, T->line);

  // Numbers: two columns of label / value.
  auto stat = [&](bool right, int y, const char* label, const char* value) {
    int lx = right ? PX + PW / 2 + 10 : x0, vx = right ? x1 : PX + PW / 2 - 10;
    put(label, lx, y, small, T->dim, DT_ML);
    put(value, vx, y, bold, T->text, DT_MR);
  };
  float brg = atan2f(p.x, p.y) / DEG2RAD;
  if (brg < 0) brg += 360.0f;
  if (a.hasAlt) snprintf(b, sizeof b, "%d ft", (int)a.alt);
  else strlcpy(b, "--", sizeof b);
  stat(false, 318, "ALT", b);
  snprintf(b, sizeof b, "%d kt", (int)a.gs);
  stat(true, 318, "SPEED", b);
  snprintf(b, sizeof b, "%.1f km", p.d);
  stat(false, 345, "DIST", b);
  snprintf(b, sizeof b, "%d", (int)brg);
  stat(true, 345, "BEARING", b);
  snprintf(b, sizeof b, "%d", (int)a.track);
  stat(false, 372, "TRACK", b);
  strlcpy(b, a.hex, sizeof b);
  for (char* c = b; *c; ++c)
    if (*c >= 'a' && *c <= 'z') *c -= 32;
  stat(true, 372, "HEX", b);
}

void drawButtons() {
  const Fonts& F = *T->f;
  for (int i = 0; i <= RANGE_COUNT; ++i) {
    char b[12];
    bool active = i < RANGE_COUNT && i == rangeIdx;
    if (i < RANGE_COUNT) snprintf(b, sizeof b, F.mono ? "%.0fkm" : "%.0f km", RANGES_KM[i]);
    else strlcpy(b, "REFRESH", sizeof b);
    if (active) {
      cv->fillRoundRect(BTNS[i].x, BTN_Y, BTNS[i].w, BTN_H, 8, T->accent);
    } else {
      cv->fillRoundRect(BTNS[i].x, BTN_Y, BTNS[i].w, BTN_H, 8, T->panel);
      cv->drawRoundRect(BTNS[i].x, BTN_Y, BTNS[i].w, BTN_H, 8, T->btnBorder);
    }
    put(b, BTNS[i].x + BTNS[i].w / 2, BTN_Y + BTN_H / 2, F.bold, active ? T->btnText : T->text, DT_MC);
  }
}

void drawFooter(const TrafficSnapshot& t) {
  const Fonts& F = *T->f;
  cv->fillRect(0, FOOT_Y, W, H - FOOT_Y, T->panel);
  char b[96];
  int cy = FOOT_Y + (H - FOOT_Y) / 2;
  if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof b, "WiFi %d dBm", WiFi.RSSI());
  else strlcpy(b, "WiFi off", sizeof b);
  char age[16] = "--";
  if (t.haveData) snprintf(age, sizeof age, "%us", (unsigned)((millis() - t.fetchMs) / 1000));

  const char* right = "(c) OpenStreetMap";
  const IFont* rightFont = F.small;
  uint16_t rightColor = T->dim;
  char toast[40];
  if (debugLabel[0]) {
    right = debugLabel;
    rightFont = F.bold;
    rightColor = T->warn;
  } else if ((int32_t)(toastUntil - millis()) > 0) {
    snprintf(toast, sizeof toast, "THEME %s", T->name);
    right = toast;
    rightFont = F.bold;
    rightColor = T->accent;
  } else if (t.failures && t.lastError[0]) {
    right = t.lastError;
    rightColor = T->warn;
  }

  char line[128];
  snprintf(line, sizeof line, "Data %s old   |   %s   |   Heap %uK  PSRAM %uK", age, b, (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));
  if (textW(F.small, line) > W - 30 - textW(rightFont, right))
    snprintf(line, sizeof line, "Data %s | %s | Heap %uK PSRAM %uK", age, b, (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024));
  put(line, 10, cy, F.small, T->dim, DT_ML);
  put(right, W - 10, cy, rightFont, rightColor, DT_MR);
}

// ---- phosphor theme: scanlines and the radar sweep ---------------------------------------------

// The sweep is not drawn with the frame. After each full frame the radar disc is copied to `rad` (frame
// as drawn, without the sweep); then, every few milliseconds, the pixels in the sweep wedge are
// recomputed from `rad` (blended towards the sweep colour, brightest at the arm) directly in the frame on
// screen, and pixels the wedge has left are put back. `ang` is the bearing step (0..255, clockwise from
// north) of each disc pixel.
constexpr int SWEEP_MS = 5000;     // one turn
constexpr int SWEEP_TRAIL = 56;    // wedge length in bearing steps, about 79 degrees
uint16_t* rad = nullptr;
uint8_t* ang = nullptr;
uint16_t halfW[DISC_D];
uint8_t sweepAlpha[SWEEP_TRAIL];   // 0..32
bool radValid = false;             // rad matches the frame on screen
int sweepPrev = -1;                // bearing step of the arm in the frame on screen

inline uint16_t blend565(uint16_t bgc, uint16_t fgc, uint32_t a) {  // a 0..32
  uint32_t b = (bgc | (bgc << 16)) & 0x07E0F81F, f = (fgc | (fgc << 16)) & 0x07E0F81F;
  uint32_t r = ((((f - b) * a) >> 5) + b) & 0x07E0F81F;
  return (uint16_t)(r | (r >> 16));
}

void initSweep() {
  rad = (uint16_t*)heap_caps_malloc(DISC_D * DISC_D * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  ang = (uint8_t*)heap_caps_malloc(DISC_D * DISC_D, MALLOC_CAP_SPIRAM);
  if (!rad || !ang) {
    Serial.println("[ui] no PSRAM for the sweep animation, phosphor theme will not sweep");
    return;
  }
  for (int y = 0; y < DISC_D; ++y) {
    float dy = y - RR + 0.5f;
    halfW[y] = (uint16_t)sqrtf(fmaxf(0.0f, (float)RR * RR - dy * dy));
    for (int x = 0; x < DISC_D; ++x) {
      float b = atan2f(x - RR + 0.5f, -dy);  // clockwise from north
      if (b < 0) b += 6.2831853f;
      ang[y * DISC_D + x] = (uint8_t)((int)(b / 6.2831853f * 256.0f) & 255);
    }
  }
  for (int i = 0; i < SWEEP_TRAIL; ++i) sweepAlpha[i] = i == 0 ? 30 : (i == 1 ? 18 : (uint8_t)lroundf(12.0f * (1.0f - (float)i / SWEEP_TRAIL)));
}

int sweepNow() { return (int)((millis() % SWEEP_MS) * 256UL / SWEEP_MS); }

// Moves the wedge in `fb` from bearing step `prev` (-1: none drawn) to `cur`. Only the bounding box of the
// sector the wedge covers (old and new position) is scanned.
void sweepApply(uint16_t* fb, int cur, int prev) {
  const uint16_t sc = T->sweepColor;
  const int from = (prev >= 0 ? prev : cur) - SWEEP_TRAIL;
  const int len = SWEEP_TRAIL + (prev >= 0 ? ((cur - prev) & 255) : 0);
  int boxX0 = 0, boxX1 = DISC_D, boxY0 = 0, boxY1 = DISC_D;
  if (len < 200) {
    float minx = 0, maxx = 0, miny = 0, maxy = 0;
    auto add = [&](int step) {
      float a = step * (6.2831853f / 256.0f), x = RR * sinf(a), y = -RR * cosf(a);
      minx = fminf(minx, x);
      maxx = fmaxf(maxx, x);
      miny = fminf(miny, y);
      maxy = fmaxf(maxy, y);
    };
    add(from);
    add(from + len);
    for (int q = 0; q < 256; q += 64)  // north, east, south, west lie inside the sector?
      if (((q - from) & 255) <= len) add(q);
    boxX0 = max(0, RR + (int)floorf(minx) - 1);
    boxX1 = min(DISC_D, RR + (int)ceilf(maxx) + 2);
    boxY0 = max(0, RR + (int)floorf(miny) - 1);
    boxY1 = min(DISC_D, RR + (int)ceilf(maxy) + 2);
  }
  for (int y = boxY0; y < boxY1; ++y) {
    const int xs = max(RR - (int)halfW[y], boxX0), xe = min(RR + (int)halfW[y], boxX1);
    const uint16_t* r = rad + y * DISC_D;
    const uint8_t* a = ang + y * DISC_D;
    uint16_t* d = fb + (DISC_Y + y) * W + DISC_X;
    const bool dimRow = T->crt && (DISC_Y + y) % 3 == 2;
    for (int x = xs; x < xe; ++x) {
      int age = (cur - a[x]) & 255;
      if (age < SWEEP_TRAIL) {
        uint32_t al = sweepAlpha[age];
        if (dimRow) al = al * 3 / 4;
        d[x] = blend565(r[x], sc, al);
      } else if (prev >= 0 && ((prev - a[x]) & 255) < SWEEP_TRAIL) {
        d[x] = r[x];
      }
    }
  }
}

// Called between the stages of a frame and from loop(): advances the wedge on screen.
void sweepStep() {
  if (!T->sweep || !radValid) return;
  int cur = sweepNow();
  if (cur == sweepPrev) return;
  uint32_t u0 = micros();
  sweepApply(board::frameBuffer(board::backIndex() ^ 1), cur, sweepPrev);
  sweepUs = micros() - u0;
  sweepPrev = cur;
  lastSweepMs = millis();
}

// Same, but at most every 30 ms: called from the drawing code (put()) so the arm keeps moving while a
// frame is being drawn.
void sweepPump() {
  if (T->sweep && radValid && millis() - lastSweepMs >= 30) sweepStep();
}

// CRT scanlines: every third pixel row at three quarters brightness.
void scanlines(uint16_t* fb) {
  for (int y = 2; y < H; y += 3) {
    uint32_t* row = (uint32_t*)(fb + y * W);
    for (int i = 0; i < W / 2; ++i) {
      uint32_t w = row[i];
      row[i] = ((w >> 1) & 0x7BEF7BEF) + ((w >> 2) & 0x39E739E7);
    }
  }
}

// Last step of a frame, on the back buffer before it is presented. Returns the arm position it drew.
int finishFrame() {
  uint16_t* fb = board::frameBuffer(board::backIndex());
  if (T->crt) scanlines(fb);
  if (!T->sweep || !rad || !ang) return -1;
  for (int y = 0; y < DISC_D; ++y) memcpy(rad + y * DISC_D, fb + (DISC_Y + y) * W + DISC_X, DISC_D * sizeof(uint16_t));
  int cur = sweepNow();
  sweepApply(fb, cur, -1);  // the rest of the back buffer already equals `rad`
  return cur;
}

void setTheme(int idx, bool remember) {
  if (idx < 0 || idx >= THEME_COUNT) return;
  themeIdx = idx;
  T = &THEMES[idx];
  radValid = false;
  sweepPrev = -1;
  toastUntil = millis() + 3000;
  if (remember) {
    Preferences prefs;
    if (prefs.begin("radar", false)) {
      prefs.putUChar("theme", (uint8_t)idx);
      prefs.end();
    }
  }
}

}  // namespace

void setDebugLabel(const char* text) { strlcpy(debugLabel, text, sizeof debugLabel); }

int themeCount() { return THEME_COUNT; }
int themeIndex() { return themeIdx; }
const char* themeName(int index) { return index >= 0 && index < THEME_COUNT ? THEMES[index].name : ""; }
void requestTheme(int index) {
  if (index >= 0 && index < THEME_COUNT) pendingTheme = index;
}

void begin() {
  for (int i = 0; i < 2; ++i) {
    spr[i].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);  // the panel scans out native RGB565
    spr[i].setBuffer(board::frameBuffer(i), W, H);
  }
  bg.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  bg.setPsram(true);
  bgOk = bg.createSprite(BG_SIZE, BG_SIZE) != nullptr;
  Serial.printf("[ui] background sprite %dx%d: %s\n", BG_SIZE, BG_SIZE, bgOk ? "ok" : "FAILED, drawing the radar directly");

  findRunway();
  initSweep();

  Preferences prefs;
  int saved = 0;
  if (prefs.begin("radar", false)) {
    saved = prefs.getUChar("theme", 0);
    prefs.end();
  }
  if (saved < 0 || saved >= THEME_COUNT) saved = 0;
  themeIdx = saved;
  T = &THEMES[saved];
  Serial.printf("[ui] theme %s\n", T->name);
}

namespace {
volatile int queuedX = 0, queuedY = 0;
volatile bool tapQueued = false;
}  // namespace

void queueTap(int x, int y) {
  queuedX = x;
  queuedY = y;
  tapQueued = true;
}

bool takeQueuedTap(int& x, int& y) {
  if (!tapQueued) return false;
  x = queuedX;
  y = queuedY;
  tapQueued = false;
  return true;
}

void tick() { sweepStep(); }

void draw(const TrafficSnapshot& t) {
  uint32_t t0 = millis();
  int want = pendingTheme;
  if (want >= 0) {
    pendingTheme = -1;
    if (want != themeIdx) setTheme(want, true);
  }

  cv = &spr[board::backIndex()];
  cv->fillScreen(T->bg);
  computeLive(t);
  updateTrails(t);
  refreshRoutes(t);
  uint32_t t1 = millis();
  drawRadar(t);
  sweepStep();
  uint32_t t2 = millis();
  if (selIdx >= 0) {
    rowCount = 0;  // the detail panel covers the table
    drawDetail(t);
  } else {
    drawTable(t);
    drawCard();
  }
  sweepStep();
  uint32_t t3 = millis();
  drawButtons();
  drawTopBar(t);
  drawFooter(t);
  uint32_t t4 = millis();
  int arm = finishFrame();
  uint32_t t5 = millis();
  board::present();
  sweepPrev = arm;
  radValid = arm >= 0;
  perfMs[0] = t1 - t0;
  perfMs[1] = t2 - t1;
  perfMs[2] = t3 - t2;
  perfMs[3] = t4 - t3;
  perfMs[4] = t5 - t4;
  perfMs[5] = millis() - t5;

  lastFrameMs = millis() - t0;
  if (lastFrameMs > maxFrameMs) maxFrameMs = lastFrameMs;
  static int logged = 0;
  if (logged < 3) {
    Serial.printf("[ui] frame %u ms\n", (unsigned)lastFrameMs);
    ++logged;
  }
}

void frameTimes(uint32_t& lastMs, uint32_t& maxMs) {
  lastMs = lastFrameMs;
  maxMs = maxFrameMs;
}

const char* perfSummary() {
  static char b[96];
  snprintf(b, sizeof b, "prep=%u,radar=%u,panel=%u,chrome=%u,finish=%u,present=%u,sweep_us=%u", (unsigned)perfMs[0], (unsigned)perfMs[1],
           (unsigned)perfMs[2], (unsigned)perfMs[3], (unsigned)perfMs[4], (unsigned)perfMs[5], (unsigned)sweepUs);
  return b;
}

Action tap(int x, int y) {
  if (y < BAR_H) {
    if (x < TITLE_TAP_X) {
      setTheme((themeIdx + 1) % THEME_COUNT, true);
      return Action::Redraw;
    }
    if (x < BRIGHT_TAP_X) return Action::None;
    brightIdx = (brightIdx + 1) % (int)(sizeof(BRIGHT) / sizeof(BRIGHT[0]));
    board::setBacklight(BRIGHT[brightIdx]);
    return Action::Redraw;
  }

  if (y >= BTN_Y && y < BTN_Y + BTN_H) {
    for (int i = 0; i <= RANGE_COUNT; ++i) {
      if (x < BTNS[i].x || x >= BTNS[i].x + BTNS[i].w) continue;
      if (i == RANGE_COUNT) return Action::Refresh;
      rangeIdx = i;
      return Action::Redraw;
    }
    return Action::None;
  }

  const char* hit = nullptr;
  if (x >= PX) {
    if (selIdx >= 0) {  // detail panel up: a tap on it goes back to the list
      selectedHex[0] = 0;
      return Action::Redraw;
    }
    int row = (y - ROW_Y0) / ROW_H;
    if (y >= ROW_Y0 && row < rowCount) hit = rowHex[row];
    else return Action::None;
  } else {
    int best = 30 * 30;  // pixels squared
    for (int i = 0; i < markerCount; ++i) {
      int dx = markers[i].x - x, dy = markers[i].y - y;
      if (dx * dx + dy * dy < best) {
        best = dx * dx + dy * dy;
        hit = markers[i].hex;
      }
    }
  }

  if (hit && strcmp(hit, selectedHex) != 0) strlcpy(selectedHex, hit, sizeof selectedHex);
  else selectedHex[0] = 0;  // tap on the selected one, or on empty sky, clears it
  return Action::Redraw;
}

}  // namespace ui
