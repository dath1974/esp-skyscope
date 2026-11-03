#ifdef USE_ESP32_VARIANT_ESP32S3
#include "rgb_panel.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>

namespace esphome::rgb_panel {
static const char *const TAG = "rgb_panel";
static constexpr uint8_t DELAY_FLAG = 0xFF;  // display.py: a delay's milliseconds, then this in place of a length

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
  config.num_fbs = 1;
  // The frame in PSRAM. With no bounce rows the panel's DMA reads it there itself, starting again from its top at
  // every frame (CONFIG_LCD_RGB_RESTART_IN_VSYNC), so a moment when the PSRAM is too busy spoils one frame at most.
  // With them, it goes through two buffers in internal RAM that an interrupt refills in turn; a refill that comes late
  // leaves the picture shifted by a buffer's rows, frame after frame, until it slips further.
  config.flags.fb_in_psram = 1;
  config.dma_burst_size = 64;
  config.bounce_buffer_size_px = (size_t) width_ * bounce_rows_;
  // Each refill drops the rows it read from the cache again, so the frame passing through does not push out what
  // drawing and the worker are using. Only this core writes the frame (draw_pixels_at), and it writes the cache back
  // as it goes, so nothing written is lost.
  config.flags.bb_invalidate_cache = bounce_rows_ > 0;
  config.hsync_gpio_num = (gpio_num_t) hsync_pin_->get_pin();
  config.vsync_gpio_num = (gpio_num_t) vsync_pin_->get_pin();
  config.de_gpio_num = (gpio_num_t) de_pin_->get_pin();
  config.pclk_gpio_num = (gpio_num_t) pclk_pin_->get_pin();
  config.disp_gpio_num = GPIO_NUM_NC;
  for (int i = 0; i < 16; ++i) config.data_gpio_nums[i] = (gpio_num_t) data_pins_[i]->get_pin();
  esp_err_t err = esp_lcd_new_rgb_panel(&config, &handle_);
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
  esp_lcd_panel_draw_bitmap(handle_, x, y, x + 1, y + 1, &pixel);
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
