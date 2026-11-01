#pragma once
#include "traffic.h"

namespace ui {

enum class Action { None, Redraw, Refresh };

// Wraps the two panel framebuffers as drawing surfaces. Call after board::begin().
void begin();

// Renders one frame into the back buffer and presents it.
void draw(const TrafficSnapshot& traffic);

// Cheap per-loop animation between frames (the radar sweep of the phosphor theme).
void tick();

// Duration of the last draw() in ms, longest since boot (for /status).
void frameTimes(uint32_t& lastMs, uint32_t& maxMs);

// Where the last frame's time went, for /status: "prep=..,radar=..,panel=..,chrome=..,finish=..,present=..,sweep_us=..".
const char* perfSummary();

// Short text shown at the right end of the footer (debug phase name); "" clears it.
void setDebugLabel(const char* text);

// Colour themes: tapping FLIGHTSCANNER cycles them, the choice is kept over reboots.
// requestTheme() may be called from any task; the switch happens at the next draw().
int themeCount();
int themeIndex();
const char* themeName(int index);
void requestTheme(int index);

// Handles a touch-down at (x, y).
Action tap(int x, int y);

// Thread-safe way for other tasks (the remote monitor) to inject a tap; loop()
// picks it up with takeQueuedTap().
void queueTap(int x, int y);
bool takeQueuedTap(int& x, int& y);

}  // namespace ui
