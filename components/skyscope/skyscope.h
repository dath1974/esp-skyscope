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
// finds the places typed in the menu (places.h), and the plane followed (follow.h), whose view the loop moves with it.
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/font/font.h"
#include "esphome/components/lvgl/lvgl_esphome.h"
#include "esphome/components/output/float_output.h"
#ifdef USE_TOUCHSCREEN
#include "esphome/components/touchscreen/touchscreen.h"
#endif
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_OTA_STATE_LISTENER
#include "esphome/components/ota/ota_backend.h"
#endif
#ifdef USE_UPDATE
#include "esphome/components/http_request/update/http_request_update.h"
#include "esphome/components/update/update_entity.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "airspace.h"
#include "beam.h"
#include "card_io.h"
#include "emblem.h"
#include "feed.h"
#include "firmware.h"
#include "follow.h"
#include "keyboard.h"
#include "networks.h"
#include "offline.h"
#include "overlay.h"
#include "pinch.h"
#include "radar.h"
#include "photos.h"
#include "places.h"
#include "providers.h"
#include "touch.h"
#include "settings.h"
#include "sightings.h"
#include "ticks.h"
#include "tiles.h"
#include "tour.h"
#include "view.h"
#include "weather.h"
#ifdef USE_SKYSCOPE_RADIO
#include "radio.h"
#endif

namespace esphome::skyscope {
using namespace ::sky;

// A plane's photo as the card shows it (photos.h): what the API said, and its pixels, decoded in PSRAM and freed with
// it. Held only while the card shows it, as Planespotters.net's terms ask.
struct PhotoImage {
  std::string hex;
  photos::Photo photo;
  uint16_t *pixels = nullptr;
  int width = 0, height = 0;
  PhotoImage() = default;
  PhotoImage(const PhotoImage &) = delete;
  PhotoImage &operator=(const PhotoImage &) = delete;
  ~PhotoImage();
};

// The screen's own IPv4 address on its network, "" while it has none.
std::string own_address();

class Skyscope : public Component,
                 public AsyncWebHandler,
                 public wifi::WiFiScanResultsListener
#ifdef USE_TOUCHSCREEN
    ,
                 public touchscreen::TouchListener
#endif
#ifdef USE_OTA_STATE_LISTENER
    ,
                 public ota::OTAGlobalStateListener
#endif
{
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
  // A large font for the screen's name on the splash (splash.cpp); without one, the title's.
  void set_font_splash(font::Font *f) { font_splash_ = f; }
  void set_dpi(float dpi) { dpi_ = dpi; }
  void set_tile_scale(int scale) { tile_scale_ = scale; }
  void set_user_agent(const std::string &agent) { agent_ = agent; }
  void set_setup_network(const std::string &name) { setup_network_ = name; }
  void set_brightness_output(output::FloatOutput *out) { brightness_ = out; }
  void set_version(const std::string &v) { version_ = v; }
  void set_panel_te_pin(int pin) { panel_te_pin_ = pin; }
  void set_round(bool round) { round_ = round; }
  // The map card's slot (card_io.h), on a board that has one.
  void set_card(int slot, int width, int clk, int cmd, int d0, int d1, int d2, int d3, int hold_high, int power_ldo,
                int khz) {
    card_fitted_ = true;
    card_config_ = {slot, width, clk, cmd, d0, d1, d2, d3, hold_high, power_ldo, khz};
  }
#ifdef USE_SKYSCOPE_RADIO
  void set_radio(int gain_tenths, int ppm) {
    radio_config_.gain_tenths = gain_tenths;
    radio_config_.ppm = ppm;
  }
#endif
  void set_networks(Networks *networks) { known_networks_ = networks; }
#ifdef USE_TOUCHSCREEN
  // Every finger on the glass, for two-finger zoom and pan (preview.cpp): LVGL is told of the first finger only.
  void set_touchscreen(touchscreen::Touchscreen *touch) { touch->register_listener(this); }
  void update(const touchscreen::TouchPoints_t &points) override;
  void release() override;
#endif
#ifdef USE_UPDATE
  void set_firmware(http_request::HttpRequestUpdate *firmware) { firmware_ = firmware; }
  void set_coprocessor(update::UpdateEntity *coprocessor) { coprocessor_ = coprocessor; }
#endif
#ifdef USE_OTA_STATE_LISTENER
  // Firmware coming in by any of ESPHome's ways (web.cpp): one sent from a computer gets the updating page too.
  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *component) override;
#endif

