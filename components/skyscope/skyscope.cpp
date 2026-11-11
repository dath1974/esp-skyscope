// The loop's side of Skyscope: the settings, what the glass shows and what a finger does.
#include "skyscope.h"
#include "http.h"

#include "colors.h"
#include "panel_sync.h"
#include "scope.h"
#include "ui.h"
#include "widgets.h"
#include "swipe.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome::skyscope {
static const char *const TAG = "skyscope";

namespace {
// The stored settings: their JSON, in ESPHome's preferences (NVS). A key the firmware does not know is skipped, a key
// it lacks keeps its default, so an update never loses the settings.
constexpr uint32_t PREF_HASH = 0x5C75C0BE;
struct Stored {
  char json[1800];
};
constexpr uint32_t SAVE_DELAY_MS = 4000;
// The farthest a plane's card says which way to look for it: past this it is a speck at best.
constexpr double SIGHT_MOST_NM = 60;
// An alert stays on the glass this long, unless tapped; the day's tally is stored at most this often.
constexpr uint32_t ALERT_MS = 20000, STATS_SAVE_MS = 30 * 60 * 1000;
// The day's tally as stored (sightings.h pack), with its size.
struct StoredStats {
  uint32_t size;
  uint8_t bytes[30000];
};
constexpr uint32_t STATS_PREF_HASH = 0x5D57A751;
// A tap on the map shows the screen's address and range in the status line for this long.
constexpr uint32_t ADDRESS_MS = 6000;
// A finger picks the plane nearest it within this many reference pixels.
constexpr int PICK_REACH = 36;
// After the touch that woke the screen, taps do nothing for this long: the finger that woke it is not a tap on a plane.
constexpr uint32_t WAKE_DEAF_MS = 800;
// The brightness strip: its width, the lowest brightness a drag gives (0 is the screen off), how far up the glass a
// drag goes from darkest to brightest (a share of its height), how far a finger moves before it is a drag, and how
// long the level shows after.
constexpr float DIMMER_MM = 7;
constexpr int DIM_LOWEST = 5;
constexpr float DRAG_SPAN = 0.7f;
constexpr int DRAG_PX = 6;
constexpr uint32_t LEVEL_MS = 1500;
// A long press is a finger that moved no farther than this.
constexpr float STILL_MM = 1.5f;
// A part of the map comes at most this often while its tiles arrive; the last one at once.
constexpr uint32_t FRAME_EVERY_MS = 600;
// After two fingers chose another view (preview.cpp), its old frame stays stretched to it this long at most, until the
// new one is whole.
constexpr uint32_t WHOLE_FRAME_MS = 8000;
// Planes that glide move this often, each in a small part of the glass.
constexpr uint32_t GLIDE_FRAME_MS = 200;
// The most places of the followed plane's track drawn (follow.h Trail), spread along it.
constexpr size_t TRAIL_POINTS = 160;

// Frame timing: when LVGL starts a frame and when the last of it has gone to the panel (its wait for the panel's
// tearing signal included), summed over five seconds for /api/status.
int64_t frame_started_us = 0, window_started_us = 0, flush_started_us = 0;
int64_t window_send_us = 0, window_wait_from_us = 0;
uint64_t window_pixels = 0;
uint32_t window_frames = 0;
float window_ms = 0, window_longest_ms = 0;
uint8_t frame_phase = ui::IDLE;
bool frame_after_report = false;
uint64_t frame_pixels_from = 0;
uint32_t frame_flushes = 0;
void frame_start(lv_event_t *) {
  ++ui::frame;
  frame_started_us = esp_timer_get_time();
  frame_pixels_from = window_pixels;
  frame_flushes = 0;
  for (auto &us : ui::part_us) us = 0;
  ui::draw_calls = 0;
  frame_phase = ui::worker_phase;
  frame_after_report = ui::report_taken;
  ui::report_taken = false;
}
// Registered after panel_sync's own, so its wait for the pulse is not counted as sending.
void flush_start(lv_event_t *e) {
  flush_started_us = esp_timer_get_time();
  if (auto *area = static_cast<lv_area_t *>(lv_event_get_param(e))) window_pixels += lv_area_get_size(area);
  ++frame_flushes;
}
void flush_finish(lv_event_t *) {
  if (flush_started_us) window_send_us += esp_timer_get_time() - flush_started_us;
  flush_started_us = 0;
}
void frame_ready(lv_event_t *) {
  const int64_t now = esp_timer_get_time();
  if (!frame_started_us) return;
  const float ms = (now - frame_started_us) / 1000.0f;
  frame_started_us = 0;
  if (ms > ui::SLOW_MS) {
    // Put down to what the worker was doing as it started or is doing now, whichever is busier.
    const uint8_t now_phase = ui::worker_phase, phase = std::max(frame_phase, now_phase);
    ++ui::slow_frames[phase < ui::PHASES ? phase : 0];
    if (frame_after_report) ++ui::slow_with_report;
    ESP_LOGI(TAG, "Slow frame: %.0f ms, %u px in %u flushes, worker %s%s; planes drawn %u times: tracks %.1f, "
             "markers %.1f, layout %.1f, labels %.1f ms", ms,
             (unsigned) (window_pixels - frame_pixels_from), (unsigned) frame_flushes, ui::PHASE_NAMES[phase],
             frame_after_report ? ", after a report" : "", (unsigned) ui::draw_calls, ui::part_us[ui::TRACKS] / 1000.0f,
             ui::part_us[ui::MARKERS] / 1000.0f, ui::part_us[ui::LAYOUT] / 1000.0f, ui::part_us[ui::LABELS] / 1000.0f);
  }
  ++window_frames;
  window_ms += ms;
  window_longest_ms = std::max(window_longest_ms, ms);
  if (!window_started_us) window_started_us = now;
  if (now - window_started_us >= 5000000) {
    const float seconds = (now - window_started_us) / 1e6f;
    const int64_t waited = panel_sync::waited_us;
    const float n = window_frames;
    const float wait_ms = (waited - window_wait_from_us) / 1000.0f / n, send_ms = window_send_us / 1000.0f / n;
    ui::frame_stats = {window_frames, n / seconds, window_ms / n, window_longest_ms,
                       window_ms / n - wait_ms - send_ms, wait_ms, send_ms, (float) window_pixels / n};
    window_started_us = now;
    window_frames = 0;
    window_ms = window_longest_ms = 0;
    window_send_us = 0;
    window_pixels = 0;
    window_wait_from_us = waited;
  }
}

lv_obj_t *panel(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_style_bg_color(o, lv_color_hex(colors::CARD), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
  lv_obj_set_style_radius(o, ui::px(8), 0);
  lv_obj_set_style_pad_all(o, ui::px(6), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, "");
  return l;
}
void set_text(lv_obj_t *l, const std::string &text) {
  const char *now = lv_label_get_text(l);
  if (!now || text != now) lv_label_set_text(l, text.c_str());
}
void set_shown(lv_obj_t *o, bool shown) {
  if (shown == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
    if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}
}  // namespace

std::string own_address() {
  if (!wifi::global_wifi_component || !wifi::global_wifi_component->is_connected()) return "";
  for (const auto &ip : wifi::global_wifi_component->get_ip_addresses()) {
    if (!ip.is_set() || !ip.is_ip4()) continue;
    char text[network::IP_ADDRESS_BUFFER_SIZE];
    return ip.str_to(text);
  }
  return "";
}

// ---------------------------------------------------------------- settings

void Skyscope::load_settings_() {
  pref_ = global_preferences->make_preference<Stored>(PREF_HASH);
  Stored stored{};
  if (!pref_.load(&stored)) return;
  stored.json[sizeof(stored.json) - 1] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, stored.json)) {
    ESP_LOGW(TAG, "The stored settings could not be read: defaults");
    return;
  }
  // Key by key, so one key that no longer passes leaves the others.
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    JsonDocument one;
    one[kv.key()] = kv.value();
    std::string error;
    if (!apply(one.as<JsonObjectConst>(), settings_, error)) ESP_LOGW(TAG, "Stored setting %s skipped", error.c_str());
  }
}

void Skyscope::save_settings_() {
  Stored stored{};
  JsonDocument doc;
  // While two fingers preview another view, the view saved stays the one before them until Save (preview.cpp).
  Settings kept = settings_;
  if (previewing_) {
    kept.located = held_.located;
    kept.lat = held_.lat;
    kept.lon = held_.lon;
    kept.range_nm = held_.range_nm;
    kept.place = held_.place;
  }
  to_json(kept, doc.to<JsonObject>(), true);
  const size_t n = serializeJson(doc, stored.json, sizeof(stored.json) - 1);
  if (n == 0 || n >= sizeof(stored.json) - 1) {
    ESP_LOGE(TAG, "The settings are too long to store");
    return;
  }
  pref_.save(&stored);
  global_preferences->sync();
  save_pending_ = false;
  ESP_LOGI(TAG, "Settings saved");
}

void Skyscope::use_settings_(const Settings &s) {
  // Another way up takes new buffers and a new layout of everything on the glass: saved, and the screen starts again
  // that way up.
  if (s.rotation != settings_.rotation && ui_built_) {
    settings_ = s;
    save_settings_();
    ESP_LOGI(TAG, "Rotation %d: restarting", s.rotation);
    this->set_timeout(400, []() { App.safe_reboot(); });
    return;
  }
  const bool frame_changed = s.lat != settings_.lat || s.lon != settings_.lon || s.range_nm != settings_.range_nm ||
                             s.map != settings_.map || s.custom_url != settings_.custom_url ||
                             s.theme != settings_.theme || s.map_brightness != settings_.map_brightness ||
                             s.tile_scale != settings_.tile_scale ||
                             s.rings != settings_.rings || s.tint != settings_.tint || s.located != settings_.located ||
                             s.scope != settings_.scope ||
                             s.place_names != settings_.place_names;
  if (s.lat != settings_.lat || s.lon != settings_.lon) tracks_.clear();
  const bool new_zone = s.timezone != settings_.timezone;
  const bool new_bedtime = s.bedtime != settings_.bedtime || s.bed_off != settings_.bed_off || s.bed_on != settings_.bed_on;
  settings_ = s;
  if (offline_) offline_settings_ = offline::shown(settings_, offline_card_);
  reshow_();
  if (new_zone) use_timezone_();
  if (new_bedtime) bed_minute_ = -1;
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.settings = settings_;
    shared_.settings_rev++;
    // The new frame in the same turn of the lock: the worker never sees the settings without the frame they make (it
    // asked for the airspace of the old place once, with the airspace just switched on).
    if (frame_changed) replan_locked_();
  }
  restyle_();
  // The planes again, in the new style.
  seen_flights_rev_ = AGAIN;
  save_pending_ = true;
  save_at_ = millis() + SAVE_DELAY_MS;
}

void Skyscope::reshow_() {
  shown_settings_ = follow::shown(offline_ ? offline_settings_ : settings_, offline_ ? follow::Center{} : center_);
}

Skyscope::Chrome Skyscope::chrome_() const { return chrome_for_(shown_()); }

Skyscope::Chrome Skyscope::chrome_for_(const Settings &s) {
  if (s.map != RADAR) {
    const colors::Palette &p = colors::palette(s.theme == LIGHT);
    return {p.ink, p.muted, p.card, s.theme == LIGHT ? colors::LIGHT_ACCENT : colors::ACCENT};
  }
  const colors::Tint &t = colors::tint(s.tint);
  return {t.glow, colors::glow_share(t, 0.6f), t.ground, t.hot};
}

void Skyscope::set_status_color_(uint32_t color) {
  // Set only when it changes: LVGL draws a label again whenever a style is set, the same value or not.
  if (color == status_color_) return;
  status_color_ = color;
  lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
}

