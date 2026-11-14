// The menu's color picker for the radar's phosphor (colorpick.h has its sums), over the menu as the keyboard is.
//
// At its top Cancel and Done; under them a small scope lit in the color (its ground, a ring and a plane, as the radar
// would draw them), the color written out (a tap types it whole, on the keyboard) and a line about it. Then a swatch
// for each named phosphor, and a slider for each of red, green and blue: a finger anywhere along one sets it, and its
// number beside it is typed on the keyboard too. Each slider's track runs from its channel off to full with the other
// two as they are, so it shows what moving it does. Done puts the color in the menu's copy of the settings (lit enough
// for a scope, as the line said), and the menu is built again in it; nothing is applied until the menu's Save.
#include "skyscope.h"

#include "colorpick.h"
#include "colors.h"
#include "menu_rows.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/log.h"

#include <algorithm>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.color";
using namespace widgets;

namespace {
Skyscope *self_of(lv_event_t *e) { return static_cast<Skyscope *>(lv_event_get_user_data(e)); }
int index_of(lv_event_t *e) { return (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target_obj(e)); }
lv_obj_t *row_in(lv_obj_t *parent, int gap) {
  lv_obj_t *r = plain(parent);
  lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, gap, 0);
  return r;
}
lv_obj_t *circle(lv_obj_t *parent, int side) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, side, side);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
// A slider's track and knob stay where the finger is: no drag along one scrolls the page under it.
void hold_scroll(lv_obj_t *o) {
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
}
}  // namespace