  // For Home Assistant, through ESPHome's own API (home-assistant.yaml): what the screen sees, and the screen itself.
  // Each alert as it comes: its kind ("watched", "emergency"), the plane's name and the words.
  void add_on_alert_callback(std::function<void(std::string, std::string, std::string)> &&f) {
    alert_callbacks_.add(std::move(f));
  }
  const std::string &last_alert() const { return last_alert_; }
  uint32_t planes_in_range();   // in the last report around the place
  uint32_t planes_today();      // told apart by their address, since local midnight
  std::string nearest_plane();  // its name, "" for none
  float nearest_nm();           // NAN for none
  float nearest_altitude_ft();  // NAN for none, or on the ground
  std::string followed_state();  // "" while none is followed; else looking, live, lost, landed or gave_up
  bool screen_on() const { return !asleep_; }
  void set_screen_on(bool on);
  int brightness() const { return settings_.brightness; }
  void set_brightness(int percent);

  void setup() override;
  // A change still waiting its few seconds to be saved is saved before any restart (the API's, a network joined, new
  // firmware): it would be lost otherwise. The radio is stopped too: a restart mid-stream hangs (radio.h).
  void on_safe_shutdown() override {
    // Not on the way out of a factory reset: what was just erased would be written again.
    if (save_pending_ && !resetting_) save_settings_();
    if (!resetting_) save_stats_();
#ifdef USE_SKYSCOPE_RADIO
    if (radio_) radio_->stop();
#endif
  }
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
    // The plan of the next step while a plane is followed, for the worker to draw ahead (follow.h next_center); no
    // tiles for none.
    tiles::Plan ahead;
    uint32_t ahead_rev = 0;
    bool online = false;
    bool offline = false;  // the screen runs on its radio alone, off a network (offline.h)
    bool asleep = false;  // the screen is off: no planes or tiles are asked for
    bool updating = false;  // new firmware is on its way in: nothing else is asked for
    // worker -> loop
    std::vector<feed::Flight> flights;
    uint32_t flights_rev = 0;
    uint32_t feed_at = 0;  // millis() of the last good answer
    providers::Providers providers;  // each exchange's record: waiting, failures, pace (providers.h)
    Feed feed_source = ADSB_FI;      // the exchange the last good report came from
    bool feed_simulated = false;     // and it was a receiver whose planes are made up (feed.h credit)
    uint32_t frame_rev = 0, frame_plan_rev = 0;  // the base frame changed, for that plan
    int tiles_done = 0, tiles_total = 0;
    std::string tile_error;
    // How long the last report took: its download, reading its JSON (both on the worker), and its bytes.
    uint32_t fetch_ms = 0, parse_ms = 0, reply_bytes = 0;
    uint32_t feed_every_ms = 0;
    size_t cached_tiles = 0;  // the worker's tile cache, entries
    // Since the screen started: tiles fetched from their servers (and their bytes) and found in the cache, the cache's
    // bytes now, and frames shown whole again from the one kept (worker.cpp) with no tile drawn.
    uint32_t tiles_fetched = 0, tiles_from_cache = 0, frames_again = 0;
    uint64_t tile_bytes_fetched = 0;
    // How long the map takes, on the worker: a tile's download, its read from the card, its decoding (PNG or JPEG) and
    // its laying into the frame; frames drawn from their tiles, from the plan to the last; the last frame, where it
    // came from ("tiles", "kept", "ahead", or "ahead, part" with the rest drawn after) and the tiles it drew; and the
    // frames and tiles drawn ahead for a plane followed.
    struct TileTimes {
      tiles::Spent fetch, card, png, jpeg, lay, frames;
      uint32_t frame_ms = 0, frame_tiles = 0, frames_ahead = 0, tiles_ahead = 0;
      const char *frame_from = "";
    } tile_times;
    // The test API's benchmark of the tile decoder (worker.cpp bench): asked (bench_rev, for bench_runs runs of each),
    // running, and its answer as JSON.
    uint32_t bench_rev = 0;
    int bench_runs = 3;
    bool bench_busy = false;
    std::string bench_result;
    size_t cache_bytes = 0;
    uint32_t radar_scan = 0;  // the radar scan drawn over the map, minutes since 1970; 0 for none
    // The airspace and runways of the last place and range asked (airspace.h), which may be an earlier frame's while a
    // new one comes, and why the last request failed.
    std::shared_ptr<const airspace::Map> airspace;
    uint32_t airspace_rev = 0, airspace_bytes = 0;
    std::string airspace_error;
    uint32_t airspace_retry_at = 0;  // millis() when a request that failed is made again, 0 for none
    bool airspace_kept = false;  // the airspace shown is the one kept in flash from before the start, not asked
    // loop -> web: the firmware on the update server (web.cpp), as the update entity last told it.
    struct Firmware {
      bool known = false;  // a manifest was read
      bool available = false, installing = false;
      std::string latest, summary, release_url, error;
      float progress = 0;  // percent, while installing
    } firmware;
    // loop -> worker: the view's middle while a plane is followed (follow.h). worker -> loop: that plane as the reports
    // have seen it, for the follow named in follower_for.
    follow::Center center;
    uint32_t center_rev = 0;
    follow::Follower follower;
    std::string follower_for;
    uint32_t follower_rev = 0;
    follow::Route route;  // where it flies from and to (follow.h)
    // loop -> worker: the plane picked on the glass, for the route on its card; worker -> loop: that route.
    follow::RouteAsk route_ask;
    uint32_t route_ask_rev = 0;
    follow::Route picked_route;
    uint32_t picked_route_rev = 0;
    // loop -> worker: the plane whose photo the card wants ("" for none: the one held is let go); worker -> loop: it.
    std::string photo_ask;
    uint32_t photo_ask_rev = 0;
    std::shared_ptr<const PhotoImage> photo;
    uint32_t photo_rev = 0;
    // worker -> loop and web: what the screen has seen in its range (sightings.h), and the alerts not shown yet.
    sightings::Book book;
    uint32_t book_rev = 0;
    std::vector<sightings::Alert> alerts;
    // loop -> worker: a place to find, typed in the menu, or where the screen is (`search_here`, nothing typed);
    // worker -> loop: what was found for the search found_rev.
    std::string search;
    bool search_here = false;
    uint32_t search_rev = 0, found_rev = 0;
    std::vector<places::Found> found;
    std::string search_error;
    // loop -> worker: the weather page wants the weather (weather.h) again; worker -> loop: what came, for that ask.
    uint32_t weather_ask = 0, weather_rev = 0;
    std::shared_ptr<const weather::Report> weather;
    // The map card (card.h). web -> worker: an area to download (for download_ask), a download to stop (stop_ask), the
    // card to look for again (mount_ask). worker -> loop and web: the card and the download as they are.
    // clear_ask: the screen's tiles cleared; format_ask: the card formatted (mounted first if it has no FAT32).
    card::Area download;
    uint32_t download_ask = 0, stop_ask = 0, mount_ask = 0, clear_ask = 0, format_ask = 0;
    struct Card {
      bool mounted = false;
      std::string error;  // why there is none
      bool unformatted = false;  // there is a card, with no FAT32 on it: the page offers to format it
      uint64_t total = 0, free = 0;
      uint32_t cluster = 0;  // its clusters' bytes, 0 while not known
      // Tiles cleared away and still being deleted, and how many files and folders went so far.
      bool clearing = false;
      uint32_t cleared = 0;
      uint32_t read = 0, written = 0, failed = 0;  // tiles since the start
      int missing = 0;  // the frame's tiles not on the card, off a network
      // The download: its area, how far it got (tiles fetched, found on the card already, given up) and why it stopped
      // or waits.
      bool downloading = false;
      card::Area area;
      size_t next = 0, total_tiles = 0, fetched = 0, skipped = 0, given_up = 0;
      uint32_t waiting_s = 0;
      std::string download_error;
    } card;
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
  void build_menu_();  // the menu's objects, in the colors of the settings it is editing
  void close_menu_();
  void refresh_menu_();
  void menu_step_(int index, int step);
  void menu_action_(int action);
  void apply_menu_(const Settings &menu);  // the menu's changes, where there are any
  void refresh_menu_firmware_();  // the menu's firmware part, from what the update entity last said
  void refresh_menu_wifi_();      // the network the screen is on
  // A question over everything (menu.cpp), asked before what cannot be undone: Cancel, and a key that does it.
  void ask_(const std::string &title, const std::string &body, const char *yes, std::function<void()> &&then);
  void close_ask_();
  // Everything the screen keeps erased, and a restart as a new screen (skyscope.cpp): the menu's Factory reset, and
  // POST /api/reset. The map card is left as it is.
  void factory_reset_();
  // The tour of what a finger does here (tour.h, tour.cpp): shown over the map while settings_.tour is set.
  void tour_tick_(uint32_t now);
  tour::Seen tour_seen_() const;
  void open_tour_();
  void show_tour_();  // the card's words, dots and keys for the step the tour is at
  void close_tour_();
  void end_tour_();   // taken or ended: not shown again until it is asked for
  // A key of the menu: a row's step back or on (row >= 0), or one of its actions (row < 0, MENU_* in menu.cpp). Its
  // face is `label`, or an arrow drawn (`arrow` one of menu.cpp's ARROW_*) where `label` is nullptr. A primary key is
  // filled with the accent.
  lv_obj_t *key_(lv_obj_t *parent, const char *label, int row, int step, int width, int height, int arrow = -1,
                 bool primary = false);
  lv_obj_t *button_(lv_obj_t *parent, const char *label, int width, int height, int arrow = -1, bool primary = false);
  // The keyboard (editor.cpp), over the menu: a row's value, a place to find, or a Wi-Fi network's name or password.
  enum Typing : uint8_t { TYPING_ROW, TYPING_PLACE, TYPING_NETWORK, TYPING_PASSWORD };
  // A place to find (editor.cpp): `q` typed, or nothing for where the screen is; its list opened to wait for it.
  void search_place_(const std::string &q);
  void open_row_editor_(int row);
  void open_editor_(Typing what, const std::string &title, const std::string &text, keyboard::Pad pad, size_t most,
                    const std::string &hint);
  void build_keys_();  // the keys of the keyboard's page now
  void press_key_(int index);
  void refresh_editor_();
  void editor_done_();
  void close_editor_();
  // A list to pick from, over the keyboard or the menu (editor.cpp): the places a search found, or the networks in
  // reach.
  enum Picking : uint8_t { PICKING_PLACES, PICKING_NETWORKS };
  void open_picker_(Picking what);
  void refresh_picker_();       // its line, from the search or the scan under way
  void fill_picker_();          // its items, built again
  bool picker_stale_{false};    // a scan answered under a finger: its items built again once it lifts
  void picker_pick_(int index);
  void close_picker_();
  void scan_networks_();
  void networks_from_(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results);
  void join_network_(const std::string &name, const std::string &password);
  void known_key_(int code);  // a known network's key: up, down, Forget
  int forget_armed_{-1};       // the known network Forget's second tap would forget, until forget_armed_until_
  uint32_t forget_armed_until_{0};
  void set_brightness_now_(int percent);
  // Two fingers zoom and pan a preview of another view (preview.cpp): a picture of the glass as they landed, stretched
  // and shifted as they move, the view planned once they have been still for a while, and kept or dropped by the bar's
  // Save and Cancel (or dropped when its countdown ends).
  bool can_pinch_() const;
  bool touch_locked_() const {
#ifdef SKYSCOPE_TEST_API
    return touch_locked_until_ != 0;
#else
    return false;
#endif
  }
  void begin_preview_();
  bool freeze_();       // the picture taken, the live planes hidden
  void draw_frozen_();  // the picture into the canvas, as the view previewed shows it
  void unfreeze_();     // a frame of the plan's own is in the canvas: the live planes again
  void end_pinch_();
  void settle_preview_();
  void use_view_(const Settings &s, const follow::Center *center = nullptr);
  void end_preview_(bool keep);
  void preview_tick_(uint32_t now);
  // A swipe along the strip at the bottom changes the map (swipe.cpp): its name shown at once, the map planned and
  // saved once the swipes have stopped for a while.
  void on_released_(lv_point_t at);
  void show_swipe_();
  void swipe_tick_(uint32_t now);
  void finish_swipe_();  // the map swiped to, at once (the menu opening, the screen going off)
  // The weather page (wxpage.cpp): the station's METAR, TAF and MOS and the area's advisories, over everything; a swipe
  // up from the bottom strip, or the menu, opens it.
  void open_weather_();
  void close_weather_();
  void ask_weather_();
  void weather_tab_(int tab);
  void fill_weather_();
  void refresh_weather_();  // what the worker found, and another ask when it is old
  int64_t unix_now_() const;  // the clock's Unix seconds, 0 while it is not set
  // The day's tally on the glass (todaypage.cpp): a swipe down from the top edge, or the menu, opens it.
  void open_today_();
  void refresh_today_();
  void close_today_();
  // The splash at the start (splash.cpp): the emblem turning to rest over the screen's name, then gone.
  void start_splash_();
  void splash_tick_(uint32_t now);
  void stay_on_top_();
  void end_splash_();
#ifdef SKYSCOPE_TEST_API
  // A test's mock in place of every server (/api/test {"divert": origin}, http.h): kept through a restart, which is how
  // it starts and ends (every cache empty, the map card left out), and gone at power off. "" for none.
  static std::string kept_divert_();
  static void keep_divert_(const std::string &origin);
  std::string divert_;
  // Tests without a finger (probe.cpp): a touch as a second input device, and what the glass shows.
  void make_probe_input_();
  void probe_touch_start_(const touch::Script &s);  // on the loop
  void describe_ui_(JsonObject out);                  // on the loop
  void handle_probe_(AsyncWebServerRequest *request, const std::string &url, const std::string &body);
  void send_screenshot_(AsyncWebServerRequest *request);  // GET /api/screenshot, in the web server's task
  touch::Player probe_touch_;
  lv_indev_t *probe_input_{nullptr};
  // Fingers on the glass taken no notice of while a test runs (/api/test {"lock_touch_s": N}), so one cannot upset
  // it: LVGL's touch input off, two fingers ignored, and a touch does not wake the screen. The test's own touches
  // (the probe's) go on. Kept over a restart, as the divert is, and over by itself N seconds after it was asked.
  uint32_t touch_locked_until_{0};
  lv_obj_t *touch_lock_note_{nullptr};
  static uint32_t kept_touch_lock_s_();
  static void keep_touch_lock_(uint32_t seconds);
  void lock_touch_(uint32_t seconds);  // 0 unlocks; on the loop
  void touch_lock_tick_(uint32_t now);
  uint32_t probe_busy_until_{0};  // the web server's task: the touch under way ends then
#endif
  // The colors of what lies over the map: the radar's phosphor on a scope, else the screen's own.
  struct Chrome { uint32_t ink, muted, card, accent; };
  Chrome chrome_() const;
  static Chrome chrome_for_(const Settings &s);
  void restyle_();
  void set_status_color_(uint32_t color);
  // The map card's state for the API (web.cpp), from a copy of Shared::Card.
  void card_json_(JsonObject out, const Shared::Card &c) const;
  void stats_json_(JsonObject out) const;  // what the screen has seen today, lock_ held (web.cpp)
  // Off a network, a screen with a radio shows the radar from it alone (offline.h). shown_ is settings_ as the glass
  // shows them: the same, or offline::shown of them. Drawing goes by shown_; what is saved and edited is settings_.
  // Following a plane (follow.h) moves the view's middle with it, and so shown_ is follow::shown of them too.
  const Settings &shown_() const { return shown_settings_; }
  void reshow_();  // shown_settings_ again, from settings_, offline_ and center_
  void go_offline_(bool offline);
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
  Networks *known_networks_{nullptr};  // the networks the screen knows, first choice first (networks.h)
  lvgl::LvglComponent *lvgl_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *clock_{nullptr};
#endif
  int bed_minute_{-1};  // the minute of the day bedtime was last looked at; -1 before the first
  int native_width_{0}, native_height_{0};  // the panel's own way up, rotation 0
  font::Font *font_small_{nullptr}, *font_label_{nullptr}, *font_title_{nullptr}, *font_splash_{nullptr};
  output::FloatOutput *brightness_{nullptr};
  float dpi_{200};
  int tile_scale_{1};
  int panel_te_pin_{-1};
  bool round_{false};  // the glass is a circle (glass.h)
  bool card_fitted_{false};
  card_io::Config card_config_;
#ifdef USE_SKYSCOPE_RADIO
  Radio::Config radio_config_;
  std::unique_ptr<Radio> radio_;  // the screen's own radio (radio.h), from setup on
#endif
  std::string agent_{"esp-skyscope"}, setup_network_, version_;

