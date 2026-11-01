#include "board.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace board {
namespace {

constexpr int PIN_SDA = 15;
constexpr int PIN_SCL = 16;
constexpr int PIN_TOUCH_RST = 1;
constexpr uint8_t BACKLIGHT_ADDR = 0x30;  // STC8H1K28: 0 = brightest, 245 = off
constexpr size_t FB_BYTES = WIDTH * HEIGHT * sizeof(uint16_t);

esp_lcd_panel_handle_t panel = nullptr;
SemaphoreHandle_t vsyncSem = nullptr;
uint16_t* fbs[2] = {nullptr, nullptr};
int back = 1;  // framebuffer 0 is scanned out first
uint8_t touchAddr = 0;

bool probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void sendBacklight(uint8_t value) {
  Wire.beginTransmission(BACKLIGHT_ADDR);
  Wire.write(value);
  if (Wire.endTransmission() != 0) Serial.printf("[board] backlight write %u failed\n", value);
}

bool gtRead(uint16_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(touchAddr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(touchAddr, len) != len) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
  return true;
}

void gtClearStatus() {
  Wire.beginTransmission(touchAddr);
  Wire.write(0x81);
  Wire.write(0x4E);
  Wire.write(0);
  Wire.endTransmission();
}

constexpr int BOUNCE_LINES = 20;
constexpr uint32_t BOUNCE_PX = WIDTH * BOUNCE_LINES;
constexpr uint32_t EOF_PER_FRAME = HEIGHT / BOUNCE_LINES;  // one DMA EOF per bounce buffer

// The IDF RGB driver (v5.5, bounce-buffer mode) picks the bounce buffer to refill from
// bb_eof_count % 2. With CONFIG_LCD_RGB_RESTART_IN_VSYNC it restarts the DMA every frame
// but never resets that counter, so one EOF interrupt that arrives late (two EOFs merge
// into one callback) leaves the counter's parity wrong for good: every refill then goes
// to the wrong buffer and the whole picture sits one buffer (20 lines) off, header
// wrapped to the bottom, until another slip flips it back. The driver's other branch
// (restart only when needed) zeroes the counter each VSYNC; onVsync() does the same.
volatile uint32_t* eofCounter = nullptr;
volatile uint32_t vsyncCount = 0, badFrames = 0, lastBadCount = 0;

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
  if (eofCounter) {  // runs before the driver's own restart in the same interrupt
    uint32_t n = *eofCounter;
    if (n != EOF_PER_FRAME) {
      ++badFrames;
      lastBadCount = n;
    }
    *eofCounter = 0;
  }
  ++vsyncCount;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsyncSem, &woken);
  return woken == pdTRUE;
}

// Finds bb_eof_count in the driver's private struct without depending on its exact
// layout: it sits between `int bounce_pos_px` (a multiple of one bounce buffer in pixels)
// and `size_t expect_eof_count` (== EOF_PER_FRAME), and it ticks once per bounce buffer.
volatile uint32_t* findEofCounter(esp_lcd_panel_handle_t panel) {
  constexpr int WORDS = 120;
  static uint32_t a[WORDS], b[WORDS];
  const volatile uint32_t* w = reinterpret_cast<const volatile uint32_t*>(panel);
  for (int i = 0; i < WORDS; ++i) a[i] = w[i];
  delay(5);
  for (int i = 0; i < WORDS; ++i) b[i] = w[i];

  int hits = 0, at = -1;
  for (int i = 1; i < WORDS - 1; ++i) {
    bool plausible = a[i + 1] == EOF_PER_FRAME && b[i + 1] == EOF_PER_FRAME && a[i] != b[i] &&
                     a[i - 1] % BOUNCE_PX == 0 && b[i - 1] % BOUNCE_PX == 0 && a[i - 1] <= WIDTH * HEIGHT &&
                     b[i - 1] <= WIDTH * HEIGHT;
    if (plausible) {
      ++hits;
      at = i;
    }
  }
  if (hits != 1) {
    Serial.printf("[board] EOF counter not found (%d candidates): bounce-buffer resync disabled\n", hits);
    return nullptr;
  }
  Serial.printf("[board] EOF counter at word %d: resynced every VSYNC\n", at);
  return const_cast<volatile uint32_t*>(w) + at;
}