void Skyscope::open_color_(int index) {
  const auto &rows = menu::rows();
  if (index < 0 || index >= (int) rows.size() || !menu::colored(rows[index])) return;
  close_color_();
  color_row_ = index;
  color_now_ = menu::color_of(rows[index], menu_settings_);
  const Chrome c = chrome_for_(menu_settings_);
  const uint32_t surface = colors::mix(c.ink, c.card, 20);
  const int tall = ui::mm(8), gap = ui::mm(1);

  color_page_ = plain(lv_screen_active());
  lv_obj_set_size(color_page_, width_, height_);
  lv_obj_set_style_bg_color(color_page_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(color_page_, LV_OPA_COVER, 0);
  ui::pad_page(color_page_, ui::mm(1));
  lv_obj_set_flex_flow(color_page_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(color_page_, ui::mm(0.8f), 0);
  lv_obj_add_flag(color_page_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched

  // Cancel, the row's name, and Done.
  lv_obj_t *head = row_in(color_page_, gap);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  color_cancel_ = button_(head, "Cancel", LV_SIZE_CONTENT, tall);
  lv_obj_add_event_cb(color_cancel_, [](lv_event_t *e) {
    Skyscope *self = self_of(e);
    self->defer([self]() { self->close_color_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_t *title = text(head, ui::fonts.title, c.ink, rows[index].name);
  lv_obj_set_flex_grow(title, 1);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  color_done_key_ = button_(head, "Done", LV_SIZE_CONTENT, tall, -1, true);
  lv_obj_add_event_cb(color_done_key_, [](lv_event_t *e) { self_of(e)->color_done_(); }, LV_EVENT_CLICKED, this);

  // The title on one line: in the menu's own words where the title's letters would wrap.
  lv_obj_update_layout(color_page_);
  if (lv_obj_get_height(title) > lv_font_get_line_height(ui::fonts.title))
    lv_obj_set_style_text_font(title, ui::fonts.label, 0);

  // The rest, in a list that scrolls should it still not fit (a round glass's corners), drawn smaller by one share
  // where the glass has less room than it takes (colorpick.h scale_for).
  lv_obj_t *body = plain(color_page_);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, ui::mm(1.2f), 0);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_update_layout(color_page_);
  const float share =
      colorpick::scale_for(lv_obj_get_content_width(body), lv_obj_get_content_height(body), ui::px_per_mm);
  auto mm = [share](float v) { return ui::mm(v * share); };
  const int key_tall = mm(8);
  lv_obj_set_style_pad_row(body, mm(1.2f), 0);

  // The color on a small scope, and beside it the color written out and what is said of it.
  lv_obj_t *top = row_in(body, mm(2));
  const int side = mm(15);
  color_scope_ = circle(top, side);
  lv_obj_set_style_bg_opa(color_scope_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(color_scope_, std::max(2, mm(0.35f)), 0);
  color_ring_ = circle(color_scope_, side / 2);
  lv_obj_set_style_border_width(color_ring_, std::max(1, mm(0.2f)), 0);
  lv_obj_center(color_ring_);
  color_blip_ = plain(color_scope_);
  lv_obj_set_size(color_blip_, mm(1.4f), mm(1.4f));
  lv_obj_set_style_bg_opa(color_blip_, LV_OPA_COVER, 0);
  lv_obj_align(color_blip_, LV_ALIGN_CENTER, side / 5, -side / 6);
  color_tag_ = text(color_scope_, ui::fonts.small, c.ink, "N123");
  lv_obj_align_to(color_tag_, color_blip_, LV_ALIGN_OUT_BOTTOM_MID, 0, mm(0.3f));
  lv_obj_t *about = plain(top);
  lv_obj_set_height(about, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(about, 1);
  lv_obj_set_flex_flow(about, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(about, mm(0.6f), 0);
  color_hex_ = button_(about, "", lv_pct(100), key_tall);
  lv_obj_set_style_radius(color_hex_, mm(1.2f), 0);
  lv_obj_add_event_cb(color_hex_, [](lv_event_t *e) {
    Skyscope *self = self_of(e);
    self->open_editor_(TYPING_COLOR, "Phosphor color", colorpick::hex_of(self->color_now_), keyboard::WORDS, 7,
                       "A color as #RRGGBB (#FF66CC), or a name: green, amber, blue, white, red, violet");
  }, LV_EVENT_CLICKED, this);
  color_note_ = text(about, ui::fonts.small, c.muted, "");
  lv_obj_set_width(color_note_, lv_pct(100));
  lv_label_set_long_mode(color_note_, LV_LABEL_LONG_WRAP);

  // The named phosphors, a swatch each.
  lv_obj_t *swatches = row_in(body, mm(1.6f));
  lv_obj_set_flex_flow(swatches, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(swatches, mm(1.2f), 0);
  lv_obj_set_flex_align(swatches, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(swatches, mm(1.2f), 0);
  lv_obj_set_style_pad_hor(swatches, mm(1.2f), 0);
  lv_obj_add_flag(swatches, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // the chosen one's ring, outside it
  color_swatches_.clear();
  for (int i = 0; i < colors::PHOSPHOR_COUNT; ++i) {
    lv_obj_t *s = circle(swatches, mm(7));
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s, mm(0.6f));
    lv_obj_set_style_bg_color(s, lv_color_hex(colors::PHOSPHORS[i].tint.glow), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_color(s, lv_color_hex(c.ink), 0);
    lv_obj_set_style_outline_pad(s, mm(0.5f), 0);
    lv_obj_set_user_data(s, (void *) (intptr_t) i);
    lv_obj_add_event_cb(s, [](lv_event_t *e) {
      Skyscope *self = self_of(e);
      self->color_now_ = colors::PHOSPHORS[index_of(e)].tint.glow;
      self->refresh_color_();
    }, LV_EVENT_CLICKED, this);
    color_swatches_.push_back(s);
  }

  // Red, green and blue: a name, a slider and its number.
  const int rail_tall = mm(7), track_tall = mm(2.6f), knob = mm(5);
  for (int i = 0; i < 3; ++i) {
    lv_obj_t *line = row_in(body, mm(1));
    lv_obj_t *name = text(line, ui::fonts.label, c.muted, colorpick::CHANNELS[i]);
    lv_obj_set_width(name, lv_pct(18));
    lv_obj_t *rail = plain(line);
    lv_obj_set_height(rail, rail_tall);
    lv_obj_set_flex_grow(rail, 1);
    lv_obj_add_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    hold_scroll(rail);
    lv_obj_set_user_data(rail, (void *) (intptr_t) i);
    lv_obj_add_event_cb(rail, [](lv_event_t *e) { self_of(e)->color_slide_(index_of(e)); }, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(rail, [](lv_event_t *e) { self_of(e)->color_slide_(index_of(e)); }, LV_EVENT_PRESSING, this);
    lv_obj_t *track = plain(rail);
    lv_obj_set_size(track, lv_pct(100), track_tall);
    lv_obj_set_style_radius(track, track_tall / 2, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(track, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_color(track, lv_color_hex(surface), 0);
    lv_obj_set_style_border_width(track, 1, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(track);
    lv_obj_t *k = circle(rail, knob);
    lv_obj_set_style_bg_color(k, lv_color_hex(c.ink), 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(k, lv_color_hex(c.card), 0);
    lv_obj_set_style_border_width(k, std::max(2, mm(0.4f)), 0);
    lv_obj_set_y(k, (rail_tall - knob) / 2);
    lv_obj_t *value = button_(line, "", mm(12), key_tall);
    lv_obj_set_style_radius(value, mm(1.2f), 0);
    lv_obj_set_user_data(value, (void *) (intptr_t) i);
    lv_obj_add_event_cb(value, [](lv_event_t *e) {
      Skyscope *self = self_of(e);
      const int which = index_of(e);
      self->typing_channel_ = which;
      self->open_editor_(TYPING_CHANNEL, colorpick::CHANNELS[which],
                         std::to_string(colorpick::channel(self->color_now_, which)), keyboard::WHOLE, 3,
                         "0 (none) to 255 (full)");
    }, LV_EVENT_CLICKED, this);
    color_rails_[i] = rail;
    color_tracks_[i] = track;
    color_knobs_[i] = k;
    color_values_[i] = value;
  }
  lv_obj_update_layout(color_page_);  // the rails' widths, for their knobs
  refresh_color_();
  ESP_LOGD(TAG, "Picking %s from %s, drawn at %.2f", rows[index].key, colorpick::hex_of(color_now_).c_str(), share);
}

void Skyscope::refresh_color_() {
  if (!color_page_) return;
  const uint32_t lit = colors::phosphor_lit(color_now_);
  const colors::Tint t = colors::tint(lit);
  // The small scope, as the radar would draw this color.
  lv_obj_set_style_bg_color(color_scope_, lv_color_hex(t.ground), 0);
  lv_obj_set_style_border_color(color_scope_, lv_color_hex(t.glow), 0);
  lv_obj_set_style_border_color(color_ring_, lv_color_hex(colors::glow_share(t, colors::share::RING)), 0);
  lv_obj_set_style_bg_color(color_blip_, lv_color_hex(t.hot), 0);
  set_color(color_tag_, t.glow);
  set_label(lv_obj_get_child(color_hex_, 0), colorpick::hex_of(color_now_));
  set_label(color_note_, colorpick::note_of(color_now_));
  for (int i = 0; i < (int) color_swatches_.size(); ++i) {
    const int ring = colors::PHOSPHORS[i].tint.glow == color_now_ ? std::max(2, ui::mm(0.4f)) : 0;
    if (lv_obj_get_style_outline_width(color_swatches_[i], LV_PART_MAIN) != ring)
      lv_obj_set_style_outline_width(color_swatches_[i], ring, 0);
  }
  for (int i = 0; i < 3; ++i) {
    const int v = colorpick::channel(color_now_, i);
    lv_obj_set_style_bg_color(color_tracks_[i], lv_color_hex(colorpick::with_channel(color_now_, i, 0)), 0);
    lv_obj_set_style_bg_grad_color(color_tracks_[i], lv_color_hex(colorpick::with_channel(color_now_, i, 255)), 0);
    const int x = colorpick::knob_left(v, lv_obj_get_width(color_rails_[i]), lv_obj_get_width(color_knobs_[i]));
    if (lv_obj_get_x(color_knobs_[i]) != x) lv_obj_set_x(color_knobs_[i], x);
    set_label(lv_obj_get_child(color_values_[i], 0), std::to_string(v));
  }
}

void Skyscope::color_slide_(int which) {
  if (!color_page_ || which < 0 || which > 2) return;
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  lv_area_t a;
  lv_obj_get_coords(color_rails_[which], &a);
  const int v = colorpick::value_at(p.x - a.x1, lv_area_get_width(&a), lv_obj_get_width(color_knobs_[which]));
  if (v == colorpick::channel(color_now_, which)) return;
  color_now_ = colorpick::with_channel(color_now_, which, v);
  refresh_color_();
}

void Skyscope::color_done_() {
  if (!color_page_) return;
  const auto &rows = menu::rows();
  if (color_row_ < 0 || color_row_ >= (int) rows.size()) return;
  const Chrome before = chrome_for_(menu_settings_);
  menu::color_in(rows[color_row_], menu_settings_, color_now_);
  ESP_LOGI(TAG, "Picked %s", colorpick::hex_of(menu::color_of(rows[color_row_], menu_settings_)).c_str());
  // Closed after this event: the key pressed is part of what goes. The menu in the color's own colors.
  this->defer([this, before]() {
    close_color_();
    menu_recolor_(before);
  });
}

void Skyscope::close_color_() {
  if (!color_page_) return;
  lv_obj_delete(color_page_);
  color_page_ = color_cancel_ = color_done_key_ = color_hex_ = color_note_ = nullptr;
  color_scope_ = color_ring_ = color_blip_ = color_tag_ = nullptr;
  for (int i = 0; i < 3; ++i) color_rails_[i] = color_tracks_[i] = color_knobs_[i] = color_values_[i] = nullptr;
  color_swatches_.clear();
}
}  // namespace esphome::skyscope
