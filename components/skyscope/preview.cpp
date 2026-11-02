// Two fingers on the map: spread or pinched they zoom, moved together they pan (pinch.h), as a preview of another view
// that the saved settings wait behind. One finger dragged across the map pans too: the touch panel takes two fingers
// close together (as for a pan) for one, and goes from one to two and back as they move, so the gesture follows one
// finger or two, and carries on without a jump when their count changes.
//
// When the fingers start, what the glass shows (the map, its planes and their labels) is taken as one picture, and
// while they move that picture is drawn into the map's canvas larger, smaller or shifted as the new view would show it;
// the live planes are hidden and nothing is downloaded. Once the fingers have been off the glass for SETTLE_MS the
// view is planned, once, and its tiles and planes asked for as for any other place and range; a second gesture before
// then costs nothing more. The picture stays until the new view's frame is whole (skyscope.cpp update_frame_), and
// the live planes come back with it. A bar along the bottom edge offers Cancel and Save for PREVIEW_MS, and drops the
// view when it runs out. Save keeps the place and range (the place's name too, where the middle hardly moved); the
// settings are stored only then.
//
// LVGL is told of the first finger only (ESPHome's touch listener), so it would read the first finger of a pinch as a
// tap, a long press or a drag along the brightness strip: from the moment a gesture starts until all fingers are up,
// the map's taps are not taken. A drag pans only where it starts on the map itself, not on the brightness strip, a
// plane's card, the bar or the strip along the bottom (whose swipes change the map, swipe.cpp); and only past
// PAN_MM, so a tap that wavers is still a tap.
#include "skyscope.h"

#include "colours.h"
#include "swipe.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.preview";
using namespace widgets;

namespace {
constexpr uint32_t SETTLE_MS = 3000;     // fingers off the glass this long: the view previewed is planned
constexpr uint32_t PREVIEW_MS = 60000;   // the bar's countdown, from the last gesture: then the view goes back
constexpr uint32_t AFTER_PINCH_MS = 400;  // taps not taken after the fingers lift, as LVGL sees them lift later
constexpr uint32_t DRAW_EVERY_MS = 40;    // the picture drawn again at most this often while the fingers move
constexpr float PAN_MM = 3;               // one finger moved this far on the map: a pan, no tap
}  // namespace

#ifdef USE_TOUCHSCREEN
void Skyscope::update(const touchscreen::TouchPoints_t &points) {
  if (points.empty()) return;
  const uint32_t now = millis();
  // The panel's own coordinates turned the way LVGL turns the first finger's.
  auto turned = [this](const touchscreen::TouchPoint &tp) {
    int32_t x = tp.x, y = tp.y;
    if (lvgl_) lvgl_->rotate_coordinates(x, y);
    return pinch::Point{(double) x, (double) y};
  };
  const size_t count = std::min<size_t>(points.size(), 2);
  const pinch::Point a = turned(points[0]), b = count > 1 ? turned(points[1]) : a;
  if (!touch_down_) {
    // A new touch: on the map itself, or on what lies over it (the strip, a card, the bar, the menu).
    touch_down_ = true;
    gestured_ = false;
    touch_from_ = a;
    lv_point_t p{(int32_t) a.x, (int32_t) a.y};
    touch_on_map_ = lv_indev_search_obj(lv_screen_active(), &p) == canvas_;
    // The bottom strip is for swiping between maps (swipe.cpp): a drag there is no pan.
    touch_in_strip_ = swipe::in_strip(a.y, height_, ui::mm(swipe::STRIP_MM));
  }
  if (gestured_ || count > 1) {
    gestured_ = true;
    taps_from_ = now + AFTER_PINCH_MS;
  }
  if (!can_pinch_()) return;
  if (!fingers_.active) {
    const bool dragged = std::hypot(a.x - touch_from_.x, a.y - touch_from_.y) >= ui::mm(PAN_MM);
    if (count < 2 && !(touch_on_map_ && !touch_in_strip_ && dragged)) return;
    gestured_ = true;
    taps_from_ = now + AFTER_PINCH_MS;
    begin_preview_();
    if (!freeze_()) return;
    // A drag from where the finger came down, so the map keeps up with it from the first millimetre.
    if (count < 2) fingers_.start(touch_from_, touch_from_, target_view_);
    else fingers_.start(a, b, target_view_);
    finger_count_ = count;
    settle_at_ = 0;
  } else if (count != finger_count_) {
    // A finger more or fewer (or two the panel now takes for one): on from the view as it is, with no jump.
    fingers_.start(a, b, target_view_);
    finger_count_ = count;
  }
  target_view_ = fingers_.moved(a, b);
  frozen_dirty_ = true;
}