  // The loop's own.
  Settings settings_;
  offline::Tracker offline_tracker_;
  bool offline_{false}, offline_card_{false};  // offline_card_: offline_settings_ were made with a map card in
  Settings offline_settings_;  // offline::shown(settings_), while offline_
  Settings shown_settings_;
  // The plane followed (follow.h): the view's middle, the plane as the worker last told of it and for which follow, and
  // the middle the view had when a preview began (preview.cpp), for its Cancel.
  follow::Center center_, held_center_;
  follow::Follower follower_;
  std::string follower_for_;
  uint32_t follower_rev_{0};
  // Since when the screen has been on and online to ask for the plane: no stop is decided before it has had the time
  // to (asleep, nothing is asked, and the plane only looks lost).
  uint32_t follow_asking_since_{0};
  follow::Trail trail_;  // its way so far, for its track (follow.h)
  std::string ranged_hex_;  // the plane last put at its own range on being followed (follow_tick_)
  // The view of the next step, as given to the worker to draw ahead (follow.h next_center), and when it was last
  // looked at; ahead_.on false for none.
  follow::Center ahead_;
  geo::View ahead_view_;
  uint32_t ahead_at_{0};
  void plan_ahead_(uint32_t now, const geo::Place &plane);
  void drop_ahead_();
  tiles::Plan plan_for_(const Settings &shown) const;
  bool following_() const { return center_.on; }
  void follow_tick_(uint32_t now);  // the follower from the worker, the view moved with the plane, a landing's stop
  // The view's middle there (and the range `range_nm`, where it is one), its frame planned, the glass as it was moved
  // to it meanwhile.
  void set_center_(const follow::Center &c, int range_nm = 0);
  // The keys down the left edge while a plane is followed: the whole route in view, the range the screen had before
  // following changed it, or the plane at its own range (follow.h plane_range_nm). show_plane_ also begins following,
  // at that range; `range_nm` 0 is the plane's own.
  void show_route_();
  void show_plane_(int range_nm = 0);
  void refresh_follow_keys_();
  follow::Route route_;
  bool route_view_{false};  // the whole route is in view: the view moves only once the plane would leave it
  lv_obj_t *follow_keys_{nullptr}, *route_key_{nullptr}, *own_key_{nullptr}, *plane_key_{nullptr};
  // Their symbols, drawn (the fonts have none): the route's leg and the sight's four ticks, their points kept here as
  // LVGL keeps only a pointer to them.
  lv_point_precise_t route_leg_[3]{}, sight_ticks_[4][2]{}, own_handle_[2]{}, own_roof_[3]{};
  lv_obj_t *round_key_(lv_obj_t *parent, int across);
  void draw_route_symbol_(lv_obj_t *key, int side);
  void draw_sight_symbol_(lv_obj_t *key, int side);
  void draw_own_symbol_(lv_obj_t *key, int side);
  // The card's line of altitude, the arrow of a climb or descent (drawn: the fonts have none) and the rest after it.
  lv_obj_t *card_motion_{nullptr}, *card_arrow_{nullptr}, *card_after_{nullptr};
  lv_point_precise_t card_arrow_points_[5]{};
  int card_arrow_way_{0};
  size_t route_points_{0};  // the route's points on the glass in the last overlay, for the test API
  void follow_picked_();  // the card's Follow, or Stop following, for the plane picked
  lv_obj_t *card_follow_{nullptr};
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
  uint32_t status_color_{0xFFFFFFFF};
  uint32_t address_until_{0}, last_tick_{0}, last_status_{0};
  bool asleep_{false};
  bool wake_asked_{false};  // the screen woken by something but a finger: the probe, the clock, Home Assistant
  // An install: the screen says so, and the worker rests, until the screen restarts or the install fails.
  firmware::Tracker install_;
#ifdef USE_UPDATE
  http_request::HttpRequestUpdate *firmware_{nullptr};
  update::UpdateEntity *coprocessor_{nullptr};
#endif
  // The Wi-Fi chip's firmware being brought in line with the one this firmware carries: tried once a start.
  bool coprocessor_tried_{false}, coprocessor_updating_{false};
  void update_coprocessor_();
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
  std::vector<lv_obj_t *> menu_switches_;  // by row: the switch of a flag that is on or off, nullptr for the others
  lv_obj_t *menu_wifi_{nullptr}, *menu_wifi_value_{nullptr};
  // The menu's keys that are no setting: the pages, the tour, a restart and the factory reset.
  lv_obj_t *menu_weather_{nullptr}, *menu_today_{nullptr}, *menu_tour_{nullptr}, *menu_restart_{nullptr};
  lv_obj_t *menu_reset_{nullptr};
  // The question asked over the menu, its keys, and what its yes does.
  lv_obj_t *ask_page_{nullptr}, *ask_yes_{nullptr}, *ask_no_{nullptr}, *ask_title_{nullptr};
  std::function<void()> ask_then_;
  uint32_t ask_at_{0};  // when it came up: its keys take no tap in its first moment
  bool resetting_{false};  // a factory reset is under way: the screen says so, and nothing more is stored
  // The tour: where it is, its card and the card's parts, and when the glass was last looked at for it.
  tour::Guide tour_guide_;
  lv_obj_t *tour_{nullptr}, *tour_dots_{nullptr}, *tour_title_{nullptr}, *tour_body_{nullptr};
  lv_obj_t *tour_end_{nullptr}, *tour_next_{nullptr};
  uint32_t tour_looked_at_{0};
  // The keyboard: what is typed, for which row, and its objects. Its keys by index, as built for the page showing.
  lv_obj_t *editor_{nullptr}, *editor_title_{nullptr}, *editor_field_{nullptr}, *editor_text_{nullptr};
  lv_obj_t *editor_cursor_{nullptr}, *editor_hint_{nullptr}, *editor_keys_{nullptr}, *editor_done_key_{nullptr};
  lv_obj_t *editor_show_{nullptr};
  lv_obj_t *editor_locate_{nullptr};  // the place's Locate: where the screen is, for nothing typed
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
  // The networks as the list on the glass shows them: a scan that ends while a finger is on the list changes
  // networks_ and not the list, and a tap picks what it shows.
  std::vector<Network> listed_networks_;
  uint32_t scanning_until_{0};  // a scan asked for, until it answers or this
  std::string network_name_;   // the network whose password is being typed
  std::string joining_;        // the network the screen restarts to join
  bool asked_where_{false};    // the menu opened by itself, once, for a screen with no place yet
  uint32_t menu_install_armed_until_{0};  // Install takes a second tap before this
  uint32_t menu_checking_until_{0}, menu_checking_from_{0};  // a check asked for in the menu, and the reports then
  uint32_t firmware_reports_{0};  // the update entity's reports so far
  bool long_pressed_{false};  // the click that ends a long press (which opened the menu) is no tap
  lv_point_t pressed_at_{};   // where the finger on the map came down
  // The card of the plane picked: its title, and rows of its type, its motion, its operator and route, and which way to
  // look for it from the place (overlay.h sight_text).
  lv_obj_t *card_{nullptr}, *card_title_{nullptr}, *card_rows_[4]{};
  // The picked plane's route (follow.h), as the worker found it, and the plane it was asked for.
  follow::Route picked_route_;
  uint32_t picked_route_rev_{0};
  std::string route_asked_hex_, route_asked_callsign_;
  void ask_picked_route_(const feed::Flight &f);
  // The picked plane's photo (photos.h): beside the QR code of its page, with its photographer under them.
  lv_obj_t *card_photo_{nullptr}, *card_photo_image_{nullptr}, *card_qr_{nullptr}, *card_photo_credit_{nullptr};
  std::shared_ptr<const PhotoImage> photo_;
  lv_image_dsc_t photo_dsc_{};
  std::string photo_asked_hex_;
  uint32_t photo_rev_{0};
  void ask_photo_(const std::string &hex);  // "" lets the one held go
  void show_photo_();
  // An alert (sightings.h) along the top of the glass for a while: a watched plane come into range, or an emergency.
  // A tap picks the plane; Home Assistant hears of each one (on_alert).
  lv_obj_t *alert_{nullptr}, *alert_text_{nullptr};
  uint32_t alert_until_{0};
  std::string alert_hex_;
  void take_alerts_(uint32_t now);
  void show_alert_(const sightings::Alert &a);
  CallbackManager<void(std::string, std::string, std::string)> alert_callbacks_;
  std::string last_alert_;
  uint32_t alerts_seen_{0};
  // The day's tally kept over a restart (sightings.h pack): read at the start, stored every so often.
  ESPPreferenceObject stats_pref_;
  uint32_t stats_saved_at_{0}, stats_saved_rev_{0};
  void load_stats_();
  void save_stats_();
  lv_obj_t *setup_{nullptr}, *setup_title_{nullptr}, *setup_body_{nullptr};
  lv_obj_t *setup_wifi_{nullptr};  // the setup page's way to the networks in reach, while it has none
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
  // The worker's: the last frame of tiles it drew whole, for its view and map wanted again (a map swiped away and
  // back). Swapped with base_frame_ under frame_lock_.
  uint16_t *kept_frame_{nullptr};
  // The worker's: the map whole, without the weather radar drawn over it (radar.h), while the radar is shown.
  uint16_t *clean_frame_{nullptr};
  TaskHandle_t worker_task_{nullptr};

