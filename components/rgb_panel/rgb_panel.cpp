#ifdef USE_ESP32_VARIANT_ESP32S3
#include "rgb_panel.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <esp32s3/rom/cache.h>
#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_private/esp_clk.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace esphome::rgb_panel {
static const char *const TAG = "rgb_panel";
static constexpr uint8_t DELAY_FLAG = 0xFF;  // display.py: a delay's milliseconds, then this in place of a length
static constexpr uint32_t WINDOW_MS = 5000;  // late refills are told of over this long
static constexpr uint32_t NEAR_US = 100;  // a refill that ends closer than this to its deadline is a near miss

// The refill, run in the panel's interrupt (in IRAM, its data in internal RAM, so it runs while flash is written).
// It does what ESP-IDF's own refill does from a frame of its own: copies the rows due next from the frame, then asks
// the cache to start fetching the rows after them. Timing it costs two reads of the CPU's cycle counter.
namespace {
struct Refill {
  const uint8_t *frame = nullptr;
  size_t frame_bytes = 0;
  int after_pause_px = 0;    // the refill that follows the pause between frames (the third of a frame)
  uint32_t budget = 0;       // cycles: two buffers' time on the wire
  uint32_t pause = 0;        // cycles: the pause between frames, which that refill has besides
  uint32_t last_start = 0;   // cycle count as the refill before began
  int block_px = 1;          // pixels in a block of bounce rows
  uint32_t block = 0;        // cycles: a block's time on the wire
  // The panel's own pace, for this frame: the panel's DMA takes a block every block's time, so the earliest any
  // refill of the frame began (less its place in the frame) is when the frame's blocks are due. A refill is late by
  // how far it ends after the panel starts on the buffer it fills: the end of the block before it.
  uint32_t frame_base = 0;
  bool base_set = false;
  int32_t lateness_max = INT32_MIN;  // cycles, the most late (or least early) this window
  uint32_t near = 0;         // refills that ended less than NEAR_US before their deadline
  uint32_t count = 0;        // refills so far
  uint32_t late = 0, fill_max = 0, span_max = 0;
  // The loop's, for the last whole window: refills late, and the worst times (cycles).
  uint32_t late_recent = 0, fill_recent = 0, span_recent = 0, near_recent = 0, near_at_window = 0;
  int32_t lateness_recent = 0;
  uint32_t near_cycles = 0;
};
DRAM_ATTR Refill refill_;

IRAM_ATTR bool refill_bounce(esp_lcd_panel_handle_t, void *buffer, int pos_px, int len_bytes, void *) {
  Refill &r = refill_;
  const uint32_t start = esp_cpu_get_cycle_count();
  const size_t at = (size_t) pos_px * 2;
  std::memcpy(buffer, r.frame + at, len_bytes);
  // The next rows, fetched into the cache while the panel sends these.
  const size_t next = at + len_bytes < r.frame_bytes ? at + len_bytes : 0;
  Cache_Start_DCache_Preload((uint32_t) (r.frame + next), len_bytes, 0);
  const uint32_t end = esp_cpu_get_cycle_count();
  ++r.count;
  if (end - start > r.fill_max) r.fill_max = end - start;
  // This buffer's refill had to be done before the panel came back to it: two buffers' time after the refill before
  // it began (that refill came on time or this one would be later still, so this can only undercount). The two first
  // refills of a frame are made at the end of the frame before, and the third comes after the pause between frames.
  const uint32_t span = end - r.last_start, allowed = pos_px == r.after_pause_px ? r.budget + r.pause : r.budget;
  if (r.count > 1 && span - (allowed - r.budget) > r.span_max) r.span_max = span - (allowed - r.budget);
  // Blocks 0 and 1 are refilled at the end of the frame before; from block 2 (after the pause) on, the frame's pace.
  const int j = pos_px / r.block_px;
  if (j >= 2) {
    const uint32_t rel = start - (uint32_t) j * r.block;
    if (j == 2 || !r.base_set || (int32_t) (rel - r.frame_base) < 0) r.frame_base = rel;
    r.base_set = true;
    const int32_t lateness = (int32_t) (end - (r.frame_base + (uint32_t) (j + 1) * r.block));
    if (lateness > r.lateness_max) r.lateness_max = lateness;
    if (lateness > 0) ++r.late;
    else if (lateness > -(int32_t) r.near_cycles) ++r.near;
  }
  r.last_start = start;
  return false;
}
}  // namespace

