// The tour on the glass (tour.h): a card over the map that asks for one touch at a time, says what the touch did, and
// goes on. It begins by itself the first time a new screen shows its map (a network joined, a place set), and again
// whenever the menu's About group or the web page asks for it; settings_.tour says it is still to be shown.
//
// The card is small and sits high on the glass, clear of the edges the touches start from, and it takes no touch but
// on its two keys: a finger on its words reaches the map under it. While a page covers the glass (the menu, the
// weather, the day's tally) the card is hidden, and comes back when the page is closed. It is built again for every
// step, and whenever the screen's colors change (a swipe to the radar is one of its steps).
#include "skyscope.h"

#include "colors.h"
#include "swipe.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::skyscope {
static const char *const TAG = "skyscope.tour";
using namespace widgets;

namespace {
constexpr uint32_t LOOK_MS = 100;  // the glass is looked at this often for the touch a step waits for
}  // namespace

tour::Seen Skyscope::tour_seen_() const {
  tour::Seen seen;
  seen.planes = shown_count_ > 0;
  seen.can_swipe = !(offline_ && !offline_card_);
  seen.card = !lv_obj_has_flag(card_, LV_OBJ_FLAG_HIDDEN);
  seen.address = address_until_ != 0;
  seen.level = !lv_obj_has_flag(level_, LV_OBJ_FLAG_HIDDEN);
  seen.zooming = previewing_;
  seen.swiping = swiping_;
  seen.weather = wx_ != nullptr;
  seen.tally = today_ != nullptr;
  seen.menu = menu_ != nullptr;
  return seen;
}

void Skyscope::tour_tick_(uint32_t now) {
  if (!settings_.tour) {
    close_tour_();
    return;
  }
  if (now - tour_looked_at_ < LOOK_MS) return;
  tour_looked_at_ = now;
  if (!tour_) {
    // It begins over the map alone: the screen on its network with a place to watch, and nothing else open.
    if (!can_pinch_() || previewing_ || swiping_ || wx_ || today_) return;
    open_tour_();
    return;
  }
  if (tour_guide_.look(tour_seen_(), now)) show_tour_();
  // Out of the way of a page over the glass, and of the screen saying it is joining a network or updating.
  set_hidden(tour_, menu_ || wx_ || today_ || !lv_obj_has_flag(setup_, LV_OBJ_FLAG_HIDDEN));
}

void Skyscope::open_tour_() {
  tour_guide_.start();
  show_tour_();
  ESP_LOGI(TAG, "The tour begins");
}