// Pin map and porches as in Elecrow's V1.1+ example. Two PSRAM framebuffers plus
// bounce buffers in internal RAM. The pixel clock is lower than Elecrow's 18 MHz:
// their build runs PSRAM at 120 MHz, the Arduino libs here at 80 MHz, and during a
// TLS handshake the display refill interrupt then misses its deadline (measured with
// scanoutStats(): 20 late frames in 30 s at 18 MHz, 2 at 15 MHz, none at 14 MHz and below).
void initPanel() {
  vsyncSem = xSemaphoreCreateBinary();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 14000000;
  cfg.timings.h_res = WIDTH;
  cfg.timings.v_res = HEIGHT;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 8;
  cfg.timings.hsync_front_porch = 8;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 8;
  cfg.timings.vsync_front_porch = 8;
  cfg.timings.flags.hsync_idle_low = 0;
  cfg.timings.flags.vsync_idle_low = 0;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high = 1;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;
  cfg.bounce_buffer_size_px = BOUNCE_PX;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = GPIO_NUM_40;
  cfg.vsync_gpio_num = GPIO_NUM_41;
  cfg.de_gpio_num = GPIO_NUM_42;
  cfg.pclk_gpio_num = GPIO_NUM_39;
  cfg.disp_gpio_num = -1;
  // B0..B4, G0..G5, R0..R4
  const int pins[16] = {21, 47, 48, 45, 38, 9, 10, 11, 12, 13, 14, 7, 17, 18, 3, 46};
  for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = pins[i];
  cfg.flags.fb_in_psram = 1;
  cfg.flags.bb_invalidate_cache = 0;  // 1 would drop dirty lines of a frame that was not written back

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  eofCounter = findEofCounter(panel);

  void* fb0 = nullptr;
  void* fb1 = nullptr;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &fb0, &fb1));
  fbs[0] = static_cast<uint16_t*>(fb0);
  fbs[1] = static_cast<uint16_t*>(fb1);
  memset(fbs[0], 0, FB_BYTES);
  memset(fbs[1], 0, FB_BYTES);
}

}  // namespace

bool begin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setTimeOut(50);
  delay(50);

  // The backlight MCU and GT911 share the bus and sometimes need a nudge after
  // power-up: command 250 plus a low pulse on GPIO1 (Elecrow's recovery sequence).
  bool ok = false;
  for (int attempt = 0; attempt < 30 && !ok; ++attempt) {
    touchAddr = probe(0x5D) ? 0x5D : (probe(0x14) ? 0x14 : 0);
    ok = probe(BACKLIGHT_ADDR) && touchAddr;
    if (ok) break;
    sendBacklight(250);
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(120);
    pinMode(PIN_TOUCH_RST, INPUT);
    delay(100);
  }
  Serial.printf("[board] backlight MCU %s, touch 0x%02X %s\n", probe(BACKLIGHT_ADDR) ? "ok" : "MISSING",
                touchAddr, touchAddr ? "ok" : "MISSING");

  sendBacklight(245);  // dark while the panel starts up
  initPanel();
  setBacklight(100);
  return ok;
}

uint16_t* frameBuffer(int index) { return fbs[index & 1]; }
int backIndex() { return back; }

void present() {
  uint16_t* fb = fbs[back];
  // No cache write-back here: the bounce-buffer refill reads the framebuffer through
  // the CPU cache, so it already sees the new frame. Writing back 768 KB stalled that
  // refill for milliseconds, and a refill that late shifts the picture by one bounce
  // buffer (20 lines) until the driver's buffer parity happens to flip back.
  xSemaphoreTake(vsyncSem, 0);  // drop a stale notification
  if (esp_lcd_panel_draw_bitmap(panel, 0, 0, WIDTH, HEIGHT, fb) == ESP_OK) {
    xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(100));  // the flip happens at the next vsync
    back ^= 1;
  }
}

void setBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  sendBacklight(245 - (percent * 245) / 100);
}

bool setPixelClock(uint32_t hz) { return esp_lcd_rgb_panel_set_pclk(panel, hz) == ESP_OK; }

ScanoutStats scanoutStats() { return {vsyncCount, badFrames, lastBadCount, eofCounter != nullptr}; }

bool readTouch(int& x, int& y) {
  static bool down = false;
  if (!touchAddr) return false;

  uint8_t status = 0;
  if (!gtRead(0x814E, &status, 1) || !(status & 0x80)) return down;  // no new report yet

  uint8_t points = status & 0x0F;
  if (points > 0) {
    uint8_t p[4];
    if (gtRead(0x8150, p, 4)) {
      x = p[0] | (p[1] << 8);
      y = p[2] | (p[3] << 8);
    }
  }
  gtClearStatus();
  down = points > 0;
  return down;
}

}  // namespace board
