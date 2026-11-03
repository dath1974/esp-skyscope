#pragma once
// An RGB panel the ESP32-S3 feeds all the time from a frame in PSRAM (display.py says how it is set up). LVGL's frames
// are copied into that frame as they are drawn; the panel shows whatever it holds.
#ifdef USE_ESP32_VARIANT_ESP32S3
#include "esphome/components/display/display.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include <cstdint>
#include <vector>

namespace esphome::rgb_panel {

class RgbPanel : public display::Display,
                 public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                       spi::DATA_RATE_1MHZ> {
 public:
  void setup() override;
  void dump_config() override;
  void update() override { this->do_update_(); }
  float get_setup_priority() const override { return setup_priority::HARDWARE; }
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_COLOR; }

  void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr, display::ColorOrder order,
                      display::ColorBitness bitness, bool big_endian, int x_offset, int y_offset, int x_pad) override;
  void draw_pixel_at(int x, int y, Color color) override;

  void set_dimensions(int width, int height) {
    width_ = width;
    height_ = height;
  }
  void set_init_sequence(const std::vector<uint8_t> &sequence) { sequence_ = sequence; }
  void set_reset_pin(GPIOPin *pin) { reset_pin_ = pin; }
  void set_data_pin(int index, InternalGPIOPin *pin) { data_pins_[index] = pin; }
  void set_de_pin(InternalGPIOPin *pin) { de_pin_ = pin; }
  void set_pclk_pin(InternalGPIOPin *pin) { pclk_pin_ = pin; }
  void set_hsync_pin(InternalGPIOPin *pin) { hsync_pin_ = pin; }
  void set_vsync_pin(InternalGPIOPin *pin) { vsync_pin_ = pin; }
  void set_pclk(uint32_t hz, bool inverted) {
    pclk_hz_ = hz;
    pclk_inverted_ = inverted;
  }
  void set_hsync(int pulse, int back, int front) {
    hsync_pulse_ = pulse;
    hsync_back_ = back;
    hsync_front_ = front;
  }
  void set_vsync(int pulse, int back, int front) {
    vsync_pulse_ = pulse;
    vsync_back_ = back;
    vsync_front_ = front;
  }
  void set_bounce_rows(int rows) { bounce_rows_ = rows; }

 protected:
  int get_width_internal() override { return width_; }
  int get_height_internal() override { return height_; }

  void send_sequence_();
  void write_word_(uint16_t word);  // 9 bits: the top one says data (1) or command (0)

  int width_{0}, height_{0};
  std::vector<uint8_t> sequence_;
  GPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *data_pins_[16]{};
  InternalGPIOPin *de_pin_{nullptr}, *pclk_pin_{nullptr}, *hsync_pin_{nullptr}, *vsync_pin_{nullptr};
  uint32_t pclk_hz_{12000000};
  bool pclk_inverted_{false};
  int hsync_pulse_{8}, hsync_back_{10}, hsync_front_{50};
  int vsync_pulse_{2}, vsync_back_{18}, vsync_front_{8};
  int bounce_rows_{0};
  esp_lcd_panel_handle_t handle_{nullptr};
};

}  // namespace esphome::rgb_panel
#endif
