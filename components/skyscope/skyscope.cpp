// The loop's side of Skyscope: the settings, what the glass shows and what a finger does.
#include "skyscope.h"

#include "colours.h"
#include "panel_sync.h"
#include "scope.h"
#include "ui.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

#include <esp_heap_caps.h>
#include <esp_timer.h>

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
// A tap on the map shows the screen's address and range in the status line for this long.
constexpr uint32_t ADDRESS_MS = 6000;
// A finger picks the plane nearest it within this many reference pixels.
constexpr int PICK_REACH = 36;
// A part of the map comes at most this often while its tiles arrive; the last one at once.
constexpr uint32_t FRAME_EVERY_MS = 600;
// Planes that glide move this often, each in a small part of the glass.
constexpr uint32_t GLIDE_FRAME_MS = 200;

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
  lv_obj_set_style_bg_color(o, lv_color_hex(colours::CARD), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
  lv_obj_set_style_radius(o, ui::px(8), 0);
  lv_obj_set_style_pad_all(o, ui::px(6), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour) {
  lv_obj_t *l = lv_label_create(parent);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
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
  to_json(settings_, doc.to<JsonObject>());
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
                             s.filter != settings_.filter || s.dim != settings_.dim || s.tile_scale != settings_.tile_scale ||
                             s.rings != settings_.rings || s.tint != settings_.tint || s.located != settings_.located;
  if (s.lat != settings_.lat || s.lon != settings_.lon) tracks_.clear();
  settings_ = s;
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.settings = settings_;
    shared_.settings_rev++;
  }
  if (frame_changed) replan_();
  restyle_();
  // The planes again, in the new style.
  seen_flights_rev_ = 0;
  save_pending_ = true;
  save_at_ = millis() + SAVE_DELAY_MS;
}

Skyscope::Chrome Skyscope::chrome_() const {
  if (settings_.map != RADAR) return {colours::INK, colours::MUTED, colours::CARD};
  const colours::Tint &t = colours::tint(settings_.tint);
  return {t.glow, colours::glow_share(t, 0.6f), t.ground};
}

void Skyscope::set_status_colour_(uint32_t colour) {
  // Set only when it changes: LVGL draws a label again whenever a style is set, the same value or not.
  if (colour == status_colour_) return;
  status_colour_ = colour;
  lv_obj_set_style_text_color(status_, lv_color_hex(colour), 0);
}

void Skyscope::restyle_() {
  if (!ui_built_) return;
  const Chrome c = chrome_();
  lv_obj_set_style_bg_color(status_, lv_color_hex(c.card), 0);
  status_colour_ = 0xFFFFFFFF;
  lv_obj_set_style_text_color(credit_, lv_color_hex(c.muted), 0);
  lv_obj_set_style_bg_color(card_, lv_color_hex(c.card), 0);
  lv_obj_set_style_border_color(card_, lv_color_hex(c.muted), 0);
  lv_obj_set_style_border_width(card_, settings_.map == RADAR ? std::max(1, ui::px(1)) : 0, 0);
  lv_obj_set_style_radius(card_, settings_.map == RADAR ? 0 : ui::px(8), 0);
  for (auto *row : card_rows_) lv_obj_set_style_text_color(row, lv_color_hex(c.muted), 0);
  last_status_ = 0;
  update_card_();
}

void Skyscope::replan_() {
  if (width_ <= 0) return;
  const int scale = settings_.tile_scale ? settings_.tile_scale : tile_scale_;
  plan_ = tiles::plan(settings_, width_, height_, scale);
  std::lock_guard<std::mutex> guard(lock_);
  shared_.plan = plan_;
  plan_rev_ = ++shared_.plan_rev;
}

// ---------------------------------------------------------------- component