Refills refills() {
  Refills out;
  const Refill &r = refill_;
  if (!r.frame) return out;
  const uint32_t per_us = esp_clk_cpu_freq() / 1000000;
  out.count = r.count;
  out.late = r.late;
  out.late_recent = r.late_recent;
  out.fill_us_max = r.fill_recent / per_us;
  out.span_us_max = r.span_recent / per_us;
  out.budget_us = r.budget / per_us;
  out.near_recent = r.near_recent;
  out.lateness_us_max = r.lateness_recent / (int32_t) per_us;
  return out;
}

void RgbPanel::setup() {
  // A reset first, so the panel starts from its own defaults after a restart as after power on.
  if (reset_pin_) {
    reset_pin_->setup();
    reset_pin_->digital_write(true);
    delay(5);
    reset_pin_->digital_write(false);
    delay(10);
    reset_pin_->digital_write(true);
    delay(120);
  }
  this->spi_setup();
  send_sequence_();
  // Set up for good: the lines are let go (a card may share them).
  this->spi_teardown();

  esp_lcd_rgb_panel_config_t config{};
  config.clk_src = LCD_CLK_SRC_PLL160M;
  config.timings.pclk_hz = pclk_hz_;
  config.timings.h_res = width_;
  config.timings.v_res = height_;
  config.timings.hsync_pulse_width = hsync_pulse_;
  config.timings.hsync_back_porch = hsync_back_;
  config.timings.hsync_front_porch = hsync_front_;
  config.timings.vsync_pulse_width = vsync_pulse_;
  config.timings.vsync_back_porch = vsync_back_;
  config.timings.vsync_front_porch = vsync_front_;
  config.timings.flags.pclk_active_neg = pclk_inverted_;
  config.data_width = 16;
  // The frame in PSRAM. With no bounce rows the panel's DMA reads it there itself (ESP-IDF's frame), starting again
  // from its top at every frame (CONFIG_LCD_RGB_RESTART_IN_VSYNC), so a moment when the PSRAM is too busy spoils one
  // frame at most. With them, it goes through two buffers in internal RAM that an interrupt refills in turn from a
  // frame this driver keeps (refill_bounce, which times each refill); a refill that comes late sends rows that are
  // not the frame's.
  config.flags.fb_in_psram = 1;
  config.dma_burst_size = 64;
  config.bounce_buffer_size_px = (size_t) width_ * bounce_rows_;
  if (bounce_rows_ > 0) {
    const size_t bytes = (size_t) width_ * height_ * 2;
    frame_ = (uint16_t *) heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_) {
      ESP_LOGE(TAG, "No PSRAM for the frame");
      this->mark_failed();
      return;
    }
    config.flags.no_fb = 1;
    // Two buffers' time on the wire: each row is its pixels and the line's blanking, a clock each.
    const uint64_t row_clocks = (uint64_t) width_ + hsync_pulse_ + hsync_back_ + hsync_front_;
    refill_.budget = (uint32_t) (2 * bounce_rows_ * row_clocks * esp_clk_cpu_freq() / pclk_hz_);
    refill_.pause = (uint32_t) ((uint64_t) (vsync_pulse_ + vsync_back_ + vsync_front_) * row_clocks *
                                esp_clk_cpu_freq() / pclk_hz_);
    refill_.after_pause_px = 2 * width_ * bounce_rows_;
    refill_.block_px = width_ * bounce_rows_;
    refill_.block = refill_.budget / 2;
    refill_.near_cycles = NEAR_US * (esp_clk_cpu_freq() / 1000000);
    refill_.frame_bytes = bytes;
    refill_.frame = (const uint8_t *) frame_;
  } else {
    config.num_fbs = 1;
  }
  config.hsync_gpio_num = (gpio_num_t) hsync_pin_->get_pin();
  config.vsync_gpio_num = (gpio_num_t) vsync_pin_->get_pin();
  config.de_gpio_num = (gpio_num_t) de_pin_->get_pin();
  config.pclk_gpio_num = (gpio_num_t) pclk_pin_->get_pin();
  config.disp_gpio_num = GPIO_NUM_NC;
  for (int i = 0; i < 16; ++i) config.data_gpio_nums[i] = (gpio_num_t) data_pins_[i]->get_pin();
  esp_err_t err = esp_lcd_new_rgb_panel(&config, &handle_);
  if (err == ESP_OK && frame_) {
    esp_lcd_rgb_panel_event_callbacks_t callbacks{};
    callbacks.on_bounce_empty = refill_bounce;
    err = esp_lcd_rgb_panel_register_event_callbacks(handle_, &callbacks, nullptr);
  }
  if (err == ESP_OK) err = esp_lcd_panel_reset(handle_);
  if (err == ESP_OK) err = esp_lcd_panel_init(handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The panel would not start: %s", esp_err_to_name(err));
    this->mark_failed();
  }
}

