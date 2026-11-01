#include "bmp565.h"

namespace Bmp565 {

size_t bmpSizeFor(int width, int height) {
    size_t rowSize = (((size_t)width * 3 + 3) & ~(size_t)3);  // satir 4 bayta yuvarlanir (BMP kurali)
    return 54 + rowSize * (size_t)height;
}

bool encode(const uint16_t *pixels, int width, int height, uint8_t *out, size_t cap, size_t &outLen) {
    if (!pixels || !out || width <= 0 || height <= 0) return false;
    const size_t rowSize = (((size_t)width * 3 + 3) & ~(size_t)3);
    const size_t imgSize = rowSize * (size_t)height;
    const size_t total = 54 + imgSize;
    if (cap < total) return false;

    uint8_t *p = out;
    auto put16 = [&](uint16_t v) { *p++ = (uint8_t)(v & 0xFF); *p++ = (uint8_t)((v >> 8) & 0xFF); };
    auto put32 = [&](uint32_t v) {
        *p++ = (uint8_t)(v & 0xFF); *p++ = (uint8_t)((v >> 8) & 0xFF);
        *p++ = (uint8_t)((v >> 16) & 0xFF); *p++ = (uint8_t)((v >> 24) & 0xFF);
    };

    // BITMAPFILEHEADER (14 bayt) + BITMAPINFOHEADER (40 bayt), 24bpp, BI_RGB, alttan-yukari.
    *p++ = 'B'; *p++ = 'M';
    put32((uint32_t)total);
    put32(0);
    put32(54);
    put32(40);
    put32((uint32_t)width);
    put32((uint32_t)height);
    put16(1);
    put16(24);
    put32(0);
    put32((uint32_t)imgSize);
    put32(0); put32(0);
    put32(0); put32(0);

    for (int y = height - 1; y >= 0; y--) {
        const uint16_t *row = pixels + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            uint16_t px = row[x];
            uint8_t r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
            *p++ = (uint8_t)((b5 << 3) | (b5 >> 2));  // B
            *p++ = (uint8_t)((g6 << 2) | (g6 >> 4));  // G
            *p++ = (uint8_t)((r5 << 3) | (r5 >> 2));  // R
        }
        for (size_t i = 0; i < rowSize - (size_t)width * 3; i++) *p++ = 0;  // satir dolgusu
    }
    outLen = total;
    return true;
}

}  // namespace Bmp565