void Skyscope::setup() {
  ui::set_dpi(dpi_);
  ui::fonts.small = font_small_ ? font_small_->get_lv_font() : nullptr;
  ui::fonts.label = font_label_ ? font_label_->get_lv_font() : ui::fonts.small;
  ui::fonts.title = font_title_ ? font_title_->get_lv_font() : ui::fonts.label;
  load_settings_();
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
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.online = online;
  }
  if (save_pending_ && (int32_t) (now - save_at_) >= 0) save_settings_();
  const float level = settings_.brightness / 100.0f;
  if (brightness_ && level != applied_brightness_) {
    brightness_->set_level(level);
    applied_brightness_ = level;
  }
  update_frame_();
  update_planes_(now);
  if (now - last_tick_ >= GLIDE_FRAME_MS) {
    last_tick_ = now;
    view::tick(view_, lv_tick_get());
  }
  if (now - last_status_ >= 500) {
    last_status_ = now;
    update_status_(now);
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
  if (lvgl_ && settings_.rotation >= 0) lvgl_->set_rotation(settings_.rotation);
  width_ = lv_display_get_horizontal_resolution(display);
  height_ = lv_display_get_vertical_resolution(display);
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
    static_cast<Skyscope *>(lv_event_get_user_data(e))->on_tap_(e);
  }, LV_EVENT_CLICKED, this);

  // The planes lie in this, over the map and under everything else.
  planes_ = lv_obj_create(screen);
  lv_obj_remove_style_all(planes_);
  lv_obj_set_size(planes_, width_, height_);
  lv_obj_remove_flag(planes_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(planes_, LV_OBJ_FLAG_SCROLLABLE);

  status_ = label(screen, ui::fonts.small, colours::INK);
  lv_obj_set_style_bg_color(status_, lv_color_hex(colours::CARD), 0);
  lv_obj_set_style_bg_opa(status_, LV_OPA_70, 0);
  lv_obj_set_style_radius(status_, ui::px(6), 0);
  lv_obj_set_style_pad_hor(status_, ui::px(6), 0);
  lv_obj_set_style_pad_ver(status_, ui::px(2), 0);
  lv_obj_align(status_, LV_ALIGN_TOP_LEFT, ui::px(6), ui::px(6));
  lv_obj_remove_flag(status_, LV_OBJ_FLAG_CLICKABLE);

  credit_ = label(screen, ui::fonts.small, colours::MUTED);
  lv_obj_align(credit_, LV_ALIGN_BOTTOM_LEFT, ui::px(6), -ui::px(4));
  lv_obj_set_style_text_opa(credit_, LV_OPA_80, 0);

  // The card of the plane picked.
  card_ = panel(screen);
  lv_obj_set_width(card_, std::min(width_ - 2 * ui::px(8), ui::mm(60)));
  lv_obj_set_height(card_, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(card_, ui::px(2), 0);
  lv_obj_align(card_, LV_ALIGN_BOTTOM_MID, 0, -ui::px(8));
  lv_obj_set_style_bg_opa(card_, LV_OPA_90, 0);
  card_title_ = label(card_, ui::fonts.title, colours::INK);
  for (auto &row : card_rows_) {
    row = label(card_, ui::fonts.label, colours::MUTED);
    lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row, lv_pct(100));
  }
  lv_obj_add_flag(card_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(card_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->picked_.clear();
    view::set_picked(self->view_, "");
    self->update_card_();
  }, LV_EVENT_CLICKED, this);

  // What to do before there is anything to show: join a Wi-Fi network, or set the place.
  setup_ = lv_obj_create(screen);
  lv_obj_remove_style_all(setup_);
  lv_obj_set_size(setup_, width_, height_);
  lv_obj_set_style_bg_color(setup_, lv_color_hex(colours::BACKGROUND), 0);
  lv_obj_set_style_bg_opa(setup_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(setup_, ui::mm(3), 0);
  lv_obj_set_flex_flow(setup_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(setup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(setup_, ui::mm(2), 0);
  setup_title_ = label(setup_, ui::fonts.title, colours::ACCENT);
  setup_body_ = label(setup_, ui::fonts.label, colours::INK);
  lv_label_set_long_mode(setup_body_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(setup_body_, lv_pct(100));

  ui_built_ = true;
  restyle_();
  ESP_LOGI(TAG, "Glass %d x %d", width_, height_);
  if (panel_te_pin_ >= 0) panel_sync::start(panel_te_pin_);
  view::set_beam_painter(view_, [this](float was, float now, const plan::Box &box) { painter_.paint(was, now, box); });
  lv_display_add_event_cb(display, frame_start, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(display, frame_ready, LV_EVENT_REFR_READY, nullptr);
  lv_display_add_event_cb(display, flush_start, LV_EVENT_FLUSH_START, nullptr);
  lv_display_add_event_cb(display, flush_finish, LV_EVENT_FLUSH_FINISH, nullptr);
  replan_();
  // Its stack in internal RAM: a TLS handshake needs some 8 KB of it. Core 0, beside Wi-Fi, away from LVGL.
  if (xTaskCreatePinnedToCore(worker_main, "skyscope", 16384, this, 3, &worker_task_, 0) != pdPASS)
    ESP_LOGE(TAG, "The worker did not start");
}

void Skyscope::on_tap_(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
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

void Skyscope::update_frame_() {
  uint32_t frame_rev, frame_plan_rev;
  int done, total;
  {
    std::lock_guard<std::mutex> guard(lock_);
    frame_rev = shared_.frame_rev;
    frame_plan_rev = shared_.frame_plan_rev;
    done = shared_.tiles_done;
    total = shared_.tiles_total;
  }
  const bool radar = settings_.map == RADAR;
  const bool new_plan = framed_plan_rev_ != plan_rev_;
  // The radar draws itself; a map waits for its worker to start on the new plan.
  if (!radar && (frame_plan_rev != plan_rev_ || frame_rev == seen_frame_rev_)) return;
  if (radar && !new_plan) return;
  const uint32_t now = millis();
  if (!radar && !new_plan && done < total && now - copied_at_ < FRAME_EVERY_MS) return;
  if (radar) {
    lv_canvas_fill_bg(canvas_, lv_color_hex(colours::tint(settings_.tint).ground), LV_OPA_COVER);
  } else {
    std::unique_lock<std::mutex> frame(frame_lock_, std::try_to_lock);
    if (!frame.owns_lock()) return;
    std::memcpy(canvas_buffer_, base_frame_, (size_t) width_ * height_ * 2);
  }
  seen_frame_rev_ = frame_rev;
  copied_at_ = now;
  if (settings_.rings || radar) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);
    scope::draw(&layer, plan_.view, {radar, settings_.tint});
    lv_canvas_finish_layer(canvas_, &layer);
  }
  if (radar && settings_.sweep) {
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
    painter_.set_tint(colours::tint(settings_.tint));
  }
  lv_obj_invalidate(canvas_);
  if (new_plan) {
    framed_plan_rev_ = plan_rev_;
    view::new_frame(view_);
    seen_flights_rev_ = 0;  // the planes again, on the new frame
    std::string credit = tiles::source(settings_.map).credit;
    std::lock_guard<std::mutex> guard(lock_);
    if (!shared_.flights.empty() || shared_.feed_at) credit += std::string(credit.empty() ? "" : " \xC2\xB7 ") + feed::credit(settings_.feed);
    set_text(credit_, credit);
  }
}

void Skyscope::update_planes_(uint32_t now) {
  const int64_t started_us = esp_timer_get_time();
  uint32_t rev;
  {
    std::lock_guard<std::mutex> guard(lock_);
    rev = shared_.flights_rev;
    if (rev != tracked_rev_) flights_ = shared_.flights;
  }
  if (rev == seen_flights_rev_ || framed_plan_rev_ != plan_rev_ || !settings_.located) return;
  const bool new_report = rev != tracked_rev_;
  if (new_report) {
    tracks_.update(flights_, now);
    tracked_rev_ = rev;
  }
  seen_flights_rev_ = rev;
  const bool radar = settings_.map == RADAR;
  plan::Overlay o = overlay::build(flights_, settings_, plan_.view, tracks_, lv_tick_get(), ui::px_per_mm, radar);
  if (radar) {
    o.radar.on = true;
    o.radar.tint = settings_.tint;
    o.radar.x = width_ / 2;
    o.radar.y = height_ / 2;
    o.radar.rose = scope::rose_radius(plan_.view, width_ / 2.0, height_ / 2.0);
    o.radar.sweep = settings_.sweep;
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
    return;
  }
  const bool metric = settings_.metric;
  set_text(card_title_, overlay::name_of(*f) + (f->emergency ? "  SQUAWK " + f->squawk : ""));
  lv_obj_set_style_text_color(card_title_, lv_color_hex(f->emergency ? colours::EMERGENCY : chrome_().ink), 0);
  auto join = [](std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto &p : parts) {
      if (p.empty()) continue;
      if (!out.empty()) out += "  \xC2\xB7  ";
      out += p;
    }
    return out;
  };
  const std::string kind = !f->description.empty() ? f->description : f->type;
  set_text(card_rows_[0], join({kind, f->registration != overlay::name_of(*f) ? f->registration : ""}));
  std::string climb;
  if (f->has_climb && std::abs(f->climb) >= 200) climb = f->climb > 0 ? "climbing" : "descending";
  char heading[16] = "";
  if (f->has_heading) std::snprintf(heading, sizeof(heading), "%03d\xC2\xB0", (int) std::lround(f->heading) % 360);
  set_text(card_rows_[1], join({f->on_ground ? std::string("on the ground") : overlay::altitude_text(*f, metric), climb,
                                overlay::speed_text(*f, metric), heading}));
  set_text(card_rows_[2], join({f->operator_name, f->squawk.empty() ? "" : "squawk " + f->squawk}));
  set_shown(card_, true);
}

void Skyscope::update_status_(uint32_t now) {
  std::string title, body;
  const bool online = wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
  bool portal = false;
#ifdef USE_CAPTIVE_PORTAL
  portal = captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#endif
  if (portal) {
    title = "Set up Wi-Fi";
    body = "On a phone or computer, join the Wi-Fi network \"" + (setup_network_.empty() ? App.get_name() : setup_network_) +
           "\". A page opens to choose your network (or go to http://192.168.4.1).";
  } else if (!online) {
    title = "Connecting";
    body = "Joining the Wi-Fi network...";
  } else if (!settings_.located) {
    title = "Where are you?";
    body = "Open http://" + own_address() + "/ in a browser on the same network and set the place to watch.";
  }
  set_shown(setup_, !title.empty());
  if (!title.empty()) {
    set_text(setup_title_, title);
    set_text(setup_body_, body);
    return;
  }
  std::string status, feed_error, tile_error;
  uint32_t feed_at;
  int done, total;
  {
    std::lock_guard<std::mutex> guard(lock_);
    feed_error = shared_.feed_error;
    tile_error = shared_.tile_error;
    feed_at = shared_.feed_at;
    done = shared_.tiles_done;
    total = shared_.tiles_total;
  }
  const uint32_t ink = chrome_().ink;
  uint32_t colour = ink;
  if (address_until_ && (int32_t) (now - address_until_) < 0) {
    set_text(status_, "http://" + own_address() + "/  \xC2\xB7  " + std::to_string(settings_.range_nm) + " NM");
    set_status_colour_(chrome_().ink);
    return;
  }
  address_until_ = 0;
  if (!feed_error.empty() && (feed_at == 0 || now - feed_at > 60000)) {
    status = std::string(feed::credit(settings_.feed)) + ": " + feed_error;
    colour = colours::ERROR;
  } else if (feed_at == 0) {
    status = "Finding aircraft...";
  } else {
    status = std::to_string(shown_count_) + " aircraft";
    if (now - feed_at > 3 * 1000 * (uint32_t) settings_.refresh_s + 10000) {
      status += " (" + std::to_string((now - feed_at) / 1000) + " s old)";
      colour = colours::WARNING;
    }
  }
  if (settings_.map != RADAR) {
    if (plan_.tile_z < 0) {
      status += "  \xC2\xB7  no map at this range";
    } else if (!tile_error.empty()) {
      status += "  \xC2\xB7  map: " + tile_error;
      colour = colour == ink ? colours::WARNING : colour;
    } else if (done < total) {
      status += "  \xC2\xB7  map " + std::to_string(done) + "/" + std::to_string(total);
    }
  }
  set_text(status_, status);
  set_status_colour_(colour);
}
}  // namespace esphome::skyscope
