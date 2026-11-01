#pragma once

// Switches the remote monitor's "dbg" action flips (see monitor.cpp), used to
// isolate display glitches: is it the redraw, the network, the pixel clock?
namespace dbg {

extern volatile bool redrawPaused;  // loop() stops redrawing the screen
extern volatile bool drawNow;       // one extra frame even while paused (shows the new label)

}  // namespace dbg
