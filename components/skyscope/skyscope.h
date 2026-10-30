#pragma once
// Skyscope: live aircraft over a map, fetched and drawn by the screen alone. Two threads share the work:
//
// - The worker task (core 0) does everything slow: it asks the ADS-B exchange for the planes every few seconds and
//   fetches, caches and decodes the map's tiles into the base frame.
// - ESPHome's loop (with LVGL) does everything on the glass: it copies a finished base frame into the map's canvas with
//   the rings drawn over it, places the planes (overlay.h) and paints them (view.cpp), and answers fingers.
//
// They meet in `Shared` under `lock_`, and the base frame under `frame_lock_`. The web page and its API (web.cpp) run
// in the HTTP server's task and only ever read a copy of the settings or hand a new one to the loop. The worker also
// finds the places typed in the menu (places.h).
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/font/font.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_UPDATE
#include "esphome/components/http_request/update/http_request_update.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "airspace.h"
#include "beam.h"
#include "feed.h"
#include "firmware.h"
#include "keyboard.h"
#include "overlay.h"
#include "places.h"
#include "providers.h"
#include "touch.h"
#include "settings.h"
#include "tiles.h"
#include "view.h"

namespace esphome::skyscope {
using namespace ::sky;

// The screen's own IPv4 address on its network, "" while it has none.
std::string own_address();

class Skyscope : public Component, public AsyncWebHandler, public wifi::WiFiScanResultsListener {
 public:
  void set_web_server_base(web_server_base::WebServerBase *base) { base_ = base; }
  void set_lvgl(lvgl::LvglComponent *lvgl) { lvgl_ = lvgl; }
#ifdef USE_TIME
  void set_clock(time::RealTimeClock *clock) { clock_ = clock; }
#endif
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
#ifdef USE_UPDATE
  void set_firmware(http_request::HttpRequestUpdate *firmware) { firmware_ = firmware; }
#endif

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // The web page and its API (web.cpp).
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }
  // The Wi-Fi networks in reach, after each scan (editor.cpp), on the loop.
  void on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) override;

 protected:
  struct Shared {
    // loop -> worker
    Settings settings;
    uint32_t settings_rev = 0;
    tiles::Plan plan;
    uint32_t plan_rev = 0;
    bool online = false;
    bool asleep = false;  // the screen is off: no planes or tiles are asked for
    bool updating = false;  // new firmware is on its way in: nothing else is asked for
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
    // The airspace and runways of the last place and range asked (airspace.h), which may be an earlier frame's while a
    // new one comes, and why the last request failed.
    std::shared_ptr<const airspace::Map> airspace;
    uint32_t airspace_rev = 0, airspace_bytes = 0;
    std::string airspace_error;
    // loop -> web: the firmware on the update server (web.cpp), as the update entity last told it.
    struct Firmware {
      bool known = false;  // a manifest was read
      bool available = false, installing = false;
      std::string latest, summary, release_url, error;
      float progress = 0;  // percent, while installing
    } firmware;
    // loop -> worker: a place to find, typed in the menu; worker -> loop: what was found for the search found_rev.
    std::string search;
    uint32_t search_rev = 0, found_rev = 0;
    std::vector<places::Found> found;
    std::string search_error;
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
  void go_to_sleep_(const std::string &why);
  bool bedtime_(uint32_t now);
  void wake_by_clock_();
  void use_timezone_();  // settings_.timezone to the clock
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
  void apply_menu_(const Settings &changed);  // the menu's changes, where there are any
  void refresh_menu_firmware_();  // the menu's firmware part, from what the update entity last said
  void refresh_menu_wifi_();      // the network the screen is on
  // A key of the menu: a row's step back or on (row >= 0), or one of its actions (row < 0, MENU_* in menu.cpp). Its
  // face is `label`, or an arrow drawn (`arrow` one of menu.cpp's ARROW_*) where `label` is nullptr. A primary key is
  // filled with the accent.
  lv_obj_t *key_(lv_obj_t *parent, const char *label, int row, int step, int width, int height, int arrow = -1,
                 bool primary = false);
  lv_obj_t *button_(lv_obj_t *parent, const char *label, int width, int height, int arrow = -1, bool primary = false);
  // The keyboard (editor.cpp), over the menu: a row's value, a place to find, or a Wi-Fi network's name or password.
  enum Typing : uint8_t { TYPING_ROW, TYPING_PLACE, TYPING_NETWORK, TYPING_PASSWORD };
  void open_row_editor_(int row);
  void open_editor_(Typing what, const std::string &title, const std::string &text, keyboard::Pad pad, size_t most,
                    const std::string &hint);
  void build_keys_();  // the keys of the keyboard's page now
  void press_key_(int index);
  void refresh_editor_();
  void editor_done_();
  void close_editor_();
  // A list to pick from, over the keyboard or the menu (editor.cpp): the places a search found, or the networks in reach.
  enum Picking : uint8_t { PICKING_PLACES, PICKING_NETWORKS };
  void open_picker_(Picking what);
  void refresh_picker_();       // its line, from the search or the scan under way
  void fill_picker_();          // its items, built again
  void picker_pick_(int index);
  void close_picker_();
  void scan_networks_();
  void networks_from_(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results);
  void join_network_(const std::string &name, const std::string &password);
  void set_brightness_now_(int percent);
#ifdef SKYSCOPE_TEST_API
  // Tests without a finger (probe.cpp): a touch as a second input device, and what the glass shows.
  void make_probe_input_();
  void probe_touch_start_(const touch::Script &s);  // on the loop
  void describe_ui_(JsonObject out);                  // on the loop
  void handle_probe_(AsyncWebServerRequest *request, const std::string &url, const std::string &body);
  touch::Player probe_touch_;
  lv_indev_t *probe_input_{nullptr};
  uint32_t probe_busy_until_{0};  // the web server's task: the touch under way ends then
#endif
  // The colours of what lies over the map: the radar's phosphor on a scope, else the screen's own.
  struct Chrome { uint32_t ink, muted, card, accent; };
  Chrome chrome_() const;
  static Chrome chrome_for_(const Settings &s);
  void restyle_();
  void set_status_colour_(uint32_t colour);
  // settings
  void load_settings_();
  void save_settings_();
  void use_settings_(const Settings &s);  // on the loop
  void replan_();
  void replan_locked_();  // replan_() with lock_ held
  // Firmware from the update server (web.cpp): its state to the page, and the install.
  void firmware_changed_();
  void install_firmware_();

  web_server_base::WebServerBase *base_{nullptr};
  lvgl::LvglComponent *lvgl_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif
  int bed_minute_{-1};  // the minute of the day bedtime was last looked at; -1 before the first
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
  // The airspace drawn into the frame: the worker's map of it, and whether it was shown at all.
  uint32_t drawn_airspace_rev_{0};
  bool drawn_airspace_{false};
  float airspace_draw_ms_{0};  // how long the last drawing of it took, on the loop
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
  // An install: the screen says so, and the worker rests, until the screen restarts or the install fails.
  firmware::Tracker install_;
#ifdef USE_UPDATE
  http_request::HttpRequestUpdate *firmware_{nullptr};
#endif
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
  Settings menu_settings_;  // the menu's changes, applied all at once by Save
  std::vector<lv_obj_t *> menu_rows_, menu_values_;
  std::vector<lv_obj_t *> menu_groups_;  // each group's heading and card, by row: the group the row is in
  lv_obj_t *menu_fw_line_{nullptr}, *menu_fw_install_{nullptr}, *menu_fw_check_{nullptr};
  lv_obj_t *menu_save_{nullptr}, *menu_cancel_{nullptr}, *menu_up_{nullptr}, *menu_down_{nullptr};
  std::vector<lv_obj_t *> menu_fields_;  // by row: the field a tap types in, nullptr where the row is not typed
  lv_obj_t *menu_wifi_{nullptr}, *menu_wifi_value_{nullptr};
  // The keyboard: what is typed, for which row, and its objects. Its keys by index, as built for the page showing.
  lv_obj_t *editor_{nullptr}, *editor_title_{nullptr}, *editor_field_{nullptr}, *editor_text_{nullptr};
  lv_obj_t *editor_cursor_{nullptr}, *editor_hint_{nullptr}, *editor_keys_{nullptr}, *editor_done_key_{nullptr};
  lv_obj_t *editor_show_{nullptr};
  Typing typing_{TYPING_ROW};
  int typing_row_{-1};
  keyboard::Board board_;
  std::vector<keyboard::Key> board_keys_;
  std::vector<lv_obj_t *> board_key_objs_;
  std::string editor_hint_text_, editor_error_;
  bool editor_masked_{false};  // a password, shown as stars until Show
  // The list to pick from, and what it lists: the places found for the search asked (search_rev_), or the networks.
  lv_obj_t *picker_{nullptr}, *picker_back_{nullptr}, *picker_line_{nullptr}, *picker_list_{nullptr};
  lv_obj_t *picker_scan_{nullptr};
  Picking picking_{PICKING_PLACES};
  uint32_t search_rev_{0};
  bool searching_{false};
  std::vector<places::Found> found_;
  std::string found_error_;
  struct Network {
    std::string name;
    int rssi;
    bool secured;
  };
  std::vector<Network> networks_;
  uint32_t scanning_until_{0};  // a scan asked for, until it answers or this
  std::string network_name_;   // the network whose password is being typed
  std::string joining_;        // the network the screen restarts to join
  uint32_t menu_install_armed_until_{0};  // Install takes a second tap before this
  uint32_t menu_checking_until_{0}, menu_checking_from_{0};  // a check asked for in the menu, and the reports then
  uint32_t firmware_reports_{0};  // the update entity's reports so far
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
