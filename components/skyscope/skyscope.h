#pragma once
// Skyscope: live aircraft over a map, fetched and drawn by the screen alone. Two threads share the work:
//
// - The worker task (core 0) does everything slow: it asks the ADS-B exchange for the planes every few seconds and
//   fetches, caches and decodes the map's tiles into the base frame.
// - ESPHome's loop (with LVGL) does everything on the glass: it copies a finished base frame into the map's canvas with
//   the rings drawn over it, places the planes (overlay.h) and paints them (view.cpp), and answers fingers.
//
// They meet in `Shared` under `lock_`, and the base frame under `frame_lock_`. The web page and its API (web.cpp) run
// in the HTTP server's task and only ever read a copy of the settings or hand a new one to the loop.
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/font/font.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "feed.h"
#include "overlay.h"
#include "settings.h"
#include "tiles.h"
#include "view.h"

namespace esphome::skyscope {
using namespace ::sky;

// The screen's own IPv4 address on its network, "" while it has none.
std::string own_address();

class Skyscope : public Component, public AsyncWebHandler {
 public:
  void set_web_server_base(web_server_base::WebServerBase *base) { base_ = base; }
  void set_fonts(font::Font *small, font::Font *label, font::Font *title) {
    font_small_ = small;
    font_label_ = label;
    font_title_ = title;
  }
  void set_dpi(float dpi) { dpi_ = dpi; }
  void set_tile_scale(int scale) { tile_scale_ = scale; }
  void set_user_agent(const std::string &agent) { agent_ = agent; }
  void set_setup_network(const std::string &name) { setup_network_ = name; }
  void set_brightness_output(output::FloatOutput *out) { brightness_ = out; }
  void set_version(const std::string &v) { version_ = v; }
  void set_panel_te_pin(int pin) { panel_te_pin_ = pin; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // The web page and its API (web.cpp).
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  struct Shared {
    // loop -> worker
    Settings settings;
    uint32_t settings_rev = 0;
    tiles::Plan plan;
    uint32_t plan_rev = 0;
    bool online = false;
    // worker -> loop
    std::vector<feed::Flight> flights;
    uint32_t flights_rev = 0;
    uint32_t feed_at = 0;  // millis() of the last good answer
    std::string feed_error;
    uint32_t frame_rev = 0, frame_plan_rev = 0;  // the base frame changed, for that plan
    int tiles_done = 0, tiles_total = 0;
    std::string tile_error;
  };

  // worker.cpp
  static void worker_main(void *self);
  void worker_();
  // skyscope.cpp
  void build_ui_();
  void update_frame_();
  void update_planes_(uint32_t now);
  void update_status_(uint32_t now);
  void update_card_();
  void on_tap_(lv_event_t *e);
  // settings
  void load_settings_();
  void save_settings_();
  void use_settings_(const Settings &s);  // on the loop
  void replan_();

  web_server_base::WebServerBase *base_{nullptr};
  font::Font *font_small_{nullptr}, *font_label_{nullptr}, *font_title_{nullptr};
  output::FloatOutput *brightness_{nullptr};
  float dpi_{200};
  int tile_scale_{1};
  int panel_te_pin_{-1};
  std::string agent_{"esp-skyscope"}, setup_network_, version_;

  // The loop's own.
  Settings settings_;
  ESPPreferenceObject pref_;
  uint32_t save_at_{0};
  bool save_pending_{false};
  int width_{0}, height_{0};
  tiles::Plan plan_;
  uint32_t plan_rev_{0};
  uint32_t seen_flights_rev_{0}, tracked_rev_{0}, seen_frame_rev_{0}, framed_plan_rev_{0}, copied_at_{0};
  std::vector<feed::Flight> flights_;
  overlay::Tracks tracks_;
  std::string picked_;
  size_t shown_count_{0};
  uint32_t address_until_{0}, last_tick_{0}, last_status_{0};
  bool ui_built_{false};
  float applied_brightness_{-1};

  lv_obj_t *canvas_{nullptr}, *planes_{nullptr}, *status_{nullptr}, *credit_{nullptr};
  lv_obj_t *card_{nullptr}, *card_title_{nullptr}, *card_rows_[3]{};
  lv_obj_t *setup_{nullptr}, *setup_title_{nullptr}, *setup_body_{nullptr};
  uint16_t *canvas_buffer_{nullptr};
  view::View *view_{nullptr};

  // Shared with the worker.
  std::mutex lock_;
  Shared shared_;
  std::mutex frame_lock_;
  uint16_t *base_frame_{nullptr};
  TaskHandle_t worker_task_{nullptr};

  // The body of a POST on its way in (the HTTP server's task).
  std::string body_;
};

}  // namespace esphome::skyscope