  // The preview: the fingers, the settings as they were (kept until Save), the view previewed, when it is planned
  // (settle_at_) and dropped (preview_until_). The picture taken as the fingers landed and its view: in the canvas
  // (frozen_shown_) until a frame of the plan's own is whole.
  pinch::Fingers fingers_;
  size_t finger_count_{0};      // the fingers the gesture is following: 1 (a drag) or 2
  bool touch_down_{false};      // a finger is on the glass
  bool touch_on_map_{false};    // and it came down on the map itself, not on what lies over it
  bool gestured_{false};        // this touch zoomed or panned: no tap, long press or drag comes of it
  pinch::Point touch_from_;     // where the first finger came down
  bool previewing_{false};
  Settings held_;
  geo::View target_view_, frozen_view_;
  uint32_t settle_at_{0}, preview_until_{0}, planned_at_{0}, drawn_frozen_at_{0};
  uint16_t *frozen_{nullptr};
  bool frozen_shown_{false}, frozen_dirty_{false};
  std::vector<int> frozen_columns_;
  lv_obj_t *preview_bar_{nullptr}, *preview_text_{nullptr};
  bool touch_in_strip_{false};  // the finger came down in a swipe's strip (the bottom's or the top's): no pan
  // The swipes: the settings with the map swiped to, and when it is planned; the name and dots shown meanwhile.
  Settings swiped_;
  bool swiping_{false};
  uint32_t swipe_at_{0};
  lv_obj_t *swipe_card_{nullptr}, *swipe_name_{nullptr}, *swipe_dots_{nullptr};
  // The weather page: its objects, the tab shown, the ask made, and the report shown and when it came.
  lv_obj_t *wx_{nullptr}, *wx_title_{nullptr}, *wx_line_{nullptr}, *wx_list_{nullptr}, *wx_tabs_[4]{};
  int wx_tab_{0};
  uint32_t wx_asked_{0}, wx_at_{0};
  std::shared_ptr<const weather::Report> wx_report_;