void RgbPanel::send_sequence_() {
  for (size_t i = 0; i + 1 < sequence_.size();) {
    const uint8_t first = sequence_[i++], length = sequence_[i++];
    if (length == DELAY_FLAG) {
      delay(first);
      continue;
    }
    write_word_(first);
    for (uint8_t n = 0; n < length && i < sequence_.size(); ++n) write_word_(0x100 | sequence_[i++]);
  }
}

void RgbPanel::write_word_(uint16_t word) {
  this->enable();
  this->write(word, 9);
  this->disable();
}

void RgbPanel::draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                              display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) {
  if (w <= 0 || h <= 0 || handle_ == nullptr) return;
  if (bitness != display::COLOR_BITNESS_565) {
    // Anything but LVGL's own pixels, a pixel at a time.
    display::Display::draw_pixels_at(x_start, y_start, w, h, ptr, order, bitness, big_endian, x_offset, y_offset,
                                     x_pad);
    return;
  }
  // The pixels are copied into the frame as they come (in the chip's order: lvgl's byte_order).
  const size_t stride = (size_t) (x_offset + w + x_pad) * 2;
  ptr += y_offset * stride + x_offset * 2;
  if (frame_) {
    const int x0 = std::max(x_start, 0), y0 = std::max(y_start, 0);
    const int x1 = std::min(x_start + w, width_), y1 = std::min(y_start + h, height_);
    if (x0 >= x1 || y0 >= y1) return;
    const size_t row_bytes = (size_t) (x1 - x0) * 2;
    const uint8_t *src = ptr + (size_t) (y0 - y_start) * stride + (size_t) (x0 - x_start) * 2;
    for (int y = y0; y < y1; ++y, src += stride) std::memcpy(frame_ + (size_t) y * width_ + x0, src, row_bytes);
    return;
  }
  esp_err_t err = ESP_OK;
  if (x_offset == 0 && x_pad == 0) {
    err = esp_lcd_panel_draw_bitmap(handle_, x_start, y_start, x_start + w, y_start + h, ptr);
  } else {
    for (int y = 0; y < h && err == ESP_OK; ++y, ptr += stride)
      err = esp_lcd_panel_draw_bitmap(handle_, x_start, y_start + y, x_start + w, y_start + y + 1, ptr);
  }
  if (err != ESP_OK) ESP_LOGE(TAG, "A copy into the frame failed: %s", esp_err_to_name(err));
}

void RgbPanel::draw_pixel_at(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_ || handle_ == nullptr) return;
  const uint16_t pixel = display::ColorUtil::color_to_565(color);
  if (frame_) {
    frame_[(size_t) y * width_ + x] = pixel;
    return;
  }
  esp_lcd_panel_draw_bitmap(handle_, x, y, x + 1, y + 1, &pixel);
}

// Every five seconds: a word in the log when refills came late.
void RgbPanel::loop() {
  if (!frame_) return;
  const uint32_t now = millis();
  if (now - window_start_ < WINDOW_MS) return;
  window_start_ = now;
  // The worst times start again with each window (an interrupt between these lines only loses one refill's).
  refill_.fill_recent = refill_.fill_max;
  refill_.span_recent = refill_.span_max;
  refill_.fill_max = refill_.span_max = 0;
  refill_.lateness_recent = refill_.lateness_max == INT32_MIN ? 0 : refill_.lateness_max;
  refill_.lateness_max = INT32_MIN;
  refill_.near_recent = refill_.near - refill_.near_at_window;
  refill_.near_at_window = refill_.near;
  const uint32_t late = refill_.late;
  refill_.late_recent = late - late_at_window_;
  late_at_window_ = late;
  if (refill_.late_recent) {
    const Refills r = refills();
    ESP_LOGW(TAG, "%u late refills in %u s (the longest %u us of %u, a copy %u us at most)", (unsigned) r.late_recent,
             (unsigned) (WINDOW_MS / 1000), (unsigned) r.span_us_max, (unsigned) r.budget_us,
             (unsigned) r.fill_us_max);
  }
}

void RgbPanel::dump_config() {
  ESP_LOGCONFIG(TAG,
                "RGB panel:\n"
                "  Size: %d x %d\n"
                "  Pixel clock: %u Hz\n"
                "  Bounce rows: %d (0: read from PSRAM by DMA)",
                width_, height_, (unsigned) pclk_hz_, bounce_rows_);
  LOG_PIN("  Reset pin: ", reset_pin_);
}

}  // namespace esphome::rgb_panel
#endif
