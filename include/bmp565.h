// Encodes an RGB565 pixel array (the format of nearly every small display driven through
// GFXcanvas16 / Adafruit_GFX / LovyanGFX: ILI9341, ST7789, GC9A01, RGB panels, ...) as a
// 24-bit BMP file. It does not depend on any display driver, it only takes the raw pixels.
//
// IMPORTANT: the input must be native (little-endian) RGB565, i.e. the pixels as they were
// before any byte swap done for SPI transfers (GFXcanvas16::getBuffer() returns them that
// way). Byte-swapped data produces wrong colours.
#pragma once
#include <Arduino.h>

namespace Bmp565 {

// Minimum output buffer size needed for the given width and height.
size_t bmpSizeFor(int width, int height);

// 'pixels': width*height uint16_t RGB565 values, row by row (left to right, top to bottom).
// 'out'/'cap': output buffer and its capacity (must be >= bmpSizeFor()). Returns true on
// success, outLen then holds the real size of the BMP.
bool encode(const uint16_t *pixels, int width, int height, uint8_t *out, size_t cap, size_t &outLen);

}  // namespace Bmp565