void Skyscope::release() {
  if (fingers_.active) end_pinch_();
  if (gestured_) taps_from_ = millis() + AFTER_PINCH_MS;
  touch_down_ = gestured_ = false;
}
#endif

bool Skyscope::can_pinch_() const {
  return ui_built_ && !menu_ && !asleep_ && settings_.located && !install_.updating && joining_.empty() &&
         lv_obj_has_flag(setup_, LV_OBJ_FLAG_HIDDEN);
}

void Skyscope::begin_preview_() {
  // The first finger's drag along the brightness strip, if it was one, is no drag.
  if (drag_level_ >= 0) {
    drag_level_ = -1;
    lv_obj_add_flag(level_, LV_OBJ_FLAG_HIDDEN);
  }
  dragged_ = false;
  if (previewing_) return;
  previewing_ = true;
  held_ = settings_;
  target_view_ = plan_.view;
  ESP_LOGD(TAG, "Previewing");
}

bool Skyscope::freeze_() {
  const size_t bytes = (size_t) width_ * height_ * 2;
  if (!frozen_) frozen_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frozen_) {
    ESP_LOGW(TAG, "No PSRAM for the picture");
    return false;
  }
  // The map and its planes as the glass shows them, without what lies over them (the status line, the credits, a
  // plane's card, the brightness level, the bar), which stays where it is as the picture moves.
  lv_obj_t *over[] = {status_, credit_, card_, level_, preview_bar_};
  bool shown[5];
  for (size_t i = 0; i < 5; ++i) {
    shown[i] = over[i] && !lv_obj_has_flag(over[i], LV_OBJ_FLAG_HIDDEN);
    if (shown[i]) lv_obj_add_flag(over[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_draw_buf_t buf;
  const bool taken =
      lv_draw_buf_init(&buf, width_, height_, LV_COLOR_FORMAT_RGB565, width_ * 2, frozen_, bytes) == LV_RESULT_OK &&
      lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &buf) == LV_RESULT_OK;
  for (size_t i = 0; i < 5; ++i)
    if (shown[i]) lv_obj_remove_flag(over[i], LV_OBJ_FLAG_HIDDEN);
  // Without it, the map alone.
  if (!taken) std::memcpy(frozen_, canvas_buffer_, bytes);
  frozen_view_ = target_view_;
  frozen_shown_ = true;
  view::set_hidden(view_, true);
  return true;
}

void Skyscope::draw_frozen_() {
  frozen_dirty_ = false;
  drawn_frozen_at_ = millis();
  if (!frozen_ || !frozen_shown_ || !canvas_buffer_) return;
  // The ground where the picture does not reach: the radar's, or the screen's own colour.
  const uint32_t ground = settings_.map == RADAR ? colours::tint(settings_.tint).ground : chrome_().card;
  pinch::stretch(frozen_, canvas_buffer_, width_, height_, pinch::transform(frozen_view_, target_view_),
                 lv_color_to_u16(lv_color_hex(ground)), frozen_columns_);
  lv_obj_invalidate(canvas_);
}

void Skyscope::end_pinch_() {
  fingers_.active = false;
  frozen_dirty_ = true;
  const uint32_t now = millis();
  settle_at_ = (now + SETTLE_MS) | 1;
  preview_until_ = (now + PREVIEW_MS) | 1;
  if (preview_bar_ || !previewing_) return;
  // The bar: Cancel, the range and the time left, and Save; along the bottom, so most of the map shows.
  menu_settings_ = settings_;  // the keys' colours (menu.cpp button_)
  const Chrome c = chrome_();
  preview_bar_ = plain(lv_screen_active());
  lv_obj_set_size(preview_bar_, glass::page_width(ui::shape, ui::mm(1)), LV_SIZE_CONTENT);
  ui::hug_edge(preview_bar_, LV_ALIGN_BOTTOM_MID, 0, -ui::mm(1));
  lv_obj_set_style_bg_color(preview_bar_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(preview_bar_, LV_OPA_90, 0);
  lv_obj_set_style_radius(preview_bar_, ui::mm(1.6f), 0);
  lv_obj_set_style_pad_all(preview_bar_, ui::mm(0.8f), 0);
  lv_obj_set_flex_flow(preview_bar_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(preview_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(preview_bar_, ui::mm(1), 0);
  lv_obj_add_flag(preview_bar_, LV_OBJ_FLAG_CLICKABLE);  // a tap beside its keys is no tap on the map
  lv_obj_t *cancel = button_(preview_bar_, "Cancel", LV_SIZE_CONTENT, ui::mm(8));
  lv_obj_add_event_cb(cancel, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->end_preview_(false); });
  }, LV_EVENT_CLICKED, this);
  preview_text_ = text(preview_bar_, ui::fonts.label, c.ink, "");
  lv_obj_set_flex_grow(preview_text_, 1);
  lv_obj_set_style_text_align(preview_text_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(preview_text_, LV_LABEL_LONG_DOT);
  lv_obj_t *save = button_(preview_bar_, "Save", LV_SIZE_CONTENT, ui::mm(8), -1, true);
  lv_obj_add_event_cb(save, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->end_preview_(true); });
  }, LV_EVENT_CLICKED, this);
}

