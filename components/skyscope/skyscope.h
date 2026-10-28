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
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "beam.h"
#include "feed.h"
#include "overlay.h"
#include "providers.h"
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
  void set_lvgl(lvgl::LvglComponent *lvgl) { lvgl_ = lvgl; }
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
    bool asleep = false;  // the screen is off: no planes or tiles are asked for
    // worker -> loop
    std::vector<feed::Flight> flights;
    uint32_t flights_rev = 0;
    uint32_t feed_at = 0;  // millis() of the last good answer
    providers::Providers providers;  // each exchange's record: waiting, failures, pace (providers.h)
    Feed feed_source = ADSB_FI;      // the exchange the last good report came from
    uint32_t frame_rev = 0, frame_plan_rev = 0;  // the base frame changed, for that plan
    int tiles_done = 0, tiles_total = 0;
    std::string tile_error;
    // How long the last report took: its download, reading its JSON (both on the worker), and its bytes.
    uint32_t fetch_ms = 0, parse_ms = 0, reply_bytes = 0;
    uint32_t feed_every_ms = 0;
    size_t cached_tiles = 0;  // the worker's tile cache, entries  // how often the feed is asked: refresh_s, or less often when its limits say so
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
  void tap_at_(lv_point_t p);
  // The screen off after settings_.sleep_min without a touch, and on again at one (skyscope.cpp).
  void sleep_or_wake_(uint32_t now);
  // The brightness strip down the right edge.
  void on_dimmer_(lv_event_t *e);
  void show_level_(int percent);
  // The settings menu (menu.cpp): a long press on the map opens it.
  void open_menu_();
  void build_menu_();  // the menu's objects, in the colours of the settings it is editing
  void close_menu_();
  void refresh_menu_();
  void menu_step_(int index, int step);
  void menu_action_(int action);
  // A key of the menu: a row's step back or on (row >= 0), or one of its actions (row < 0, MENU_* in menu.cpp).
  lv_obj_t *key_(lv_obj_t *parent, const char *label, int row, int step, int width, int height);
  void set_brightness_now_(int percent);
  // The colours of what lies over the map: the radar's phosphor on a scope, else the screen's own.
  struct Chrome { uint32_t ink, muted, card; };
  Chrome chrome_() const;
  static Chrome chrome_for_(const Settings &s);
  void restyle_();
  void set_status_colour_(uint32_t colour);
  // settings
  void load_settings_();
  void save_settings_();
  void use_settings_(const Settings &s);  // on the loop
  void replan_();

  web_server_base::WebServerBase *base_{nullptr};
  lvgl::LvglComponent *lvgl_{nullptr};
  int native_width_{0}, native_height_{0};  // the panel's own way up, rotation 0
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
  // seen_flights_rev_ AGAIN: the planes are drawn again on the next loop, even before any report (a radar sweeps with
  // none).
  static constexpr uint32_t AGAIN = UINT32_MAX;
  uint32_t seen_flights_rev_{AGAIN}, tracked_rev_{0}, seen_frame_rev_{0}, framed_plan_rev_{0}, copied_at_{0};
  std::vector<feed::Flight> flights_;
  overlay::Tracks tracks_;
  std::string picked_;
  size_t shown_count_{0};
  // How long the loop took over the last report (placing its planes, the plane layer's update), and the longest yet.
  float report_ms_{0}, report_ms_max_{0};
  // ESPHome's loop as this component sees it, over the last five seconds: passes a second and the longest gap.
  uint32_t loop_last_us_{0}, loop_passes_{0}, loop_window_start_{0}, steps_from_{0};
  float loop_gap_ms_{0}, loop_gap_max_ms_{0}, loops_per_s_{0}, steps_per_s_{0}, paint_ms_{0};
  int64_t paint_from_{0};
  uint32_t status_colour_{0xFFFFFFFF};
  uint32_t address_until_{0}, last_tick_{0}, last_status_{0};
  bool asleep_{false};
  uint32_t taps_from_{0};  // taps before this (the one that woke the screen) do nothing
  // A drag on the brightness strip: where it started, the brightness then, whether it moved, and the level shown.
  int32_t drag_y_{0};
  int drag_from_{0}, drag_level_{-1};  // drag_level_ -1: no drag
  bool dragged_{false};
  uint32_t level_until_{0};
  bool ui_built_{false};
  float applied_brightness_{-1};

  lv_obj_t *canvas_{nullptr}, *planes_{nullptr}, *status_{nullptr}, *credit_{nullptr};
  lv_obj_t *dimmer_{nullptr}, *level_{nullptr}, *level_fill_{nullptr}, *level_text_{nullptr};
  lv_obj_t *menu_{nullptr}, *menu_list_{nullptr};
  Settings menu_settings_;  // the menu's changes, applied all at once by Done
  std::vector<lv_obj_t *> menu_rows_, menu_values_;
  bool long_pressed_{false};  // the click that ends a long press (which opened the menu) is no tap
  lv_obj_t *card_{nullptr}, *card_title_{nullptr}, *card_rows_[3]{};
  lv_obj_t *setup_{nullptr}, *setup_title_{nullptr}, *setup_body_{nullptr};
  uint16_t *canvas_buffer_{nullptr};
  // The radar's scope as drawn, without its beam: what the beam painter (beam.h) puts back where the trail has passed.
  uint16_t *scope_background_{nullptr};
  beam::Painter painter_;
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
