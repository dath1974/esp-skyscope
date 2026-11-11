// A swipe left or right along the strip at the bottom of the map changes the map (swipe.h): to the left the next one,
// to the right the one before, in the menu's order. Each swipe shows the map's name at once, large in the middle, and a
// row of dots (one a map, the one chosen lit) above the strip; nothing is downloaded yet. The map is planned, and its
// tiles asked for, only once the swipes have stopped for SETTLE_MS, so going through several costs only the last;
// and it is saved then, as a step of the menu's Map row is by Save (a swipe back is the undo).
//
// The swipe is read from LVGL's own press and release on the map (one finger, as LVGL sees it), so the test API's
// drags make one too.
#include "skyscope.h"

#include "colors.h"
#include "swipe.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::skyscope {
static const char *const TAG = "skyscope.swipe";
using namespace widgets;

namespace {
constexpr uint32_t SETTLE_MS = 2000;     // no swipe for this long: the map chosen is planned and saved
constexpr uint32_t NO_TAP_MS = 300;      // the click LVGL sends after the swipe's release is no tap
}  // namespace

void Skyscope::on_released_(lv_point_t at) {
  // Down from the strip along the top: the day's tally (todaypage.cpp).
  if (swipe::in_top_strip(pressed_at_.y, ui::mm(swipe::STRIP_MM))) {
    if (swipe::downward(pressed_at_.x, pressed_at_.y, at.x, at.y, ui::mm(swipe::SWIPE_MM)) && can_pinch_() &&
        !previewing_) {
      taps_from_ = millis() + NO_TAP_MS;
      open_today_();
    }
    return;
  }
  if (!swipe::in_strip(pressed_at_.y, height_, ui::mm(swipe::STRIP_MM))) return;
  // Up from the strip: the weather page (wxpage.cpp).
  if (swipe::upward(pressed_at_.x, pressed_at_.y, at.x, at.y, ui::mm(swipe::SWIPE_MM)) && can_pinch_() && !previewing_) {
    taps_from_ = millis() + NO_TAP_MS;
    open_weather_();
    return;
  }
  const int step = swipe::direction(pressed_at_.x, pressed_at_.y, at.x, at.y, ui::mm(swipe::SWIPE_MM));
  // Off a network with no map card there is only the radar: no map to swipe to (offline.h).
  if (!step || !can_pinch_() || previewing_ || (offline_ && !offline_card_)) return;
  taps_from_ = millis() + NO_TAP_MS;
  if (!swiping_) {
    swiped_ = settings_;
    swiping_ = true;
  }
  menu::stepped(swipe::map_row(), swiped_, step);
  swipe_at_ = (millis() + SETTLE_MS) | 1;
  show_swipe_();
}

void Skyscope::show_swipe_() {
  const Chrome c = chrome_();
  if (!swipe_card_) {
    // The name, large, in the middle.
    swipe_card_ = plain(lv_screen_active());
    lv_obj_set_size(swipe_card_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(swipe_card_, lv_color_hex(c.card), 0);
    lv_obj_set_style_bg_opa(swipe_card_, LV_OPA_90, 0);
    lv_obj_set_style_radius(swipe_card_, ui::mm(1.6f), 0);
    lv_obj_set_style_pad_hor(swipe_card_, ui::mm(3), 0);
    lv_obj_set_style_pad_ver(swipe_card_, ui::mm(1.5f), 0);
    lv_obj_remove_flag(swipe_card_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(swipe_card_);
    swipe_name_ = text(swipe_card_, ui::fonts.title, c.ink, "");
    // The dots, just above the strip.
    swipe_dots_ = plain(lv_screen_active());
    lv_obj_set_size(swipe_dots_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(swipe_dots_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(swipe_dots_, ui::mm(1.2f), 0);
    lv_obj_set_style_bg_color(swipe_dots_, lv_color_hex(c.card), 0);
    lv_obj_set_style_bg_opa(swipe_dots_, LV_OPA_70, 0);
    lv_obj_set_style_radius(swipe_dots_, ui::mm(1.5f), 0);
    lv_obj_set_style_pad_all(swipe_dots_, ui::mm(0.8f), 0);
    lv_obj_remove_flag(swipe_dots_, LV_OBJ_FLAG_CLICKABLE);
    ui::hug_edge(swipe_dots_, LV_ALIGN_BOTTOM_MID, 0, -ui::mm(swipe::STRIP_MM));
  }
  set_label(swipe_name_, menu::shown(swipe::map_row(), swiped_));
  lv_obj_clean(swipe_dots_);
  const int side = ui::mm(1.6f);
  for (MapStyle m : swipe::maps(swiped_)) {
    lv_obj_t *dot = plain(swipe_dots_);
    lv_obj_set_size(dot, side, side);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(m == swiped_.map ? c.accent : c.muted), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }
  lv_obj_update_layout(swipe_card_);
  lv_obj_center(swipe_card_);
}

void Skyscope::swipe_tick_(uint32_t now) {
  if (swiping_ && (int32_t) (now - swipe_at_) >= 0) finish_swipe_();
}

void Skyscope::finish_swipe_() {
  if (swipe_card_) lv_obj_delete(swipe_card_);
  if (swipe_dots_) lv_obj_delete(swipe_dots_);
  swipe_card_ = swipe_name_ = swipe_dots_ = nullptr;
  if (!swiping_) return;
  swiping_ = false;
  if (swiped_.map == settings_.map) return;
  Settings s = settings_;
  s.map = swiped_.map;
  ESP_LOGI(TAG, "Map: %s", MAP_STYLES[s.map]);
  use_settings_(s);
}
}  // namespace esphome::skyscope