void Skyscope::settle_preview_() {
  settle_at_ = 0;
  Settings s = settings_;
  s.located = true;
  s.lat = target_view_.lat;
  s.lon = target_view_.lon;
  s.range_nm = pinch::range_nm(target_view_);
  // Hardly moved (a zoom whose fingers drifted): still the place by its name, at its own spot. Else no longer it.
  if (geo::metres_between(held_.lat, held_.lon, s.lat, s.lon) <= 0.02 * s.range_nm * geo::NM) {
    s.lat = held_.lat;
    s.lon = held_.lon;
    s.place = held_.place;
  } else {
    s.place.clear();
  }
  use_view_(s);
  ESP_LOGD(TAG, "Previewing %d NM", s.range_nm);
}

void Skyscope::use_view_(const Settings &s) {
  const uint32_t before = plan_rev_;
  use_settings_(s);
  planned_at_ = millis();
  target_view_ = plan_.view;  // as planned: a map's zoom is a whole one
  frozen_dirty_ = true;
  // The same view as the frame's: drawn again from it, as nothing new comes.
  if (plan_rev_ == before) framed_plan_rev_ = 0;
}

void Skyscope::end_preview_(bool keep) {
  if (!previewing_) return;
  if (keep && settle_at_) settle_preview_();  // the view the fingers left, if it was not planned yet
  settle_at_ = preview_until_ = 0;
  fingers_.active = false;
  previewing_ = false;
  if (preview_bar_) lv_obj_delete(preview_bar_);
  preview_bar_ = preview_text_ = nullptr;
  if (keep) {
    // Stored now: until Save the stored settings kept the view as it was (save_settings_).
    save_pending_ = true;
    save_at_ = millis();
    ESP_LOGI(TAG, "Kept %d NM", settings_.range_nm);
    return;
  }
  Settings s = settings_;
  s.located = held_.located;
  s.lat = held_.lat;
  s.lon = held_.lon;
  s.range_nm = held_.range_nm;
  s.place = held_.place;
  use_view_(s);
  ESP_LOGD(TAG, "Dropped");
}

void Skyscope::preview_tick_(uint32_t now) {
  if (frozen_dirty_ && now - drawn_frozen_at_ >= DRAW_EVERY_MS) draw_frozen_();
  if (!previewing_ || fingers_.active) return;
  if (settle_at_ && (int32_t) (now - settle_at_) >= 0) settle_preview_();
  if (preview_until_ && (int32_t) (now - preview_until_) >= 0) {
    end_preview_(false);
    return;
  }
  if (preview_text_) {
    const int left = preview_until_ ? std::max<int>(0, ((int32_t) (preview_until_ - now) + 999) / 1000) : 0;
    char text[48];
    std::snprintf(text, sizeof(text), "%d NM. Keep it? %d s", pinch::range_nm(target_view_), left);
    set_label(preview_text_, text);
  }
}

void Skyscope::unfreeze_() {
  if (!frozen_shown_) return;
  frozen_shown_ = false;
  frozen_dirty_ = false;
  if (!menu_) view::set_hidden(view_, false);
  if (planes_) lv_obj_invalidate(planes_);
}
}  // namespace esphome::skyscope