void Skyscope::show_tour_() {
  if (tour_) lv_obj_delete(tour_);
  tour_ = tour_dots_ = tour_title_ = tour_body_ = tour_end_ = tour_next_ = nullptr;
  if (tour_guide_.over()) return;
  const tour::Seen seen = tour_seen_();
  const tour::Step step = tour_guide_.step();
  const tour::Words words = tour::words(step, seen);
  const bool done = tour_guide_.done();
  if (!menu_) menu_settings_ = settings_;  // the keys' colors (menu.cpp button_)
  const Chrome c = chrome_();

  tour_ = plain(lv_screen_active());
  lv_obj_set_size(tour_, std::min(glass::page_width(ui::shape, ui::mm(1.5f)), ui::mm(58)), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(tour_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(tour_, (lv_opa_t) 244, 0);  // nearly solid, as a plane's card: a chart shows through less
  lv_obj_set_style_border_color(tour_, lv_color_hex(c.accent), 0);
  lv_obj_set_style_border_width(tour_, std::max(1, ui::px(1.5f)), 0);
  lv_obj_set_style_radius(tour_, ui::mm(1.6f), 0);
  lv_obj_set_style_pad_all(tour_, ui::mm(1.2f), 0);
  lv_obj_set_flex_flow(tour_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(tour_, ui::mm(0.8f), 0);
  lv_obj_remove_flag(tour_, LV_OBJ_FLAG_CLICKABLE);  // a finger on its words reaches the map
  // High on the glass, under the strip a swipe down starts from and the status line; on round glass in the middle,
  // where the glass is widest.
  if (ui::shape.round) lv_obj_align(tour_, LV_ALIGN_CENTER, 0, -ui::mm(4));
  else lv_obj_align(tour_, LV_ALIGN_TOP_MID, 0, ui::mm(swipe::STRIP_MM + 2.5f));

  // A dot for each touch the tour asks for: those made in the accent, this one in the ink, the rest still faint.
  tour_dots_ = plain(tour_);
  lv_obj_set_size(tour_dots_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tour_dots_, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tour_dots_, ui::mm(1), 0);
  lv_obj_remove_flag(tour_dots_, LV_OBJ_FLAG_CLICKABLE);
  const int at = tour::touch_index(step, seen), side = ui::mm(1.4f);
  for (int i = 0; i < tour::touches(seen); ++i) {
    lv_obj_t *dot = plain(tour_dots_);
    lv_obj_set_size(dot, side, side);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    const bool made = i < at || (i == at && done);
    lv_obj_set_style_bg_color(dot, lv_color_hex(made ? c.accent : i == at ? c.ink : c.muted), 0);
    lv_obj_set_style_bg_opa(dot, made || i == at ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  }

  tour_title_ = text(tour_, ui::fonts.title, c.ink, words.title);
  lv_obj_set_width(tour_title_, lv_pct(100));
  lv_label_set_long_mode(tour_title_, LV_LABEL_LONG_WRAP);
  // What to do; once it was done, what it did, in the accent.
  const bool says_done = done && *words.done;
  tour_body_ = text(tour_, ui::fonts.label, says_done ? c.accent : c.ink, says_done ? words.done : words.ask);
  lv_obj_set_width(tour_body_, lv_pct(100));
  lv_label_set_long_mode(tour_body_, LV_LABEL_LONG_WRAP);

  // Its keys: the tour ended, or on to the next step whether or not this one's touch was made. The goodbye has one.
  lv_obj_t *keys = plain(tour_);
  lv_obj_set_size(keys, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(keys, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(keys, LV_OBJ_FLAG_CLICKABLE);
  const int tall = ui::mm(7);
  if (step != tour::FINISH) {
    tour_end_ = button_(keys, step == tour::WELCOME ? "Not now" : "End tour", LV_SIZE_CONTENT, tall);
    lv_obj_add_event_cb(tour_end_, [](lv_event_t *e) {
      auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
      // After this event: the key pressed is part of what goes.
      self->defer([self]() { self->end_tour_(); });
    }, LV_EVENT_CLICKED, this);
  }
  const char *next = step == tour::WELCOME ? "Start" : step == tour::FINISH ? "Done" : "Next";
  tour_next_ = button_(keys, next, LV_SIZE_CONTENT, tall, -1, true);
  lv_obj_add_event_cb(tour_next_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() {
      if (!self->tour_) return;
      self->tour_guide_.next(self->tour_seen_());
      if (self->tour_guide_.over()) self->end_tour_();
      else self->show_tour_();
    });
  }, LV_EVENT_CLICKED, this);
  set_hidden(tour_, menu_ || wx_ || today_ || !lv_obj_has_flag(setup_, LV_OBJ_FLAG_HIDDEN));
  ESP_LOGD(TAG, "Step %s%s", tour::NAMES[step], done ? ", done" : "");
}

void Skyscope::close_tour_() {
  if (!tour_) return;
  lv_obj_delete(tour_);
  tour_ = tour_dots_ = tour_title_ = tour_body_ = tour_end_ = tour_next_ = nullptr;
}

void Skyscope::end_tour_() {
  close_tour_();
  if (!settings_.tour) return;
  Settings s = settings_;
  s.tour = false;
  use_settings_(s);
  ESP_LOGI(TAG, "The tour is over");
}
}  // namespace esphome::skyscope