void Skyscope::restyle_() {
  if (!ui_built_) return;
  const Chrome c = chrome_();
  lv_obj_set_style_bg_color(status_, lv_color_hex(c.card), 0);
  status_color_ = 0xFFFFFFFF;
  lv_obj_set_style_text_color(credit_, lv_color_hex(c.muted), 0);
  lv_obj_set_style_bg_color(card_, lv_color_hex(c.card), 0);
  lv_obj_set_style_border_color(card_, lv_color_hex(c.muted), 0);
  const bool radar = shown_().map == RADAR;
  lv_obj_set_style_border_width(card_, radar ? std::max(1, ui::px(1)) : 0, 0);
  lv_obj_set_style_radius(card_, radar ? 0 : ui::px(8), 0);
  // The brightness bar: the scope's phosphor on the radar, the accent over a map.
  lv_obj_set_style_bg_color(level_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_color(level_fill_, lv_color_hex(radar ? colors::tint(settings_.tint).glow : colors::ACCENT), 0);
  lv_obj_set_style_text_color(level_text_, lv_color_hex(c.ink), 0);
  for (auto *row : card_rows_) lv_obj_set_style_text_color(row, lv_color_hex(c.muted), 0);
  lv_obj_set_style_text_color(card_photo_credit_, lv_color_hex(c.muted), 0);
  lv_obj_set_style_text_color(card_after_, lv_color_hex(c.muted), 0);
  lv_obj_set_style_line_color(card_arrow_, lv_color_hex(c.muted), 0);
  for (lv_obj_t *key : {route_key_, own_key_, plane_key_}) {
    if (!key) continue;
    lv_obj_set_style_bg_color(key, lv_color_hex(c.card), 0);
    // Its symbol's lines and rings in the ink, its dot filled with it.
    lv_obj_t *symbol = lv_obj_get_child(key, 0);
    for (uint32_t i = 0; symbol && i < lv_obj_get_child_count(symbol); ++i) {
      lv_obj_t *part = lv_obj_get_child(symbol, i);
      lv_obj_set_style_line_color(part, lv_color_hex(c.ink), 0);
      lv_obj_set_style_border_color(part, lv_color_hex(c.ink), 0);
      lv_obj_set_style_bg_color(part, lv_color_hex(c.ink), 0);
    }
  }
  lv_obj_set_style_bg_color(alert_, lv_color_hex(c.card), 0);
  lv_obj_set_style_text_color(alert_text_, lv_color_hex(c.ink), 0);
  lv_obj_set_style_bg_color(card_follow_, lv_color_hex(c.accent), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(card_follow_, 0), lv_color_hex(c.card), 0);
  last_status_ = 0;
  update_card_();
}

void Skyscope::go_offline_(bool radio_only) {
  offline_ = radio_only;
  {
    std::lock_guard<std::mutex> guard(lock_);
    offline_card_ = shared_.card.mounted;
  }
  if (offline_) offline_settings_ = offline::shown(settings_, offline_card_);
  reshow_();
  ESP_LOGI(TAG, "%s", !offline_       ? "On the network again"
                      : offline_card_ ? "No network: the map from its card, with the radio's planes"
                                      : "No network: the radar, with the radio's planes");
  {
    // The worker told in the same turn of the lock as the new frame, so it never asks the network for the old one.
    std::lock_guard<std::mutex> guard(lock_);
    shared_.offline = offline_;
    replan_locked_();
  }
  restyle_();
  seen_flights_rev_ = AGAIN;
}

void Skyscope::replan_() {
  std::lock_guard<std::mutex> guard(lock_);
  replan_locked_();
}

tiles::Plan Skyscope::plan_for_(const Settings &shown) const {
  const int scale = settings_.tile_scale ? settings_.tile_scale : tile_scale_;
  tiles::Plan p = tiles::plan(shown, width_, height_, scale);
  p.view.round = round_;
  return p;
}

void Skyscope::replan_locked_() {
  if (width_ <= 0) return;
  plan_ = plan_for_(shown_());
  shared_.plan = plan_;
  plan_rev_ = ++shared_.plan_rev;
}

// ---------------------------------------------------------------- component

void Skyscope::use_timezone_() {
#if defined(USE_TIME) && defined(USE_TIME_TIMEZONE)
  schedule::Zone z;
  if (!schedule::parse(settings_.timezone, z)) return;
  // The same parts ESPHome's clock takes from a zone built in (posix_tz.h), here read at any time.
  time::ParsedTimezone tz{};
  tz.std_offset_seconds = z.std_offset_seconds;
  tz.dst_offset_seconds = z.dst_offset_seconds;
  auto rule = [](const schedule::Rule &r) {
    time::DSTRule out{};
    out.time_seconds = r.time_seconds;
    out.day = r.day;
    out.type = static_cast<time::DSTRuleType>(r.type);
    out.month = r.month;
    out.week = r.week;
    out.day_of_week = r.day_of_week;
    return out;
  };
  tz.dst_start = rule(z.dst_start);
  tz.dst_end = rule(z.dst_end);
  time::set_global_tz(tz);
  bed_minute_ = -1;  // the schedule looked at afresh in the new zone
  ESP_LOGI(TAG, "Time zone %s", settings_.timezone.c_str());
#endif
}

#ifdef SKYSCOPE_TEST_API
namespace {
// In memory a restart leaves as it is (and power on fills with noise): the origin, and a mark and sum to know it by.
struct KeptDivert {
  uint32_t mark;
  char origin[96];
  uint32_t sum;
};
__NOINIT_ATTR KeptDivert kept_divert;
constexpr uint32_t DIVERT_MARK = 0x5C0DE7E5;
uint32_t divert_sum(const KeptDivert &k) {
  uint32_t h = 2166136261u;
  for (char c : k.origin) h = (h ^ (uint8_t) c) * 16777619u;
  return h;
}
}  // namespace

// The touch lock (skyscope.h), kept over a restart the same way: its seconds, from the restart on.
struct KeptTouchLock {
  uint32_t mark, seconds, sum;
};
__NOINIT_ATTR KeptTouchLock kept_touch_lock;
constexpr uint32_t TOUCH_LOCK_MARK = 0x70C4ED11;

uint32_t Skyscope::kept_touch_lock_s_() {
  const KeptTouchLock &k = kept_touch_lock;
  if (k.mark != TOUCH_LOCK_MARK || k.sum != (k.seconds ^ TOUCH_LOCK_MARK)) return 0;
  return std::min<uint32_t>(k.seconds, 24 * 3600);
}
void Skyscope::keep_touch_lock_(uint32_t seconds) {
  kept_touch_lock = {seconds ? TOUCH_LOCK_MARK : 0, seconds, seconds ^ TOUCH_LOCK_MARK};
}

void Skyscope::lock_touch_(uint32_t seconds) {
  seconds = std::min<uint32_t>(seconds, 24 * 3600);
  keep_touch_lock_(seconds);
  const bool was = touch_locked_until_ != 0;
  touch_locked_until_ = seconds ? (millis() + seconds * 1000) | 1 : 0;
  // LVGL's own touch input on or off: every input but the probe's.
  for (lv_indev_t *i = lv_indev_get_next(nullptr); i; i = lv_indev_get_next(i))
    if (i != probe_input_) lv_indev_enable(i, seconds == 0);
  if (seconds && fingers_.active) end_pinch_();
  if (touch_lock_note_) set_shown(touch_lock_note_, seconds != 0);
  if (was != (seconds != 0))
    ESP_LOGI(TAG, seconds ? "A test runs: touches are off for %u s" : "Touches are on again", (unsigned) seconds);
}

void Skyscope::touch_lock_tick_(uint32_t now) {
  if (touch_locked_until_ && (int32_t) (now - touch_locked_until_) >= 0) lock_touch_(0);
}

std::string Skyscope::kept_divert_() {
  const KeptDivert &k = kept_divert;
  if (k.mark != DIVERT_MARK || k.origin[sizeof(k.origin) - 1] != 0 || k.sum != divert_sum(k)) return "";
  return k.origin;
}

void Skyscope::keep_divert_(const std::string &origin) {
  KeptDivert &k = kept_divert;
  std::memset(&k, 0, sizeof(k));
  if (origin.empty() || origin.size() >= sizeof(k.origin)) return;
  std::memcpy(k.origin, origin.data(), origin.size());
  k.mark = DIVERT_MARK;
  k.sum = divert_sum(k);
}
#endif

void Skyscope::setup() {
#ifdef SKYSCOPE_TEST_API
  divert_ = kept_divert_();
  if (!divert_.empty()) {
    // Before the worker asks anything; the map card left out, so no tile of the mock's is ever kept on it.
    ::sky::http::divert_to(divert_);
    card_fitted_ = false;
#ifdef USE_UPDATE
    // The update server too, which ESPHome's own client asks: its releases are the mock's.
    if (firmware_ && firmware_->get_source_url())
      firmware_->set_source_url(::sky::http::diverted(firmware_->get_source_url(), divert_));
#endif
    ESP_LOGW(TAG, "A test: every request goes to %s", divert_.c_str());
  }
#endif
#ifdef USE_OTA_STATE_LISTENER
  ota::get_global_ota_callback()->add_global_state_listener(this);
#endif
  ui::set_dpi(dpi_);
  ui::fonts.small = font_small_ ? font_small_->get_lv_font() : nullptr;
  ui::fonts.label = font_label_ ? font_label_->get_lv_font() : ui::fonts.small;
  ui::fonts.title = font_title_ ? font_title_->get_lv_font() : ui::fonts.label;
  load_settings_();
  load_stats_();
  reshow_();
  use_timezone_();
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.settings = settings_;
    shared_.settings_rev = 1;
  }
  if (base_) {
    // Kept running after the captive portal (which starts and stops the same server) is done.
    base_->init();
    base_->add_handler(this);
  }
  // The networks in reach, for the menu's Wi-Fi list (editor.cpp).
  if (wifi::global_wifi_component) wifi::global_wifi_component->add_scan_results_listener(this);
#ifdef USE_UPDATE
  if (firmware_) firmware_->add_on_state_callback([this]() {
    ++firmware_reports_;
    this->firmware_changed_();
  });
#endif
}

void Skyscope::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Skyscope %s:\n"
                "  Glass: %d x %d at %.1f dpi, tiles x%d\n"
                "  Located: %s, range %d NM, map %s, feed %s",
                version_.c_str(), width_, height_, dpi_, tile_scale_, YESNO(settings_.located), settings_.range_nm,
                MAP_STYLES[settings_.map], FEEDS[settings_.feed]);
}

void Skyscope::loop() {
  const uint32_t now = millis();
  {
    const uint32_t us = (uint32_t) esp_timer_get_time();
    if (loop_last_us_) loop_gap_ms_ = std::max(loop_gap_ms_, (us - loop_last_us_) / 1000.0f);
    loop_last_us_ = us;
    ++loop_passes_;
    if (now - loop_window_start_ >= 5000) {
      const float seconds = (now - loop_window_start_) / 1000.0f;
      loops_per_s_ = loop_passes_ / seconds;
      steps_per_s_ = (ui::sweep_steps - steps_from_) / seconds;
      const uint32_t steps = ui::sweep_steps - steps_from_;
      paint_ms_ = steps ? (ui::paint_us - paint_from_) / 1000.0f / steps : 0;
      paint_from_ = ui::paint_us;
      loop_gap_max_ms_ = loop_gap_ms_;
      loop_passes_ = 0;
      loop_gap_ms_ = 0;
      steps_from_ = ui::sweep_steps;
      loop_window_start_ = now;
    }
  }
  if (!ui_built_) {
    if (!lv_display_get_default()) return;
    build_ui_();
    if (!ui_built_) return;
  }
  const bool online = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
  // A screen with a radio, off a network for a while: the radar from the radio alone (offline.h).
  const bool radio_only = offline_tracker_.update(radio_fitted, online, now);
  bool card_mounted;
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.online = online;
    card_mounted = shared_.card.mounted;
  }
  // Offline, the map card found (or lost) since: the map from it, or the radar.
  if (radio_only != offline_ || (offline_ && card_mounted != offline_card_)) go_offline_(radio_only);
  if (save_pending_ && (int32_t) (now - save_at_) >= 0) save_settings_();
