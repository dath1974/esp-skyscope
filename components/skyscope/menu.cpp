// The screen's own settings menu: a long press on the map opens it. One row a setting (menu_rows.h): its name, and its
// value between two keys that step it back and on. Rows that do not apply to the map chosen are hidden, as on the web
// page; each change goes through the settings as the page's do, and is saved a few seconds later.
#include "skyscope.h"

#include "colours.h"
#include "menu_rows.h"
#include "ui.h"

#include "esphome/core/log.h"

#include <cstring>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.menu";

namespace {
lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, const char *s) {
  lv_obj_t *l = lv_label_create(parent);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
  lv_label_set_text(l, s);
  return l;
}
}  // namespace

void Skyscope::open_menu_() {
  if (menu_) return;
  const Chrome c = chrome_();
  menu_ = plain(lv_screen_active());
  lv_obj_set_size(menu_, width_, height_);
  lv_obj_set_style_bg_color(menu_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(menu_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(menu_, ui::mm(1.5f), 0);
  lv_obj_set_flex_flow(menu_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(menu_, ui::mm(1), 0);
  lv_obj_add_flag(menu_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched

  // The title, and the key that closes it.
  lv_obj_t *head = plain(menu_);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  text(head, ui::fonts.title, c.ink, "Settings");
  lv_obj_t *done = key_(head, "Done", -1, 0);
  lv_obj_set_width(done, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(done, ui::mm(2), 0);

  // The rows, in a list that scrolls.
  lv_obj_t *list = lv_obj_create(menu_);
  lv_obj_remove_style_all(list);
  lv_obj_set_width(list, lv_pct(100));
  lv_obj_set_flex_grow(list, 1);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list, ui::mm(0.8f), 0);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
  menu_list_ = list;
  const auto &rows = menu::rows();
  menu_rows_.assign(rows.size(), nullptr);
  menu_values_.assign(rows.size(), nullptr);
  for (size_t i = 0; i < rows.size(); ++i) {
    lv_obj_t *row = plain(list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ui::mm(1), 0);
    lv_obj_t *name = text(row, ui::fonts.label, c.muted, rows[i].name);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    key_(row, "<", (int) i, -1);
    lv_obj_t *value = text(row, ui::fonts.label, c.ink, "");
    // Room for the longest value; the keys either side stay put.
    lv_obj_set_width(value, std::min(ui::mm(17), width_ * 2 / 5));
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    key_(row, ">", (int) i, 1);
    menu_rows_[i] = row;
    menu_values_[i] = value;
  }
  // Where the rest is set.
  std::string where = "Place, custom map and rotation: on the web page";
  const std::string address = own_address();
  if (!address.empty()) where += ", http://" + address + "/";
  lv_obj_t *foot = text(list, ui::fonts.small, c.muted, where.c_str());
  lv_obj_set_width(foot, lv_pct(100));
  lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);

  // The planes stop being drawn under it.
  view::set_hidden(view_, true);
  refresh_menu_();
  ESP_LOGI(TAG, "Opened");
}

lv_obj_t *Skyscope::key_(lv_obj_t *parent, const char *label, int row, int step) {
  const Chrome c = chrome_();
  lv_obj_t *b = plain(parent);
  const int size = ui::mm(8);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, ui::px(8), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(c.ink), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *l = text(b, ui::fonts.title, c.ink, label);
  lv_obj_center(l);
  // The row and the step in its user data; -1 is the key that closes the menu.
  lv_obj_set_user_data(b, (void *) (intptr_t) (row < 0 ? -1 : row * 2 + (step > 0 ? 1 : 0)));
  lv_obj_add_event_cb(b, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    const intptr_t what = (intptr_t) lv_obj_get_user_data(lv_event_get_target_obj(e));
    // Closed after this event: the key is part of what goes.
    if (what < 0) self->defer([self]() { self->close_menu_(); });
    else self->menu_step_((int) (what / 2), what % 2 ? 1 : -1);
  }, LV_EVENT_CLICKED, this);
  return b;
}

void Skyscope::menu_step_(int index, int step) {
  const auto &rows = menu::rows();
  if (index < 0 || index >= (int) rows.size()) return;
  Settings s = settings_;
  if (!menu::stepped(rows[index], s, step)) return;
  const Chrome before = chrome_();
  use_settings_(s);
  const Chrome after = chrome_();
  if (before.ink != after.ink || before.card != after.card) {
    // Another appearance (or the radar's phosphor): the menu again in its colours, where it was scrolled to. After
    // this event, as the key pressed is part of what goes.
    const int32_t scrolled = lv_obj_get_scroll_y(menu_list_);
    this->defer([this, scrolled]() {
      close_menu_();
      open_menu_();
      lv_obj_scroll_to_y(menu_list_, scrolled, LV_ANIM_OFF);
    });
    return;
  }
  refresh_menu_();
}

void Skyscope::refresh_menu_() {
  if (!menu_) return;
  const auto &rows = menu::rows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const bool shown = menu::applies(rows[i], settings_);
    if (shown == lv_obj_has_flag(menu_rows_[i], LV_OBJ_FLAG_HIDDEN)) {
      if (shown) lv_obj_remove_flag(menu_rows_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(menu_rows_[i], LV_OBJ_FLAG_HIDDEN);
    }
    const std::string value = menu::shown(rows[i], settings_);
    if (value != lv_label_get_text(menu_values_[i])) lv_label_set_text(menu_values_[i], value.c_str());
  }
}

void Skyscope::close_menu_() {
  if (!menu_) return;
  lv_obj_delete(menu_);
  menu_ = nullptr;
  menu_list_ = nullptr;
  menu_rows_.clear();
  menu_values_.clear();
  // The radar's scope clean again (its beam stopped where it was while the menu was open), then the planes and beam.
  if (settings_.map == RADAR && scope_background_ && canvas_buffer_) {
    std::memcpy(canvas_buffer_, scope_background_, (size_t) width_ * height_ * 2);
    lv_obj_invalidate(canvas_);
  }
  view::set_hidden(view_, false);
  if (view_ && planes_) lv_obj_invalidate(planes_);
  last_status_ = 0;
  ESP_LOGI(TAG, "Closed");
}
}  // namespace esphome::skyscope