  // The day's tally: its page, its line, the chart of the hours and its bars, the list under them, the tally shown
  // (sightings.h's revision) and when it was last looked at.
  lv_obj_t *today_{nullptr}, *today_line_{nullptr}, *today_chart_{nullptr}, *today_list_{nullptr};
  lv_obj_t *today_bars_[24]{};
  // Its three numbers' tiles, and its lines in words as last shown (for /api/ui).
  lv_obj_t *today_tiles_{nullptr}, *today_stat_values_[3]{}, *today_stat_captions_[3]{};
  std::vector<std::string> today_texts_;
  uint32_t today_rev_{0}, today_at_{0};
  // The splash: its page, the emblem and the pixels it is drawn into (freed with it), when it began, and its frames.
  lv_obj_t *splash_{nullptr}, *splash_canvas_{nullptr}, *splash_name_{nullptr}, *splash_line_{nullptr};
  std::unique_ptr<emblem::Emblem> splash_emblem_;
  uint16_t *splash_pixels_{nullptr};
  uint32_t splash_from_{0}, splash_drawn_at_{0}, splash_frames_{0};
  int64_t splash_draw_us_{0}, splash_first_us_{0};
  uint32_t splash_pixels_sent_{0};
  bool splash_rested_{false}, splash_waiting_{false};
  // The glass's frame when the splash's first was drawn, and when its last was: the next waits for the glass.
  uint32_t splash_frame_at_{0};
  uint32_t splash_drawn_frame_{0};
  // The last one, for /api/ui: its frames and the longest wait between two looks at it.
  uint32_t splash_ticked_at_{0}, splash_gap_ms_{0}, splash_last_frames_{0}, splash_last_gap_ms_{0};

  // The body of a POST on its way in (the HTTP server's task).
  std::string body_;
};

// Each alert as it comes (__init__.py on_alert): its kind, the plane's name, and the words.
class AlertTrigger : public Trigger<std::string, std::string, std::string> {
 public:
  explicit AlertTrigger(Skyscope *parent) {
    parent->add_on_alert_callback(
        [this](std::string kind, std::string name, std::string text) { this->trigger(kind, name, text); });
  }
};

}  // namespace esphome::skyscope