#ifdef SKYSCOPE_TEST_API
  touch_lock_tick_(now);
#endif
  sleep_or_wake_(now);
  // The brightness set, or the one a finger is dragging to; none while the screen is off.
  const float level = (drag_level_ >= 0 ? drag_level_ : settings_.brightness) / 100.0f;
  if (brightness_ && !asleep_ && level != applied_brightness_) {
    brightness_->set_level(level);
    applied_brightness_ = level;
  }
  if (level_until_ && (int32_t) (now - level_until_) >= 0) {
    level_until_ = 0;
    set_shown(level_, false);
  }
  follow_tick_(now);
  update_frame_();
  preview_tick_(now);
  swipe_tick_(now);
  update_planes_(now);
  if (now - last_tick_ >= GLIDE_FRAME_MS) {
    last_tick_ = now;
    view::tick(view_, lv_tick_get());
  }
  update_coprocessor_();
  if (now - last_status_ >= 500) {
    last_status_ = now;
    update_status_(now);
    // A failed check publishes nothing: looked at here too.
    firmware_changed_();
    refresh_menu_firmware_();
    refresh_menu_wifi_();
    refresh_picker_();  // a search answered, a scan given up
    refresh_weather_();
    take_alerts_(now);
    if (now - stats_saved_at_ >= STATS_SAVE_MS) save_stats_();
  }
}

// ---------------------------------------------------------------- the glass

