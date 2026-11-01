#pragma once
#include <stdint.h>

// CrowPanel Advance 4.3" (800x480 RGB panel, GT911 touch, STC8H backlight MCU).
namespace board {

constexpr int WIDTH = 800;
constexpr int HEIGHT = 480;

// Brings up I2C, backlight MCU, touch and the RGB panel. Returns false if the
// backlight MCU or touch controller did not answer (the panel still runs).
bool begin();

// The two scan-out framebuffers (native RGB565). Draw into frameBuffer(backIndex()),
// then call present() to show it.
uint16_t* frameBuffer(int index);
int backIndex();
void present();

// 0..100 percent.
void setBacklight(uint8_t percent);

// Changes the RGB pixel clock on the fly (debugging aid). Returns false on error.
bool setPixelClock(uint32_t hz);

// Scan-out health: a frame is "bad" when the display DMA did not see exactly one
// refill interrupt per bounce buffer (a late interrupt; the picture can shift).
struct ScanoutStats {
  uint32_t vsyncs;
  uint32_t badFrames;
  uint32_t lastBadCount;
  bool resyncActive;  // false if the driver's counter could not be located
};
ScanoutStats scanoutStats();

// True while a finger is down; x/y keep the last position.
bool readTouch(int& x, int& y);

}  // namespace board
