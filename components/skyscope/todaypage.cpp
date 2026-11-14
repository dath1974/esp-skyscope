// The day's tally on the glass (today.h): the planes the screen has seen in its range since midnight, the hours they
// came in as a chart, today's records, the most common types and operators, and the previous daily totals. The web page has
// shown it from the start; here it is without a browser. A swipe down from the top edge of the map opens it, and so
// does the menu. It costs nothing to keep: the worker counts whether or not anyone looks.
#include "skyscope.h"

#include "colors.h"
#include "today.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.today";
using namespace widgets;

namespace {
constexpr uint32_t REFRESH_MS = 5000;  // looked at again this often while the page is open
}  // namespace

void Skyscope::open_today_() {
  if (today_) return;
  menu_settings_ = menu_ ? menu_settings_ : settings_;  // the keys' colors (menu.cpp button_)
  const Chrome c = chrome_for_(menu_settings_);
  const int tall = ui::mm(8);
  today_ = plain(lv_screen_active());
  lv_obj_set_size(today_, width_, height_);
  lv_obj_set_style_bg_color(today_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(today_, LV_OPA_COVER, 0);
  ui::pad_page(today_, ui::mm(1));
  lv_obj_set_flex_flow(today_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(today_, ui::mm(0.8f), 0);
  lv_obj_add_flag(today_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched

  // Close, and the title.
  lv_obj_t *head = plain(today_);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(head, ui::mm(1), 0);
  lv_obj_t *close = button_(head, "Close", LV_SIZE_CONTENT, tall);
  lv_obj_add_event_cb(close, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    self->defer([self]() { self->close_today_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_t *title = text(head, ui::fonts.title, c.ink, "Today in range");
  lv_obj_set_flex_grow(title, 1);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  // As wide as Close, so the title sits in the middle of the glass: where there is room for it and the title both.
  lv_obj_update_layout(head);
  const int title_wide = ui::text_width("Today in range", ui::fonts.title);
  if (lv_obj_get_content_width(head) - 2 * (lv_obj_get_width(close) + ui::mm(1)) >= title_wide) {
    lv_obj_t *balance = plain(head);
    lv_obj_set_size(balance, lv_obj_get_width(close), 1);
  }

  // What there is to say when there are no numbers yet, or they stand still.
  today_line_ = text(today_, ui::fonts.small, c.muted, "");
  lv_obj_set_width(today_line_, lv_pct(100));
  lv_obj_set_style_text_align(today_line_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(today_line_, LV_LABEL_LONG_WRAP);

  // The day's three numbers, a tile each: the number large, what it counts small under it.
  today_tiles_ = plain(today_);
  lv_obj_set_size(today_tiles_, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(today_tiles_, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(today_tiles_, ui::mm(0.8f), 0);
  for (size_t i = 0; i < 3; ++i) {
    lv_obj_t *tile = plain(today_tiles_);
    lv_obj_set_height(tile, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(tile, 1);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(tile, lv_color_hex(c.ink), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_10, 0);
    lv_obj_set_style_radius(tile, ui::mm(1.4f), 0);
    lv_obj_set_style_pad_ver(tile, ui::mm(0.8f), 0);
    today_stat_values_[i] = text(tile, ui::fonts.title, i == 0 ? c.accent : c.ink, "");
    today_stat_captions_[i] = text(tile, ui::fonts.small, c.muted, "");
  }

  // The hours: a bar each, as tall as the planes first seen in it, over the hours of the day; this hour's stands out.
  today_chart_ = plain(today_);
  lv_obj_set_size(today_chart_, lv_pct(100), ui::mm(11));
  lv_obj_set_flex_flow(today_chart_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(today_chart_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(today_chart_, std::max(1, ui::px(2)), 0);
  for (lv_obj_t *&bar : today_bars_) {
    bar = plain(today_chart_);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_height(bar, 1);
    lv_obj_set_style_bg_color(bar, lv_color_hex(c.accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, std::max(1, ui::px(2)), 0);
  }
  lv_obj_t *hours = plain(today_);
  lv_obj_set_size(hours, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(hours, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hours, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  for (const char *hour : {"0:00", "6:00", "12:00", "18:00", "24:00"}) text(hours, ui::fonts.small, c.muted, hour);

  // The rest, scrolling.
  today_list_ = lv_obj_create(today_);
  lv_obj_remove_style_all(today_list_);
  lv_obj_set_width(today_list_, lv_pct(100));
  lv_obj_set_flex_grow(today_list_, 1);
  lv_obj_set_flex_flow(today_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(today_list_, ui::mm(0.5f), 0);
  lv_obj_add_flag(today_list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(today_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(today_list_, LV_SCROLLBAR_MODE_ACTIVE);
  today_rev_ = 0;
  today_at_ = 0;
  refresh_today_();
  ESP_LOGD(TAG, "Opened");
}

void Skyscope::refresh_today_() {
  if (!today_) return;
  const uint32_t now = millis();
  if (today_at_ && now - today_at_ < REFRESH_MS) return;
  today_at_ = now | 1;
  // The words and the hours taken under the lock, the glass set after it.
  std::string summary;
  std::vector<today::Line> lines;
  std::vector<today::Stat> stats;
  uint16_t hours[24];
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (shared_.book_rev == today_rev_ && today_rev_) return;
    today_rev_ = shared_.book_rev | 1;
    summary = today::summary(shared_.book);
    lines = today::lines(shared_.book, settings_.metric);
    stats = today::stats(shared_.book);
    std::copy(std::begin(shared_.book.today().hours), std::end(shared_.book.today().hours), hours);
  }
  const Chrome c = chrome_for_(menu_settings_);
  // The tiles say the day's numbers; the line says only what they cannot. While a plane is followed elsewhere nothing
  // is counted: said, so a count standing still is no puzzle.
  const bool following = following_();
  today_texts_.clear();
  today_texts_.push_back(summary + (following ? ". Not counting while a plane is followed" : ""));
  set_label(today_line_, stats.empty() ? summary : following ? "Not counting while a plane is followed" : "");
  set_hidden(today_line_, !stats.empty() && !following);
  set_hidden(today_tiles_, stats.empty());
  for (size_t i = 0; i < 3 && i < stats.size(); ++i) {
    set_label(today_stat_values_[i], stats[i].value);
    set_label(today_stat_captions_[i], stats[i].caption);
  }
  int hour_now = -1;
#ifdef USE_TIME
  if (clock_ && clock_->now().is_valid()) hour_now = clock_->now().hour;
#endif
  const int most = std::max<int>(1, *std::max_element(std::begin(hours), std::end(hours)));
  lv_obj_update_layout(today_chart_);
  const int room = lv_obj_get_height(today_chart_) > 0 ? lv_obj_get_height(today_chart_) : ui::mm(11);
  for (int h = 0; h < 24; ++h) {
    // An hour with planes shows at least a sliver; one with none, a line along the foot. This hour's is the brightest.
    const int tall = hours[h] ? std::max(ui::px(3), room * hours[h] / most) : std::max(1, ui::px(1));
    if (lv_obj_get_height(today_bars_[h]) != tall) lv_obj_set_height(today_bars_[h], tall);
    lv_obj_set_style_bg_opa(today_bars_[h], h == hour_now ? LV_OPA_COVER : hours[h] ? LV_OPA_60 : LV_OPA_20, 0);
  }
  const int32_t scrolled = lv_obj_get_scroll_y(today_list_);
  lv_obj_clean(today_list_);
  const int label_wide = ui::text_width("Nearest", ui::fonts.label) + ui::mm(1.5f);
  for (const today::Line &line : lines) {
    today_texts_.push_back(line.text);
    const bool heading = line.tone == today::HEADING;
    const uint32_t ink = line.tone == today::QUIET ? c.muted : c.ink;
    if (line.name.empty()) {
      lv_obj_t *l = text(today_list_, heading ? ui::fonts.small : ui::fonts.label, heading ? c.accent : ink,
                         line.text.c_str());
      lv_obj_set_width(l, lv_pct(100));
      lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
      if (heading) lv_obj_set_style_pad_top(l, ui::mm(1), 0);
      continue;
    }
    // A row in columns: what it is, whose it is, and how much at the right; behind a list's row a bar as long as
    // its share of the longest.
    lv_obj_t *row = plain(today_list_);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ui::mm(1), 0);
    if (line.share >= 0) {
      lv_obj_t *bar = plain(row);
      lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
      lv_obj_set_size(bar, lv_pct(std::max(2, (int) std::lround(line.share * 100))), lv_pct(100));
      lv_obj_set_style_bg_color(bar, lv_color_hex(c.accent), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_20, 0);
      lv_obj_set_style_radius(bar, ui::mm(0.6f), 0);
    }
    if (!line.label.empty()) {
      lv_obj_t *l = text(row, ui::fonts.label, c.muted, line.label.c_str());
      lv_obj_set_width(l, label_wide);
    }
    lv_obj_t *name = text(row, ui::fonts.label, ink, line.name.c_str());
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    if (line.share >= 0) lv_obj_set_style_pad_left(name, ui::mm(0.8f), 0);
    lv_obj_t *value = text(row, ui::fonts.label, ink, line.value.c_str());
    lv_obj_set_style_pad_right(value, ui::mm(0.8f), 0);
  }
  lv_obj_update_layout(today_list_);
  lv_obj_scroll_to_y(today_list_, scrolled, LV_ANIM_OFF);
}

void Skyscope::close_today_() {
  if (!today_) return;
  lv_obj_delete(today_);
  today_ = today_line_ = today_tiles_ = today_chart_ = today_list_ = nullptr;
  for (auto &bar : today_bars_) bar = nullptr;
  for (size_t i = 0; i < 3; ++i) today_stat_values_[i] = today_stat_captions_[i] = nullptr;
  std::vector<std::string>().swap(today_texts_);
  ESP_LOGD(TAG, "Closed");
}
}  // namespace esphome::skyscope