void Skyscope::build_ui_() {
  lv_display_t *display = lv_display_get_default();
  {
    const int w = lv_display_get_horizontal_resolution(display), h = lv_display_get_vertical_resolution(display);
    const bool turned = lvgl_ && (lvgl_->get_rotation() == display::DISPLAY_ROTATION_90_DEGREES ||
                                  lvgl_->get_rotation() == display::DISPLAY_ROTATION_270_DEGREES);
    native_width_ = turned ? h : w;
    native_height_ = turned ? w : h;
  }
  if (lvgl_) lvgl_->set_rotation(settings_.rotation);
  width_ = lv_display_get_horizontal_resolution(display);
  height_ = lv_display_get_vertical_resolution(display);
  ui::shape = {width_, height_, round_};
  const size_t pixels = (size_t) width_ * height_;
  canvas_buffer_ = static_cast<uint16_t *>(heap_caps_calloc(pixels, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  base_frame_ = static_cast<uint16_t *>(heap_caps_calloc(pixels, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!canvas_buffer_ || !base_frame_) {
    ESP_LOGE(TAG, "No PSRAM for two frames of %d x %d", width_, height_);
    mark_failed();
    return;
  }
  lv_obj_t *screen = lv_screen_active();
  // The map's canvas covers the whole screen: a background under it would be filled and covered again in every frame.
  lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  canvas_ = lv_canvas_create(screen);
  lv_canvas_set_buffer(canvas_, canvas_buffer_, width_, height_, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas_, 0, 0);
  lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvas_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (self->long_pressed_) {
      self->long_pressed_ = false;
      return;
    }
    self->on_tap_(e);
  }, LV_EVENT_CLICKED, this);
  // A long press on the map: the settings menu. Only for a finger held still: one that moves is panning the map
  // (preview.cpp), and the touch panel takes two fingers close together for one.
  lv_obj_add_event_cb(canvas_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &self->pressed_at_);
  }, LV_EVENT_PRESSED, this);
  // A swipe along the bottom strip changes the map (swipe.cpp).
  lv_obj_add_event_cb(canvas_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    lv_point_t p{};
    if (lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
    self->on_released_(p);
  }, LV_EVENT_RELEASED, this);
  lv_obj_add_event_cb(canvas_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if ((int32_t) (millis() - self->taps_from_) < 0) return;
    lv_point_t p{};
    if (lv_indev_t *indev = lv_indev_active()) lv_indev_get_point(indev, &p);
    if (std::hypot(p.x - self->pressed_at_.x, p.y - self->pressed_at_.y) > ui::mm(STILL_MM)) return;
    self->long_pressed_ = true;
    self->open_menu_();
  }, LV_EVENT_LONG_PRESSED, this);

  // The credits, along the bottom of the map: made before the planes so the planes and their labels draw over them.
  credit_ = label(screen, ui::fonts.small, colors::MUTED);
  ui::hug_edge(credit_, LV_ALIGN_BOTTOM_LEFT, ui::px(6), -ui::px(4));
  lv_obj_set_style_text_opa(credit_, LV_OPA_80, 0);

  // The planes lie in this, over the map and under everything else.
  planes_ = lv_obj_create(screen);
  lv_obj_remove_style_all(planes_);
  lv_obj_set_size(planes_, width_, height_);
  lv_obj_remove_flag(planes_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(planes_, LV_OBJ_FLAG_SCROLLABLE);

  // The brightness strip down the right edge: a drag up or down sets the brightness; a tap goes to the map.
  dimmer_ = lv_obj_create(screen);
  lv_obj_remove_style_all(dimmer_);
  lv_obj_set_size(dimmer_, ui::mm(DIMMER_MM), height_);
  lv_obj_align(dimmer_, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_flag(dimmer_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(dimmer_, LV_OBJ_FLAG_SCROLLABLE);
  for (const lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED})
    lv_obj_add_event_cb(dimmer_, [](lv_event_t *e) { static_cast<Skyscope *>(lv_event_get_user_data(e))->on_dimmer_(e); },
                        code, this);
  // The level while it changes: a bar filled from the bottom, and the percentage.
  level_ = panel(screen);
  lv_obj_set_size(level_, ui::mm(6), height_ / 2);
  lv_obj_align(level_, LV_ALIGN_RIGHT_MID, -ui::mm(DIMMER_MM) - ui::px(4), 0);
  lv_obj_remove_flag(level_, LV_OBJ_FLAG_CLICKABLE);
  level_fill_ = lv_obj_create(level_);
  lv_obj_remove_style_all(level_fill_);
  lv_obj_set_width(level_fill_, lv_pct(100));
  lv_obj_set_style_bg_color(level_fill_, lv_color_hex(colors::ACCENT), 0);
  lv_obj_set_style_bg_opa(level_fill_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(level_fill_, ui::px(4), 0);
  lv_obj_align(level_fill_, LV_ALIGN_BOTTOM_MID, 0, 0);
  level_text_ = label(screen, ui::fonts.label, colors::INK);
  lv_obj_set_parent(level_text_, level_);
  lv_obj_align(level_text_, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_add_flag(level_, LV_OBJ_FLAG_HIDDEN);

  status_ = label(screen, ui::fonts.small, colors::INK);
  lv_obj_set_style_bg_color(status_, lv_color_hex(colors::CARD), 0);
  lv_obj_set_style_bg_opa(status_, LV_OPA_70, 0);
  lv_obj_set_style_radius(status_, ui::px(6), 0);
  lv_obj_set_style_pad_hor(status_, ui::px(6), 0);
  lv_obj_set_style_pad_ver(status_, ui::px(2), 0);
  ui::hug_edge(status_, LV_ALIGN_TOP_LEFT, ui::px(6), ui::px(6));
  // As wide as its text, at most the glass's width: a long message (an error, a countdown) wraps onto more lines.
  lv_obj_set_style_max_width(status_, glass::page_width(ui::shape, ui::px(6)), 0);
  lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
  lv_obj_remove_flag(status_, LV_OBJ_FLAG_CLICKABLE);

  // The card of the plane picked.
  card_ = panel(screen);
  lv_obj_set_width(card_, std::min(glass::page_width(ui::shape, ui::px(8)), ui::mm(60)));
  lv_obj_set_height(card_, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_, ui::px(2), 0);
  ui::hug_edge(card_, LV_ALIGN_BOTTOM_MID, 0, -ui::px(8));
  lv_obj_set_style_bg_opa(card_, LV_OPA_90, 0);
  card_title_ = label(card_, ui::fonts.title, colors::INK);
  for (size_t i = 0; i < 4; ++i) {
    lv_obj_t *parent = card_;
    if (i == 1) {
      // Altitude, then an arrow drawn for a climb or descent, then its rate, the speed and the heading.
      card_motion_ = widgets::plain(card_);
      lv_obj_set_size(card_motion_, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(card_motion_, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(card_motion_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(card_motion_, ui::px(3), 0);
      lv_obj_remove_flag(card_motion_, LV_OBJ_FLAG_CLICKABLE);
      parent = card_motion_;
    }
    card_rows_[i] = label(parent, ui::fonts.label, colors::MUTED);
    // Which way to look takes a second line where it needs one.
    lv_label_set_long_mode(card_rows_[i], i == 3 ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    if (i != 1) lv_obj_set_width(card_rows_[i], lv_pct(100));
  }
  {
    const int tall = ui::fonts.label ? lv_font_get_line_height(ui::fonts.label) * 3 / 5 : ui::px(10);
    const int stroke = std::max(2, ui::px(1.6f)), wide = tall * 7 / 10, h = stroke / 2;
    card_arrow_ = lv_line_create(card_motion_);
    lv_obj_set_size(card_arrow_, wide + stroke, tall + stroke);
    lv_obj_set_style_line_width(card_arrow_, stroke, 0);
    lv_obj_set_style_line_rounded(card_arrow_, true, 0);
    lv_obj_remove_flag(card_arrow_, LV_OBJ_FLAG_CLICKABLE);
    // A stem and a head, its point up (set again pointing down for a descent): left, point, right, point, foot.
    card_arrow_points_[0] = {h, h + tall * 9 / 20};
    card_arrow_points_[1] = {h + wide / 2, h};
    card_arrow_points_[2] = {h + wide, h + tall * 9 / 20};
    card_arrow_points_[3] = {h + wide / 2, h};
    card_arrow_points_[4] = {h + wide / 2, h + tall};
    lv_line_set_points_mutable(card_arrow_, card_arrow_points_, 5);
    card_arrow_way_ = 1;
    card_after_ = label(card_motion_, ui::fonts.label, colors::MUTED);
    lv_label_set_long_mode(card_after_, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(card_after_, 1);
  }
  // Its photo (photos.h), beside the QR code of the photo's page, with its photographer under them: shown once it has
  // come, as Planespotters.net's terms ask.
  card_photo_ = widgets::plain(card_);
  lv_obj_set_size(card_photo_, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_photo_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(card_photo_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(card_photo_, ui::px(6), 0);
  lv_obj_remove_flag(card_photo_, LV_OBJ_FLAG_CLICKABLE);
  card_photo_image_ = lv_image_create(card_photo_);
  lv_obj_remove_flag(card_photo_image_, LV_OBJ_FLAG_CLICKABLE);
  card_qr_ = lv_qrcode_create(card_photo_);
  lv_qrcode_set_dark_color(card_qr_, lv_color_black());
  lv_qrcode_set_light_color(card_qr_, lv_color_white());
  // The quiet margin a reader needs around the code, white.
  lv_obj_set_style_border_color(card_qr_, lv_color_white(), 0);
  lv_obj_remove_flag(card_qr_, LV_OBJ_FLAG_CLICKABLE);
  card_photo_credit_ = label(card_, ui::fonts.small, colors::MUTED);
  lv_label_set_long_mode(card_photo_credit_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(card_photo_credit_, lv_pct(100));
  lv_obj_add_flag(card_photo_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(card_photo_credit_, LV_OBJ_FLAG_HIDDEN);
  // Follow the plane wherever it flies (follow.h), or stop following it.
  menu_settings_ = settings_;  // the key's colors (menu.cpp button_); restyle_ keeps them after
  card_follow_ = button_(card_, "Follow", LV_SIZE_CONTENT, ui::mm(7), -1, true);
  lv_obj_add_event_cb(card_follow_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->follow_picked_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);

  // While a plane is followed, down the left edge: the whole route in view, the range the screen had before, and back
  // to the plane at its own range.
  follow_keys_ = widgets::plain(screen);
  lv_obj_set_size(follow_keys_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(follow_keys_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(follow_keys_, ui::mm(1.5f), 0);  // with the keys' wider touch, a gap between them
  lv_obj_remove_flag(follow_keys_, LV_OBJ_FLAG_CLICKABLE);
  // Small round keys, each just around a symbol drawn in it (the route, and a sight on the plane), both symbols inside
  // the same circle; a finger's touch is taken a little beyond the key (round_key_).
  const int side = ui::mm(3.45f), across = side * 13 / 10;
  route_key_ = round_key_(follow_keys_, across);
  draw_route_symbol_(route_key_, side);
  own_key_ = round_key_(follow_keys_, across);
  draw_own_symbol_(own_key_, side);
  plane_key_ = round_key_(follow_keys_, across);
  draw_sight_symbol_(plane_key_, side);
  lv_obj_add_event_cb(route_key_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->show_route_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(own_key_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() {
      if (self->following_() && self->settings_.own_range_nm) self->show_plane_(self->settings_.own_range_nm);
    });
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(plane_key_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->show_plane_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_update_layout(follow_keys_);
  // Low on the left, clear of the strip along the bottom whose swipes change the map (swipe.h) and of the credits over
  // it. On round glass the corner is not there: in from the edge as far as the circle needs at that height.
  {
    const int bottom = ui::mm(swipe::STRIP_MM + 2.5f);
    int left = ui::mm(1.5f);
    if (ui::shape.round) {
      const double r = width_ / 2.0, below = height_ - bottom - r;  // the keys' foot below the circle's middle
      left = (int) std::ceil(r - std::sqrt(std::max(0.0, r * r - below * below))) + ui::mm(1);
    }
    lv_obj_align(follow_keys_, LV_ALIGN_BOTTOM_LEFT, left, -bottom);
  }
  lv_obj_add_flag(follow_keys_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(card_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->picked_.clear();
    view::set_picked(self->view_, "");
    self->update_card_();
  }, LV_EVENT_CLICKED, this);

  // An alert along the top, under the status line: a tap picks its plane and shows its card.
  alert_ = panel(screen);
  lv_obj_set_width(alert_, std::min(glass::page_width(ui::shape, ui::px(8)), ui::mm(60)));
  lv_obj_set_height(alert_, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(alert_, LV_OPA_90, 0);
  lv_obj_set_style_border_width(alert_, std::max(2, ui::px(2)), 0);
  ui::hug_edge(alert_, LV_ALIGN_TOP_MID, 0, ui::mm(7));
  alert_text_ = label(alert_, ui::fonts.label, colors::INK);
  lv_label_set_long_mode(alert_text_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(alert_text_, lv_pct(100));
  lv_obj_add_flag(alert_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(alert_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(alert_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    set_shown(self->alert_, false);
    self->alert_until_ = 0;
    if (self->alert_hex_.empty()) return;
    self->picked_ = self->alert_hex_;
    view::set_picked(self->view_, self->picked_);
    self->update_card_();
  }, LV_EVENT_CLICKED, this);

  // What to do before there is anything to show: join a Wi-Fi network, or set the place.
  setup_ = lv_obj_create(screen);
  lv_obj_remove_style_all(setup_);
  lv_obj_set_size(setup_, width_, height_);
  lv_obj_set_style_bg_color(setup_, lv_color_hex(colors::BACKGROUND), 0);
  lv_obj_set_style_bg_opa(setup_, LV_OPA_COVER, 0);
  ui::pad_page(setup_, ui::mm(3));
  lv_obj_set_flex_flow(setup_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(setup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(setup_, ui::mm(2), 0);
  setup_title_ = label(setup_, ui::fonts.title, colors::ACCENT);
  setup_body_ = label(setup_, ui::fonts.label, colors::INK);
  lv_label_set_long_mode(setup_body_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(setup_body_, lv_pct(100));
  // Without a network, the networks in reach at a tap, chosen and typed on the glass as in the menu: the setup network
  // and Improv go on as before, for those who would rather use a phone.
  setup_wifi_ = button_(setup_, "Choose a network here", LV_SIZE_CONTENT, ui::mm(8), -1, true);
  lv_obj_add_event_cb(setup_wifi_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (self->install_.busy() || !self->joining_.empty()) return;
    self->open_menu_();
    self->open_picker_(PICKING_NETWORKS);
  }, LV_EVENT_CLICKED, this);
  // A long press here opens the menu too: the network and the place can be set on the glass.
  lv_obj_add_flag(setup_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(setup_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (!self->install_.busy() && self->joining_.empty()) self->open_menu_();
  }, LV_EVENT_LONG_PRESSED, this);

  ui_built_ = true;
  restyle_();
  ESP_LOGI(TAG, "Glass %d x %d", width_, height_);
  if (panel_te_pin_ >= 0) panel_sync::start(panel_te_pin_);
  view::set_beam_painter(view_, [this](float was, float now, const plan::Box &box) { painter_.paint(was, now, box); });
  lv_display_add_event_cb(display, frame_start, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(display, frame_ready, LV_EVENT_REFR_READY, nullptr);
  lv_display_add_event_cb(display, flush_start, LV_EVENT_FLUSH_START, nullptr);
  lv_display_add_event_cb(display, flush_finish, LV_EVENT_FLUSH_FINISH, nullptr);
#ifdef SKYSCOPE_TEST_API
  make_probe_input_();
  // A note while a test has the fingers locked out, and a lock kept over the restart.
  touch_lock_note_ = label(screen, ui::fonts.small, colors::WARNING);
  lv_label_set_text(touch_lock_note_, "Test running: touch is off");
  lv_obj_set_style_bg_color(touch_lock_note_, lv_color_hex(colors::CARD), 0);
  lv_obj_set_style_bg_opa(touch_lock_note_, LV_OPA_70, 0);
  lv_obj_set_style_pad_hor(touch_lock_note_, ui::px(6), 0);
  lv_obj_set_style_radius(touch_lock_note_, ui::px(6), 0);
  ui::hug_edge(touch_lock_note_, LV_ALIGN_BOTTOM_MID, 0, -ui::mm(swipe::STRIP_MM + 1));
  lv_obj_remove_flag(touch_lock_note_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(touch_lock_note_, LV_OBJ_FLAG_HIDDEN);
  if (const uint32_t s = kept_touch_lock_s_()) lock_touch_(s);
#endif
  replan_();
#ifdef USE_SKYSCOPE_RADIO
  // The radio's tasks, on the worker's core above it, before the worker asks it for planes; the stick is opened
  // whenever one is plugged in.
  radio_fitted = true;
  radio_ = std::make_unique<Radio>();
  radio_->start(radio_config_);
#endif
  // Its stack in internal RAM: a TLS handshake needs some 8 KB of it. Core 0, beside Wi-Fi, away from LVGL.
  if (xTaskCreatePinnedToCore(worker_main, "skyscope", 16384, this, 3, &worker_task_, 0) != pdPASS)
    ESP_LOGE(TAG, "The worker did not start");
}

void Skyscope::on_tap_(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  tap_at_(p);
}

void Skyscope::tap_at_(lv_point_t p) {
  if ((int32_t) (millis() - taps_from_) < 0) return;
  const std::string id = view::pick(view_, p.x, p.y, ui::px(PICK_REACH));
  if (!id.empty()) {
    picked_ = id == picked_ ? "" : id;
    view::set_picked(view_, picked_);
    update_card_();
    return;
  }
  if (!picked_.empty()) {
    picked_.clear();
    view::set_picked(view_, "");
    update_card_();
    return;
  }
  address_until_ = address_until_ ? 0 : millis() + ADDRESS_MS;
  last_status_ = 0;  // at once
}

// ---------------------------------------------------------------- the screen off, and the brightness strip

void Skyscope::sleep_or_wake_(uint32_t now) {
  if (!lvgl_) return;
  if (asleep_) {
    // LVGL woke itself at a touch (lvgl: resume_on_input), or bedtime is over: the screen is on again, and the touch
    // that woke it is no tap.
    if (lvgl_->is_paused()) {
      wake_by_clock_();
      if (lvgl_->is_paused()) return;
    }
    // Woken by a finger while a test has them locked out: dark again.
    if (touch_locked_() && !wake_asked_) {
      lvgl_->set_paused(true, false);
      return;
    }
    wake_asked_ = false;
    asleep_ = false;
    taps_from_ = now + WAKE_DEAF_MS;
    applied_brightness_ = -1;  // the brightness again, at once
    seen_flights_rev_ = AGAIN;
    last_status_ = 0;
    lv_display_trigger_activity(lv_display_get_default());
    std::lock_guard<std::mutex> guard(lock_);
    shared_.asleep = false;
    ESP_LOGI(TAG, "Screen on");
    return;
  }
  if (bedtime_(now)) return;
  if (settings_.sleep_min <= 0) return;
  if (lv_display_get_inactive_time(lv_display_get_default()) < (uint32_t) settings_.sleep_min * 60000) return;
  go_to_sleep_("after " + std::to_string(settings_.sleep_min) + " min without a touch");
}

// Bedtime, once a minute by the clock: dark at bed_off, on again at bed_on where the person asked for it. A screen that
// starts (or learns the time) between the two goes dark at once. True when it just went dark.
bool Skyscope::bedtime_(uint32_t now) {
#ifdef USE_TIME
  if (!clock_ || !settings_.bedtime) return false;
  const ESPTime t = clock_->now();
  if (!t.is_valid()) return false;
  const int minute = t.hour * 60 + t.minute;
  if (minute == bed_minute_) return false;
  const bool first = bed_minute_ < 0;
  bed_minute_ = minute;
  const int off = schedule::minutes(settings_.bed_off), on = schedule::minutes(settings_.bed_on);
  if (off < 0) return false;
  if (minute == off || (first && settings_.bed_wake && on >= 0 && schedule::within(minute, off, on))) {
    go_to_sleep_("bedtime");
    return true;
  }
#endif
  return false;
}

// The scheduled end of bedtime, checked while asleep: the screen on again by itself.
void Skyscope::wake_by_clock_() {
#ifdef USE_TIME
  if (!clock_ || !settings_.bedtime || !settings_.bed_wake) return;
  const ESPTime t = clock_->now();
  if (!t.is_valid()) return;
  const int minute = t.hour * 60 + t.minute;
  if (minute == bed_minute_) return;
  bed_minute_ = minute;
  if (minute != schedule::minutes(settings_.bed_on)) return;
  ESP_LOGI(TAG, "Bedtime over");
  wake_asked_ = true;
  lvgl_->set_paused(false, false);  // sleep_or_wake_ turns the rest on, as at a touch
#endif
}

void Skyscope::go_to_sleep_(const std::string &why) {
  if (asleep_) return;
  if (menu_) close_menu_();
  end_preview_(false);
  finish_swipe_();
  close_weather_();
  // Dark, and nothing drawn: an AMOLED shows black as off. The worker asks for no planes or tiles meanwhile.
  asleep_ = true;
  if (brightness_) brightness_->set_level(0);
  applied_brightness_ = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.asleep = true;
  }
  lvgl_->set_paused(true, false);
  ESP_LOGI(TAG, "Screen off: %s", why.c_str());
}

void Skyscope::show_level_(int percent) {
  lv_obj_set_height(level_fill_, std::max(1, (int) (lv_obj_get_content_height(level_) * percent / 100)));
  set_text(level_text_, std::to_string(percent) + "%");
  set_shown(level_, true);
  level_until_ = millis() + LEVEL_MS;
}

void Skyscope::on_dimmer_(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  if (code == LV_EVENT_PRESSED) {
    drag_y_ = p.y;
    drag_from_ = settings_.brightness;
    dragged_ = false;
    return;
  }
  if (code == LV_EVENT_PRESSING) {
    if (!dragged_ && std::abs(p.y - drag_y_) < ui::px(DRAG_PX)) return;
    if ((int32_t) (millis() - taps_from_) < 0) return;
    dragged_ = true;
    // Up is brighter: DRAG_SPAN of the glass's height from darkest to brightest.
    const float span = std::max(1.0f, height_ * DRAG_SPAN);
    drag_level_ = std::max(DIM_LOWEST, std::min(100, drag_from_ + (int) std::lround((drag_y_ - p.y) * 100.0f / span)));
    show_level_(drag_level_);
    return;
  }
  if (code == LV_EVENT_RELEASED) {
    if (!dragged_) {
      // Not a drag: a tap on the map under the strip.
      tap_at_(p);
      return;
    }
    Settings s = settings_;
    s.brightness = drag_level_;
    drag_level_ = -1;
    use_settings_(s);
    show_level_(s.brightness);
  }
}

void Skyscope::update_frame_() {
  const Settings &s = shown_();
  uint32_t frame_rev, frame_plan_rev, airspace_rev;
  int done, total;
  std::shared_ptr<const airspace::Map> airspace;
  {
    std::lock_guard<std::mutex> guard(lock_);
    frame_rev = shared_.frame_rev;
    frame_plan_rev = shared_.frame_plan_rev;
    done = shared_.tiles_done;
    total = shared_.tiles_total;
    airspace_rev = shared_.airspace_rev;
    if (s.airspace && s.range_nm <= airspace::MOST_RANGE_NM) airspace = shared_.airspace;
  }
  const bool radar = s.map == RADAR;
  const bool new_plan = framed_plan_rev_ != plan_rev_;
  // The airspace came, or was switched on or off: the frame again, with it or without.
  const bool new_airspace =
      s.airspace != drawn_airspace_ || (s.airspace && airspace_rev != drawn_airspace_rev_);
  // The radar draws itself; a map waits for its worker to start on the new plan.
  if (!radar && (frame_plan_rev != plan_rev_ || (frame_rev == seen_frame_rev_ && !new_airspace))) return;
  if (radar && !new_plan && !new_airspace) return;
  const uint32_t now = millis();
  if (!radar && !new_plan && !new_airspace && done < total && now - copied_at_ < FRAME_EVERY_MS) return;
  // A picture two fingers move (preview.cpp) stays while they choose, and until the view's own frame is whole.
  if (frozen_shown_ &&
      (fingers_.active || settle_at_ || (!radar && done < total && now - planned_at_ < WHOLE_FRAME_MS)))
    return;
  if (radar) {
    lv_canvas_fill_bg(canvas_, lv_color_hex(colors::tint(s.tint).ground), LV_OPA_COVER);
  } else {
    std::unique_lock<std::mutex> frame(frame_lock_, std::try_to_lock);
    if (!frame.owns_lock()) return;
    std::memcpy(canvas_buffer_, base_frame_, (size_t) width_ * height_ * 2);
  }
  seen_frame_rev_ = frame_rev;
  copied_at_ = now;
  const scope::Look look{radar, s.tint, s.theme == LIGHT};
  if (airspace) {
    const int64_t from = esp_timer_get_time();
    airspace::draw(canvas_buffer_, plan_.view, *airspace, scope::airspace_ink(look));
    airspace_draw_ms_ = (esp_timer_get_time() - from) / 1000.0f;
  }
  drawn_airspace_ = s.airspace;
  drawn_airspace_rev_ = airspace_rev;
  // The scope on the radar (unless it is switched off), the rings over a map.
  if (radar ? scope_shown(s) : s.rings) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    scope::draw(&layer, plan_.view, look);
    lv_canvas_finish_layer(canvas_, &layer);
  }
  if (sweeps(s)) {
    const size_t bytes = (size_t) width_ * height_ * 2;
    if (!scope_background_)
      scope_background_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scope_background_) std::memcpy(scope_background_, canvas_buffer_, bytes);
    painter_.canvas = canvas_buffer_;
    painter_.background = scope_background_;
    painter_.width = width_;
    painter_.height = height_;
    painter_.cx = width_ / 2.0f;
    painter_.cy = height_ / 2.0f;
    // To the rose, where the beam turns (as view.cpp's scope_reach).
    painter_.reach = scope::rose_radius(plan_.view, width_ / 2.0, height_ / 2.0) + 2;
    painter_.beam_width = std::max(2, ui::px(3));
    painter_.set_tint(colors::tint(s.tint));
  } else if (scope_background_) {
    // No beam to wipe away: the clean copy let go, or closing the menu would put back the scope as it last swept (in
    // the phosphor it had then).
    heap_caps_free(scope_background_);
    scope_background_ = nullptr;
    painter_.background = nullptr;
  }
  lv_obj_invalidate(canvas_);
  unfreeze_();
  if (new_plan) {
    framed_plan_rev_ = plan_rev_;
    view::new_frame(view_);
    seen_flights_rev_ = AGAIN;  // the planes again, on the new frame
    last_status_ = 0;  // the credits again, at once
  }
}

void Skyscope::update_planes_(uint32_t now) {
  const Settings &s = shown_();
  const int64_t started_us = esp_timer_get_time();
  uint32_t rev;
  {
    std::lock_guard<std::mutex> guard(lock_);
    rev = shared_.flights_rev;
    if (rev != tracked_rev_) flights_ = shared_.flights;
  }
  if (rev == seen_flights_rev_ || framed_plan_rev_ != plan_rev_ || !s.located) return;
  const bool new_report = rev != tracked_rev_;
  if (new_report) {
    tracks_.update(flights_, now);
    tracked_rev_ = rev;
  }
  seen_flights_rev_ = rev;
  // The plane followed, out of reach of the reports: where it is likely to be (or was last seen), dimmed.
  flights_.erase(std::remove_if(flights_.begin(), flights_.end(), [](const feed::Flight &f) { return f.estimated; }),
                 flights_.end());
  if (following_() && follower_.phase(now) == follow::LOST &&
      std::none_of(flights_.begin(), flights_.end(), [&](const feed::Flight &f) { return f.hex == follower_.hex(); })) {
    feed::Flight f = follower_.shown(now);
    f.estimated = true;
    flights_.push_back(std::move(f));
  }
  const bool radar = s.map == RADAR;
  plan::Overlay o = overlay::build(flights_, s, plan_.view, tracks_, lv_tick_get(), ui::px_per_mm, radar);
  // The plane followed: its route under everything, and its track from its trail, the whole flight or as many minutes
  // as chosen.
  if (following_() && route_.known) o.route = follow::route_on(route_, plan_.view);
  // Else the plane picked: its route under everything too, while its card shows it.
  else if (!following_() && picked_route_.known && !picked_.empty() && picked_ == route_asked_hex_)
    o.route = follow::route_on(picked_route_, plan_.view);
  if (following_()) o.followed = follower_.hex();
  route_points_ = o.route.size() / 2;
  if (following_())
    for (auto &p : o.planes)
      if (p.id == trail_.hex())
        p.track = follow::track_on(trail_.since(now, s.follow_track_min), plan_.view, TRAIL_POINTS);
  o.light = s.theme == LIGHT && !radar;
  if (radar) {
    o.radar.on = true;
    o.radar.tint = s.tint;
    o.radar.x = width_ / 2;
    o.radar.y = height_ / 2;
    o.radar.rose = scope::rose_radius(plan_.view, width_ / 2.0, height_ / 2.0);
    o.radar.sweep = sweeps(s);
  }
  shown_count_ = o.planes.size();
  view::show(view_, planes_, o);
  view::set_picked(view_, picked_);
  update_card_();
  if (new_report) {
    ui::report_taken = true;
    report_ms_ = (esp_timer_get_time() - started_us) / 1000.0f;
    report_ms_max_ = std::max(report_ms_max_, report_ms_);
  }
}

void Skyscope::update_card_() {
  const feed::Flight *f = nullptr;
  for (const auto &x : flights_) if (x.hex == picked_) f = &x;
  if (picked_.empty() || !f) {
    set_shown(card_, false);
    ask_photo_("");  // no card: its photo goes
    return;
  }
  const bool metric = settings_.metric;
  set_text(card_title_, overlay::name_of(*f) + (f->emergency ? "  SQUAWK " + f->squawk : ""));
  lv_obj_set_style_text_color(card_title_, lv_color_hex(f->emergency ? colors::EMERGENCY : chrome_().ink), 0);
  auto join = [](std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto &p : parts) {
      if (p.empty()) continue;
      if (!out.empty()) out += "  \xC2\xB7  ";
      out += p;
    }
    return out;
  };
  const std::string kind = !f->description.empty() ? f->description
                           : !f->type.empty()        ? f->type
                           : feed::made_up_address(*f) ? std::string("seen by radar, sends no ID")
                                                     : std::string();
  set_text(card_rows_[0], join({kind, f->registration != overlay::name_of(*f) ? f->registration : ""}));
  char heading[16] = "";
  if (f->has_heading) std::snprintf(heading, sizeof(heading), "%03d\xC2\xB0", (int) std::lround(f->heading) % 360);
  // The altitude, an arrow up or down for a climb or descent with its rate after it, the speed and the heading.
  const int way = overlay::climb_way(*f);
  set_text(card_rows_[1], f->on_ground ? std::string("on the ground") : overlay::altitude_text(*f, metric));
  set_shown(card_arrow_, way != 0);
  if (way && way != card_arrow_way_) {
    // Turned over: the point at the other end, the foot where the point was.
    const lv_value_precise_t top = card_arrow_points_[1].y, foot = card_arrow_points_[4].y;
    const lv_value_precise_t shoulder = card_arrow_points_[0].y;
    card_arrow_points_[1].y = card_arrow_points_[3].y = foot;
    card_arrow_points_[4].y = top;
    card_arrow_points_[0].y = card_arrow_points_[2].y = top + foot - shoulder;
    card_arrow_way_ = way;
    lv_line_set_points_mutable(card_arrow_, card_arrow_points_, 5);
  }
  set_text(card_after_, join({overlay::climb_rate(*f, metric), overlay::speed_text(*f, metric), heading}));
  const bool followed = !settings_.follow.empty() && follower_.known() && follower_.hex() == f->hex;
  // Its route, where adsb.lol knows one that fits where it is: its airports.
  if (!followed) ask_picked_route_(*f);
  const follow::Route &r = followed ? route_ : picked_route_;
  const bool asked = f->hex == route_asked_hex_ && f->callsign == route_asked_callsign_;
  const bool route_known = r.known && (followed || asked);
  const std::string route = route_known ? r.from + " to " + r.to : "";
  set_text(card_rows_[2], join({f->operator_name, f->squawk.empty() ? "" : "squawk " + f->squawk, route,
                                f->estimated ? "estimated, out of reach" : ""}));
  // Which way to look for it from the place, while the view is the place's own (not a plane followed far away) and the
  // plane is near enough to be seen.
  const geo::Sight sight = overlay::sight_of(*f, settings_.lat, settings_.lon);
  const bool near_enough =
      settings_.located && !following_() && !f->estimated && sight.meters < SIGHT_MOST_NM * geo::NM;
  set_text(card_rows_[3], near_enough ? overlay::sight_text(sight, metric) : std::string());
  set_shown(card_rows_[3], near_enough);
  // Its photo, where photos are wanted and there is a network to ask.
  ask_photo_(settings_.photos && !offline_ && !f->estimated && !feed::made_up_address(*f) ? f->hex : std::string());
  show_photo_();
  set_text(lv_obj_get_child(card_follow_, 0), followed ? "Stop following" : "Follow");
  // Off a network nothing can be followed: the radio hears only what is near. Nor can a plane known by a made-up
  // address: it names no aircraft to look up.
  set_shown(card_follow_, !offline_ && !feed::made_up_address(*f));
  set_shown(card_, true);
}

// The route of the plane picked, asked of the worker once for each plane and callsign; a new pick drops the last one's.
void Skyscope::ask_picked_route_(const feed::Flight &f) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    // The worker's answer, if it is for the plane asked last (one picked before may answer after another is).
    if (shared_.picked_route_rev != picked_route_rev_) {
      picked_route_rev_ = shared_.picked_route_rev;
      if (shared_.picked_route.callsign == route_asked_callsign_) {
        picked_route_ = shared_.picked_route;
        seen_flights_rev_ = AGAIN;  // its line on the map
      }
    }
    if (f.hex == route_asked_hex_ && f.callsign == route_asked_callsign_) return;
    route_asked_hex_ = f.hex;
    route_asked_callsign_ = f.callsign;
    picked_route_ = follow::Route{};
    if (f.callsign.empty() || f.on_ground) return;
    shared_.route_ask = {f.callsign, f.lat, f.lon, f.has_heading, f.heading};
    shared_.route_ask_rev++;
  }
}

PhotoImage::~PhotoImage() { heap_caps_free(pixels); }

// The photo of the plane `hex` asked of the worker, once a plane; "" lets the one held go, at once, as the terms ask
// that a photo is kept no longer than it is shown.
void Skyscope::ask_photo_(const std::string &hex) {
  if (hex == photo_asked_hex_) return;
  photo_asked_hex_ = hex;
  if (photo_) {
    lv_image_set_src(card_photo_image_, nullptr);
    photo_.reset();
  }
  set_shown(card_photo_, false);
  set_shown(card_photo_credit_, false);
  std::lock_guard<std::mutex> guard(lock_);
  shared_.photo_ask = hex;
  shared_.photo_ask_rev++;
  if (hex.empty() && shared_.photo) {
    shared_.photo.reset();
    shared_.photo_rev++;
  }
}

// The photo the worker brought, if it is the plane's on the card: the picture fitted beside its QR code, which is as
// tall as the picture (at least 12 mm, to be read from arm's length) and gets a white margin of two of its modules.
void Skyscope::show_photo_() {
  std::shared_ptr<const PhotoImage> got;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (shared_.photo_rev == photo_rev_) return;
    photo_rev_ = shared_.photo_rev;
    got = shared_.photo;
  }
  if (!got || got->hex != photo_asked_hex_ || !got->pixels) return;
  photo_ = std::move(got);
  photo_dsc_ = {};
  photo_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
  photo_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
  photo_dsc_.header.w = photo_->width;
  photo_dsc_.header.h = photo_->height;
  photo_dsc_.header.stride = photo_->width * 2;
  photo_dsc_.data = reinterpret_cast<const uint8_t *>(photo_->pixels);
  photo_dsc_.data_size = (uint32_t) photo_->width * photo_->height * 2;
  lv_obj_update_layout(card_);
  const int room = std::max(ui::mm(20), (int) lv_obj_get_content_width(card_));
  const int gap = ui::px(6);
  int qr = std::max(ui::mm(12), photo_->height);
  qr = std::min(qr, room * 2 / 5);
  // The picture as large as it is, or smaller to fit beside the code.
  const int fit = std::max(1, room - qr - gap);
  const int scale = std::min(256, 256 * fit / std::max(1, photo_->width));
  lv_image_set_src(card_photo_image_, &photo_dsc_);
  lv_image_set_scale(card_photo_image_, scale);
  lv_obj_set_size(card_photo_image_, photo_->width * scale / 256, photo_->height * scale / 256);
  lv_image_set_inner_align(card_photo_image_, LV_IMAGE_ALIGN_CENTER);
  const std::string &link = photo_->photo.link;
  const int margin = std::max(2, qr / 20);
  lv_obj_set_style_border_width(card_qr_, margin, 0);
  lv_qrcode_set_size(card_qr_, qr - 2 * margin);
  lv_qrcode_update(card_qr_, link.data(), link.size());
  set_text(card_photo_credit_, photos::credit(photo_->photo));
  set_shown(card_photo_, true);
  set_shown(card_photo_credit_, true);
}

// The alerts the worker raised since the last look: each told to Home Assistant, and the latest shown on the glass.
void Skyscope::take_alerts_(uint32_t now) {
  std::vector<sightings::Alert> alerts;
  {
    std::lock_guard<std::mutex> guard(lock_);
    alerts.swap(shared_.alerts);
    // The picked plane's route may have come meanwhile: its card again.
    if (shared_.picked_route_rev != picked_route_rev_ && !picked_.empty()) seen_flights_rev_ = AGAIN;
    if (shared_.photo_rev != photo_rev_ && !picked_.empty()) seen_flights_rev_ = AGAIN;
  }
  for (const auto &a : alerts) {
    ++alerts_seen_;
    last_alert_ = a.text;
    alert_callbacks_.call(sightings::KINDS[a.kind], a.name, a.text);
  }
  if (!alerts.empty() && !asleep_) {
    // An emergency before a watched plane, when both came at once.
    const auto *shown = &alerts.back();
    for (const auto &a : alerts)
      if (a.kind == sightings::EMERGENCY) shown = &a;
    show_alert_(*shown);
    alert_until_ = now + ALERT_MS;
  }
  if (alert_until_ && (int32_t) (now - alert_until_) >= 0) {
    alert_until_ = 0;
    set_shown(alert_, false);
  }
}

void Skyscope::show_alert_(const sightings::Alert &a) {
  if (!ui_built_) return;
  alert_hex_ = a.hex;
  set_text(alert_text_, a.text);
  const uint32_t edge = a.kind == sightings::EMERGENCY ? colors::EMERGENCY
                        : a.color > 0                  ? colors::WATCH[(a.color - 1) % 6]
                                                       : chrome_().accent;
  lv_obj_set_style_border_color(alert_, lv_color_hex(edge), 0);
  set_shown(alert_, true);
  lv_obj_move_foreground(alert_);
}

void Skyscope::load_stats_() {
  stats_pref_ = global_preferences->make_preference<StoredStats>(STATS_PREF_HASH);
  auto stored = std::make_unique<StoredStats>();
  if (!stats_pref_.load(stored.get()) || stored->size > sizeof(stored->bytes)) return;
  std::lock_guard<std::mutex> guard(lock_);
  if (shared_.book.unpack(stored->bytes, stored->size)) {
    stats_saved_rev_ = shared_.book_rev;
    ESP_LOGI(TAG, "The day's tally kept: %u planes", (unsigned) shared_.book.today().planes);
  }
}

void Skyscope::save_stats_() {
  stats_saved_at_ = millis();
  std::vector<uint8_t> bytes;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (shared_.book_rev == stats_saved_rev_) return;
    stats_saved_rev_ = shared_.book_rev;
    bytes = shared_.book.pack();
  }
  auto stored = std::make_unique<StoredStats>();
  if (bytes.size() > sizeof(stored->bytes)) {
    ESP_LOGW(TAG, "The day's tally is too large to keep (%u bytes)", (unsigned) bytes.size());
    return;
  }
  stored->size = bytes.size();
  std::memcpy(stored->bytes, bytes.data(), bytes.size());
  stats_pref_.save(stored.get());
  global_preferences->sync();
}

// ---- For Home Assistant (home-assistant.yaml), on the loop.
uint32_t Skyscope::planes_in_range() {
  std::lock_guard<std::mutex> guard(lock_);
  return shared_.book.in_range();
}
uint32_t Skyscope::planes_today() {
  std::lock_guard<std::mutex> guard(lock_);
  return shared_.book.today().planes;
}
namespace {
// The nearest plane to the place in the last report, or nullptr: none while a plane far away is followed.
const feed::Flight *nearest_of(const std::vector<feed::Flight> &flights, const Settings &s, bool following) {
  if (following || !s.located) return nullptr;
  const feed::Flight *best = nullptr;
  double best_m = std::min(s.range_nm, FEED_REACH_NM) * geo::NM;
  for (const auto &f : flights) {
    if (f.estimated || !sightings::counts(f)) continue;
    const double m = geo::meters_between(s.lat, s.lon, f.lat, f.lon);
    if (m <= best_m) {
      best = &f;
      best_m = m;
    }
  }
  return best;
}
}  // namespace
std::string Skyscope::nearest_plane() {
  const feed::Flight *f = nearest_of(flights_, settings_, following_());
  return f ? overlay::name_of(*f) : "";
}
float Skyscope::nearest_nm() {
  const feed::Flight *f = nearest_of(flights_, settings_, following_());
  return f ? (float) (geo::meters_between(settings_.lat, settings_.lon, f->lat, f->lon) / geo::NM) : NAN;
}
float Skyscope::nearest_altitude_ft() {
  const feed::Flight *f = nearest_of(flights_, settings_, following_());
  return f && f->has_altitude && !f->on_ground ? (float) f->altitude : NAN;
}
std::string Skyscope::followed_state() {
  if (settings_.follow.empty()) return "";
  if (follower_for_ != settings_.follow) return "looking";
  return follow::PHASES[follower_.phase(millis())];
}
void Skyscope::set_screen_on(bool on) {
  if (on == !asleep_ || !lvgl_) return;
  // On as at a touch: sleep_or_wake_ turns the rest on, and the screen-off timer starts again.
  if (on) {
    wake_asked_ = true;
    lvgl_->set_paused(false, false);
  }
  else go_to_sleep_("Home Assistant");
}
void Skyscope::set_brightness(int percent) {
  Settings s = settings_;
  s.brightness = std::max(0, std::min(100, percent));
  if (s.brightness != settings_.brightness) use_settings_(s);
}

// A Wi-Fi chip (an ESP-Hosted coprocessor) running other firmware than the one this firmware carries is brought in line
// at once: a mismatched one may give no Wi-Fi at all, so this cannot wait for a press on the page. The screen says so
// first, and the install (which holds the loop until it is done, then restarts) starts once that is on the glass. A
// failed one is not tried again until the next start.
void Skyscope::update_coprocessor_() {
#ifdef USE_UPDATE
  if (!coprocessor_ || coprocessor_tried_ || coprocessor_->state != update::UPDATE_STATE_AVAILABLE) return;
  coprocessor_tried_ = coprocessor_updating_ = true;
  ESP_LOGW(TAG, "Wi-Fi chip runs %s, this firmware carries %s: installing it",
           coprocessor_->update_info.current_version.c_str(), coprocessor_->update_info.latest_version.c_str());
  update_status_(millis());
  set_timeout("coprocessor", 1500, [this]() {
    coprocessor_->perform();
    // Done, the restart is on its way and the words stay up; failed, the screen goes on with the chip as it is.
    if (coprocessor_->state == update::UPDATE_STATE_NO_UPDATE) return;
    coprocessor_updating_ = false;
    ESP_LOGE(TAG, "The Wi-Fi chip's update failed");
  });
#endif
}

void Skyscope::update_status_(uint32_t now) {
  std::string title, body;
  const bool online = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
  bool portal = false;
#ifdef USE_CAPTIVE_PORTAL
  portal = captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#endif
  if (coprocessor_updating_) {
    title = "Updating the Wi-Fi chip";
    body = "Its firmware is being brought up to date with the screen's. The screen restarts by itself when it is done, "
           "in a minute or so.";
  } else if (install_.busy()) {
    title = "Updating";
    body = "New firmware is being installed. The screen restarts by itself when it is done.";
  } else if (!joining_.empty()) {
    title = "Joining Wi-Fi";
    body = "The screen restarts to join \"" + joining_ + "\". If it cannot, it opens its own network \"" +
           (setup_network_.empty() ? App.get_name() : setup_network_) + "\" again after a minute.";
  } else if (portal && !offline_) {
    title = "Set up Wi-Fi";
    body = "Choose your network below and type its password here. Or, on a phone or computer, join the Wi-Fi "
           "network \"" + (setup_network_.empty() ? App.get_name() : setup_network_) +
           "\": a page opens to choose it (or go to http://192.168.4.1).";
  } else if (!online && !offline_) {
    title = "Connecting";
    // The network ESPHome is trying: the one saved by the setup page, Improv or the menu.
    std::string name;
    if (wifi::global_wifi_component && wifi::global_wifi_component->has_sta()) {
      const StringRef ssid = wifi::global_wifi_component->get_sta().get_ssid();
      name.assign(ssid.c_str(), ssid.size());
    }
    body = (name.empty() ? std::string("Joining the Wi-Fi network") : "Joining \"" + name + "\"") +
           "... Or choose another below.";
    if (radio_fitted) body += " Without a network, the radar shows the planes the radio hears.";
  } else if (!settings_.located && !following_()) {
    // On a network with no place yet, the menu opens by itself, once: its first rows are the place, and its top the
    // address for a browser. Canceled, this page says what to do instead.
    if (online && !offline_ && !asked_where_ && !menu_ && !install_.busy()) {
      asked_where_ = true;
      this->defer([this]() { open_menu_(); });
    }
    title = "Where are you?";
    if (offline_)
      body = "With no network, the place cannot be looked up: hold a finger here and type its latitude and longitude "
             "in the menu.";
    else
      body = "Hold a finger here to find the place to watch, or open http://" + own_address() +
             "/ in a browser on the same network.";
  }
  set_shown(setup_, !title.empty());
  // While it asks for a network or looks for one: not while it restarts into one or updates.
  set_shown(setup_wifi_, !offline_ && !online && !coprocessor_updating_ && !install_.busy() && joining_.empty());
  if (!title.empty()) {
    set_text(setup_title_, title);
    set_text(setup_body_, body);
    return;
  }
  std::string status, tile_error, airspace_error;
  providers::Providers sources;
  Feed source = ADSB_FI;
  bool simulated = false;
  uint32_t feed_at;
  int done, total;
  uint32_t radar_scan = 0, airspace_retry_at = 0;
  int missing = 0;
  bool downloading = false;
  size_t download_next = 0, download_total = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    missing = shared_.card.missing;
    downloading = shared_.card.downloading;
    download_next = shared_.card.next;
    download_total = shared_.card.total_tiles;
    sources = shared_.providers;
    source = shared_.feed_source;
    simulated = shared_.feed_simulated;
    tile_error = shared_.tile_error;
    airspace_error = shared_.airspace_error;
    airspace_retry_at = shared_.airspace_retry_at;
    feed_at = shared_.feed_at;
    done = shared_.tiles_done;
    total = shared_.tiles_total;
    radar_scan = shared_.radar_scan;
  }
  const uint32_t ink = chrome_().ink;
  uint32_t color = ink;
  if (address_until_ && (int32_t) (now - address_until_) < 0) {
    const std::string address = own_address();
    set_text(status_, (address.empty() ? std::string("No network") : "http://" + address + "/") + "  \xC2\xB7  " +
                          std::to_string(settings_.range_nm) + " NM");
    set_status_color_(chrome_().ink);
    return;
  }
  address_until_ = 0;
  // The credits: the map's, and the exchange the planes come from.
  const Settings &shown = shown_();
  std::string credit = tiles::credit(shown);
  if (shown.airspace && credit.find("FAA") == std::string::npos)
    credit += std::string(credit.empty() ? "" : " \xC2\xB7 ") + "FAA";
  if (feed_at) credit += std::string(credit.empty() ? "" : " \xC2\xB7 ") + feed::credit(source, simulated);
  // The radar's scan, at its time (a loop's goes by), and its source as IEM asks.
  if (shown.radar_overlay && radar_scan)
    credit += std::string(credit.empty() ? "" : " \xC2\xB7 ") + "Radar " + radar::clock(radar_scan) + " NEXRAD/IEM";
  set_text(credit_, credit);
  const Feed preferred = shown.feed;
  const providers::State &first = sources.state(preferred);
  if (sources.order().size == 0) {
    // The receiver chosen with no address, or the radio on a screen without one, and no other source allowed: a choice
    // still to make, not a fault, so in the ordinary color. A screen with a radio points to it: its USB stick is a
    // receiver too, and the likelier one meant.
    if (preferred == RADIO) status = "This screen has no radio: choose another source";
    else if (radio_fitted) status = "No receiver address: set one, or choose USB radio for the stick";
    else status = "No receiver address: set it on the web page";
  } else if (sources.all_blocked(now) && (feed_at == 0 || now - feed_at > 60000)) {
    // Every exchange it may ask is waiting: the one free soonest, and when.
    Feed next = preferred;
    for (const Feed f : sources.order()) {
      if (!settings_.fallback) break;
      if (providers::before(sources.state(f).until, sources.state(next).until)) next = f;
    }
    const providers::State &s = sources.state(next);
    status = std::string(feed::credit(next)) + ": " + s.error + ", again in " + feed::countdown(s.until, now);
    color = colors::ERROR;
  } else if (feed_at == 0) {
    status = "Finding aircraft...";
  } else {
    // The place first, as it was found ("KJFK"), where it was found by a search.
    // Following a plane: how it is going ("Following BAW286", "out of reach 42 min, estimated") in place of the place.
    std::string where = settings_.place;
    if (!settings_.follow.empty() && !offline_) {
      where = follower_for_ == settings_.follow ? follow::status(follower_, settings_.follow, now)
                                                : "Looking for " + settings_.follow;
    }
    status = (where.empty() ? std::string() : where + "  \xC2\xB7  ") + std::to_string(shown_count_) + " aircraft";
    // While the preferred exchange waits, how long it still waits. Where the planes come from is in the credits.
    if (first.blocked && providers::before(now, first.until))
      status += std::string("  \xC2\xB7  ") + feed::credit(preferred) + " again in " + feed::countdown(first.until, now);
    // How long since the planes last came in, always: in the warning color once they are well out of date.
    status += "  \xC2\xB7  " + std::to_string((now - feed_at) / 1000) + " s ago";
    if (now - feed_at > 3 * 1000 * (uint32_t) settings_.refresh_s + 10000) color = colors::WARNING;
  }
  // Off a network, the planes are the radio's alone, and said so.
  if (offline_) status += "  \xC2\xB7  no Wi-Fi";
  if (shown.map != RADAR) {
    if (plan_.tile_z < 0) {
      status += "  \xC2\xB7  no map at this range";
    } else if (!tile_error.empty()) {
      status += "  \xC2\xB7  map: " + tile_error;
      color = color == ink ? colors::WARNING : color;
    } else if (done < total) {
      status += "  \xC2\xB7  map " + std::to_string(done) + "/" + std::to_string(total);
    } else if (missing) {
      // Off a network, the squares the card does not have are left empty.
      status += "  \xC2\xB7  map: " + std::to_string(missing) + " not on the card";
      color = color == ink ? colors::WARNING : color;
    }
  }
  if (downloading)
    status += "  \xC2\xB7  card " + std::to_string(download_next) + "/" + std::to_string(download_total);
  if (shown.airspace && !airspace_error.empty()) {
    status += "  \xC2\xB7  airspace: " + airspace_error;
    const int32_t left_ms = (int32_t) (airspace_retry_at - now);
    if (airspace_retry_at && left_ms > 0) status += ", again in " + std::to_string((left_ms + 999) / 1000) + " s";
    color = color == ink ? colors::WARNING : color;
  }
  set_text(status_, status);
  set_status_color_(color);
}
}  // namespace esphome::skyscope

// ---------------------------------------------------------------- following a plane (follow.h)

namespace esphome::skyscope {
void Skyscope::follow_tick_(uint32_t now) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (shared_.follower_rev != follower_rev_) {
      follower_rev_ = shared_.follower_rev;
      follower_ = shared_.follower;
      follower_for_ = shared_.follower_for;
      route_ = shared_.route;
      seen_flights_rev_ = AGAIN;  // its card and its estimate drawn again
      // Its trail: begun afresh for another plane, from the track the screen already has of it, and on with each
      // report of it (not its estimates).
      if (follower_.known()) {
        if (trail_.hex() != follower_.hex()) {
          trail_.clear(follower_.hex());
          for (const auto &p : tracks_.of(follower_.hex(), overlay::MOST_TRACK_S, now)) trail_.add(p.lat, p.lon, p.at);
        }
        if (follower_.phase(now) == follow::LIVE) trail_.add(follower_.last().lat, follower_.last().lon, now);
      }
    }
  }
  const bool asked = !settings_.follow.empty() && follower_for_ == settings_.follow && !offline_;
  const follow::Phase phase = asked ? follower_.phase(now) : follow::LOOKING;
  const bool asking = !asleep_ && wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
  if (!asking) follow_asking_since_ = 0;
  else if (!follow_asking_since_) follow_asking_since_ = now | 1;
  const bool settled = follow_asking_since_ && now - follow_asking_since_ >= 2 * follow::HEX_EVERY_MS;
  // Landed, or lost too long: following stops by itself, and the view goes back to the place. Not before the screen
  // has been awake on its network long enough to have asked for the plane again.
  if (asked && settled && (phase == follow::LANDED || phase == follow::GAVE_UP)) {
    ESP_LOGI(TAG, "%s: following stops", follow::status(follower_, settings_.follow, now).c_str());
    Settings s = settings_;
    s.follow.clear();
    s.follow_hex.clear();
    use_settings_(s);
    return;
  }
  // Found: its address kept with the settings, so a restart over an ocean still knows which plane it is.
  if (asked && follower_.known() && settings_.follow_hex != follower_.hex()) {
    Settings s = settings_;
    s.follow_hex = follower_.hex();
    use_settings_(s);
  }
  if (!asked || !follower_.known()) {
    // Stopped: the view back to the place, at the range it had before following changed it.
    if (center_.on || (settings_.follow.empty() && settings_.own_range_nm)) set_center_(follow::Center{});
    if (settings_.follow.empty()) {
      if (trail_.size()) trail_.clear();
      ranged_hex_.clear();
    }
    drop_ahead_();
    route_view_ = false;
    refresh_follow_keys_();
    return;
  }
  refresh_follow_keys_();
  // The view a preview's fingers chose stays while they choose it (preview.cpp).
  if (previewing_ || width_ <= 0) return;
  // A plane followed from now (just found, another one, or again after a restart): at its own range, the map moving
  // about once a minute with it. Back from offline it keeps the range it had.
  if (ranged_hex_ != follower_.hex()) {
    ranged_hex_ = follower_.hex();
    show_plane_();
    return;
  }
  const geo::Place p = follower_.where(now);
  // With the whole route in view the view stays until the plane would leave it; else it steps with the plane.
  const double keep = route_view_ ? 0.95 : follow::KEEP;
  if (center_.on && center_.hex == follower_.hex() && !follow::off_center(plan_.view, p.lat, p.lon, keep)) {
    plan_ahead_(now, p);
    return;
  }
  route_view_ = false;
  const feed::Flight &f = follower_.last();
  geo::Place c = follow::center_for(plan_.view, p.lat, p.lon, f.has_heading, f.heading);
  // The view drawn ahead, where the plane is about where it would put it: taken as it is, so its frame is the one
  // the worker has ready.
  const bool took_ahead = ahead_.on && ahead_.hex == follower_.hex() && ahead_view_.zoom == plan_.view.zoom &&
                          f.has_heading && follow::suits(ahead_view_, p.lat, p.lon, f.heading);
  if (took_ahead) c = {ahead_.lat, ahead_.lon};
  ESP_LOGD(TAG, "Following: a step, %s", took_ahead ? "to the view drawn ahead" : "drawn now");
  ahead_at_ = 0;  // the step after it planned at once
  follow::Center next;
  next.on = true;
  next.lat = c.lat;
  next.lon = c.lon;
  next.hex = follower_.hex();
  set_center_(next);
}

// The next step's view, for the worker to draw while the plane flies there: where the plane, flying on as it is, goes
// past KEEP, and the middle a step would take there. Kept while it still suits that place (a plane that holds its way
// keeps the same one), looked at again each second, and none where the next view would show another map (a chart of
// the United States at its border) or the map is the radar.
void Skyscope::plan_ahead_(uint32_t now, const geo::Place &plane) {
  if (ahead_at_ && now - ahead_at_ < 1000) return;
  ahead_at_ = now | 1;
  const feed::Flight &f = follower_.last();
  geo::Place at;
  const bool can = following_() && !route_view_ && !offline_ && !plan_.keys.empty() && f.has_heading &&
                   follow::crossing(plan_.view, plane.lat, plane.lon, f.heading, at);
  if (!can) {
    drop_ahead_();
    return;
  }
  if (ahead_.on && ahead_.hex == center_.hex && ahead_view_.zoom == plan_.view.zoom &&
      follow::suits(ahead_view_, at.lat, at.lon, f.heading))
    return;
  const geo::Place mid = follow::center_for(plan_.view, at.lat, at.lon, true, f.heading);
  follow::Center c = center_;
  c.lat = mid.lat;
  c.lon = mid.lon;
  const Settings shown = follow::shown(settings_, c);
  if (shown.map != shown_().map) {
    drop_ahead_();
    return;
  }
  ahead_ = c;
  const tiles::Plan p = plan_for_(shown);
  ahead_view_ = p.view;
  std::lock_guard<std::mutex> guard(lock_);
  shared_.ahead = p;
  shared_.ahead_rev++;
}

void Skyscope::drop_ahead_() {
  if (!ahead_.on) return;
  ahead_ = follow::Center{};
  std::lock_guard<std::mutex> guard(lock_);
  shared_.ahead = tiles::Plan{};
  shared_.ahead_rev++;
}

void Skyscope::set_center_(const follow::Center &c, int range_nm) {
  // The range following first changes is kept, to go back to (the Own range key, and once following stops).
  Settings s = settings_;
  if (range_nm > 0) {
    if (c.on && !s.own_range_nm) s.own_range_nm = s.range_nm;
    s.range_nm = range_nm;
  }
  if (!c.on && s.follow.empty() && s.own_range_nm) {
    s.range_nm = s.own_range_nm;
    s.own_range_nm = 0;
  }
  const bool new_range = s.range_nm != settings_.range_nm || s.own_range_nm != settings_.own_range_nm;
  if (c == center_ && !new_range) return;
  // A step of the map along with its plane: the glass as it was, moved to the new view, stays until the new frame is
  // whole, so the map moves in one step instead of through the bare ground while its tiles come.
  const bool step = center_.on && c.on && ui_built_ && !menu_ && !frozen_shown_ && !asleep_ && shown_().map != RADAR;
  if (step) target_view_ = plan_.view;
  const bool frozen = step && freeze_();
  center_ = c;
  if (new_range) {
    // The range is a setting (kept, as a pinch's Save keeps it): the frame is planned with the middle at once.
    {
      std::lock_guard<std::mutex> guard(lock_);
      shared_.center = center_;
      shared_.center_rev++;
    }
    use_settings_(s);
  } else {
    reshow_();
    std::lock_guard<std::mutex> guard(lock_);
    shared_.center = center_;
    shared_.center_rev++;
    replan_locked_();
  }
  restyle_();  // a chart of the United States may have given way to a map of the world, in its own colors
  seen_flights_rev_ = AGAIN;
  if (frozen) {
    target_view_ = plan_.view;
    planned_at_ = millis();
    frozen_dirty_ = true;
  }
  if (c.on) ESP_LOGD(TAG, "Following: the view's middle at %.3f, %.3f", c.lat, c.lon);
  else ESP_LOGI(TAG, "Following: back to the place");
}

lv_obj_t *Skyscope::round_key_(lv_obj_t *parent, int across) {
  lv_obj_t *b = widgets::plain(parent);
  lv_obj_set_size(b, across, across);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  // Smaller than a fingertip: its touch reaches a little beyond it, to about a finger's width.
  lv_obj_set_ext_click_area(b, std::max(0, ui::mm(7.5f) - across) / 2);
  return b;
}

// A box for a symbol, centerd in its key, its parts drawn in it; none of them takes a finger from the key.
static lv_obj_t *symbol_box(lv_obj_t *key, int side) {
  lv_obj_t *box = widgets::plain(key);
  lv_obj_set_size(box, side, side);
  lv_obj_center(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  return box;
}
static lv_obj_t *ring(lv_obj_t *box, float cx, float cy, float r, int stroke, bool filled) {
  lv_obj_t *o = widgets::plain(box);
  const int d = (int) std::lround(2 * r);
  lv_obj_set_size(o, d, d);
  lv_obj_set_pos(o, (int) std::lround(cx - r), (int) std::lround(cy - r));
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, filled ? 0 : stroke, 0);
  lv_obj_set_style_bg_opa(o, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
static lv_obj_t *stroke_line(lv_obj_t *box, lv_point_precise_t *points, int count, int stroke) {
  lv_obj_t *line = lv_line_create(box);
  lv_line_set_points_mutable(line, points, count);
  lv_obj_set_size(line, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(line, 0, 0);
  lv_obj_set_style_line_width(line, stroke, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  return line;
}

// The stroke of a key's symbol, in proportion to it.
static int symbol_stroke(int side) { return std::max(2, (int) std::lround(side / 13.0)); }

// The route: where it starts, a ring low on the left; where it ends, a ring high on the right; and the leg between,
// out from the first, around a turn high on the left, and on to the second. All of it inside the circle the sight
// fills, so the two keys' symbols are the same size.
void Skyscope::draw_route_symbol_(lv_obj_t *key, int side) {
  lv_obj_t *box = symbol_box(key, side);
  const int stroke = symbol_stroke(side);
  const float c = side / 2.0f, r = side * 0.14f;
  // The rings' and the turn's middles on the circle's diagonals, as far out as leaves the rings inside it.
  const float k = (c - r - stroke / 2.0f) / std::sqrt(2.0f);
  const float ax = c - k, ay = c + k, bx = c + k, by = c - k, turn_x = c - k, turn_y = c - k;
  ring(box, ax, ay, r, stroke, false);
  ring(box, bx, by, r, stroke, false);
  // The leg from each ring's edge, a little clear of it.
  const float gap = r + stroke;
  const float ux = turn_x - ax, uy = turn_y - ay, ul = std::hypot(ux, uy);
  const float vx = bx - turn_x, vy = by - turn_y, vl = std::hypot(vx, vy);
  route_leg_[0] = {ax + ux / ul * gap, ay + uy / ul * gap};
  route_leg_[1] = {turn_x, turn_y};
  route_leg_[2] = {bx - vx / vl * gap, by - vy / vl * gap};
  stroke_line(box, route_leg_, 3, stroke);
}

// A sight: a ring, four ticks pointing in at it from outside, and a dot in the middle.
void Skyscope::draw_sight_symbol_(lv_obj_t *key, int side) {
  lv_obj_t *box = symbol_box(key, side);
  const int stroke = symbol_stroke(side);
  const float c = side / 2.0f, r = side * 0.3f, inner = r + stroke * 1.5f, outer = c - stroke / 2.0f;
  ring(box, c, c, r, stroke, false);
  ring(box, c, c, std::max(1.5f, side * 0.08f), stroke, true);
  const float dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
  for (int i = 0; i < 4; ++i) {
    sight_ticks_[i][0] = {c + dx[i] * inner, c + dy[i] * inner};
    sight_ticks_[i][1] = {c + dx[i] * outer, c + dy[i] * outer};
    stroke_line(box, sight_ticks_[i], 2, stroke);
  }
}

// The range of one's own: a magnifier (the zoom) with a roof in its glass (home).
void Skyscope::draw_own_symbol_(lv_obj_t *key, int side) {
  lv_obj_t *box = symbol_box(key, side);
  const int stroke = symbol_stroke(side);
  // The glass high on the left, its handle out to the bottom right corner, all of it inside the sight's circle.
  const float r = side * 0.27f, cx = side * 0.42f, cy = side * 0.42f;
  ring(box, cx, cy, r, stroke, false);
  const float k = 0.7071f, from = r + stroke / 2.0f, to = side * 0.57f;
  own_handle_[0] = {cx + k * from, cy + k * from};
  own_handle_[1] = {cx + k * to, cy + k * to};
  stroke_line(box, own_handle_, 2, stroke);
  // The roof: a chevron in the glass, a little below its middle.
  const float w = r * 0.55f, rise = r * 0.45f, base = cy + rise * 0.45f;
  own_roof_[0] = {cx - w, base};
  own_roof_[1] = {cx, base - rise};
  own_roof_[2] = {cx + w, base};
  stroke_line(box, own_roof_, 3, std::max(1, stroke * 3 / 4));
}

void Skyscope::refresh_follow_keys_() {
  if (!follow_keys_) return;
  // Not over a plane's card, which lies along the bottom too.
  const bool shown = following_() && !menu_ && !asleep_ && lv_obj_has_flag(card_, LV_OBJ_FLAG_HIDDEN);
  set_shown(follow_keys_, shown);
  if (!shown) return;
  set_shown(route_key_, route_.known);
  set_shown(own_key_, settings_.own_range_nm > 0 && settings_.own_range_nm != settings_.range_nm);
}

void Skyscope::show_route_() {
  if (!following_() || !route_.known || width_ <= 0) return;
  // The route's great circle and the plane, all of them on the glass.
  std::vector<geo::Place> places = follow::route_places(route_, 24);
  places.push_back(follower_.where(millis()));
  const follow::Fit f = follow::fit(places, width_, height_, round_);
  follow::Center c = center_;
  c.lat = f.center.lat;
  c.lon = f.center.lon;
  route_view_ = true;
  ESP_LOGI(TAG, "Following: the route %s to %s, %d NM", route_.from.c_str(), route_.to.c_str(), f.range_nm);
  set_center_(c, f.range_nm);
}

void Skyscope::show_plane_(int range_nm) {
  if (!follower_.known() || width_ <= 0) return;
  // Around it at its own range (follow.h plane_range_nm) or the one given, the plane a little behind the middle as the
  // view steps with it.
  const geo::Place p = follower_.where(millis());
  const feed::Flight &f = follower_.last();
  if (range_nm <= 0) range_nm = follow::plane_range_nm(f, p.lat, width_, height_, round_);
  geo::View v = follow::view_for(p.lat, p.lon, range_nm, width_, height_);
  v.round = round_;
  const geo::Place at = follow::center_for(v, p.lat, p.lon, f.has_heading, f.heading);
  follow::Center c;
  c.on = true;
  c.lat = at.lat;
  c.lon = at.lon;
  c.hex = follower_.hex();
  route_view_ = false;
  ESP_LOGI(TAG, "Following: %d NM around the plane at %.0f kt", range_nm, f.has_speed ? f.speed : 0.0f);
  set_center_(c, range_nm);
}

void Skyscope::follow_picked_() {
  const feed::Flight *f = nullptr;
  for (const auto &x : flights_) if (x.hex == picked_) f = &x;
  if (!f) return;
  Settings s = settings_;
  if (!settings_.follow.empty() && follower_.known() && follower_.hex() == f->hex) {
    s.follow.clear();
    s.follow_hex.clear();
    ESP_LOGI(TAG, "Following stopped");
  } else {
    // By its registration, else its callsign, else its address: what the status line and the menu show.
    JsonDocument doc;
    doc["follow"] = !f->registration.empty() ? f->registration : !f->callsign.empty() ? f->callsign : f->hex;
    doc["follow_hex"] = f->hex;
    std::string error;
    if (!apply(doc.as<JsonObjectConst>(), s, error)) return;
    ESP_LOGI(TAG, "Following %s (%s)", s.follow.c_str(), f->hex.c_str());
  }
  use_settings_(s);
  update_card_();
}
}  // namespace esphome::skyscope
