// The screen's own settings menu: a long press on the map opens it. One row a setting (menu_rows.h): its name, and its
// value between two keys that step it back and on, or in a field a tap opens the keyboard on (editor.cpp), or both.
// The changes wait in a copy of the settings and are applied together by Save (Cancel drops them), so the map is drawn
// again once, not at every step; rows that do not apply to the map chosen in the menu are hidden, as on the web page.
// The rows are grouped under headings, a changed value shows in the accent until Save, and the arrows at the foot turn
// the list a page at a time. Then come the Wi-Fi network, whose field opens the networks in reach (editor.cpp), and the
// firmware: the version running, the newest release, Check and Install (a second tap installs; the menu's changes are
// dropped).
#include "skyscope.h"

#include "colours.h"
#include "firmware.h"
#include "menu_rows.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.menu";

using namespace widgets;

namespace {
// The menu's own keys, by negative ids in their user data.
constexpr int MENU_SAVE = -1, MENU_CANCEL = -2, MENU_UP = -3, MENU_DOWN = -4, MENU_CHECK = -5, MENU_INSTALL = -6,
              MENU_WIFI = -7;
// A row's keys, by row * 3 and one of these in their user data.
enum { STEP_BACK, STEP_ON, EDIT };
constexpr uint32_t ARMED_MS = 4000;     // Install's second tap, within this
constexpr uint32_t CHECKING_MS = 20000;  // a check unanswered for this long is said no more
}  // namespace

void Skyscope::open_menu_() {
  if (menu_) return;
  menu_settings_ = settings_;
  build_menu_();
  // The planes stop being drawn under it.
  view::set_hidden(view_, true);
  ESP_LOGI(TAG, "Opened");
}

void Skyscope::build_menu_() {
  // In the colours of what is being chosen: another appearance or phosphor shows on the menu at once, and on the map
  // only with Save.
  const Chrome c = chrome_for_(menu_settings_);
  const uint32_t surface = colours::mix(c.ink, c.card, 20);  // the groups' cards, a shade off the ground
  const int tall = ui::mm(8);
  make_arrows(ui::mm(2.6f), std::max(2, ui::mm(0.55f)));
  menu_ = plain(lv_screen_active());
  lv_obj_set_size(menu_, width_, height_);
  lv_obj_set_style_bg_color(menu_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(menu_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(menu_, ui::mm(1), 0);
  lv_obj_set_flex_flow(menu_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(menu_, ui::mm(1), 0);
  lv_obj_add_flag(menu_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched

  // Cancel, the title, and Save.
  lv_obj_t *head = plain(menu_);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  menu_cancel_ = key_(head, "Cancel", MENU_CANCEL, 0, LV_SIZE_CONTENT, tall);
  text(head, ui::fonts.title, c.ink, "Settings");
  menu_save_ = key_(head, "Save", MENU_SAVE, 0, LV_SIZE_CONTENT, tall, -1, true);

  // The groups, in a list that scrolls.
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
  // A group: its heading in the accent, over a card of its rows. Returns the card; `box` the two together.
  auto group = [&](const char *name, lv_obj_t *&box) {
    box = plain(list);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, ui::mm(0.4f), 0);
    lv_obj_t *heading = text(box, ui::fonts.small, c.accent, name);
    lv_obj_set_style_pad_left(heading, ui::mm(1), 0);
    lv_obj_set_style_pad_top(heading, ui::mm(0.6f), 0);
    lv_obj_t *card = plain(box);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(surface), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, ui::mm(1.6f), 0);
    lv_obj_set_style_pad_all(card, ui::mm(0.8f), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, ui::mm(0.5f), 0);
    return card;
  };
  // A row of a card: its name a third of it (wrapping between words when long), then what follows.
  auto row_of = [&](lv_obj_t *card, const char *name) {
    lv_obj_t *row = plain(card);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ui::mm(0.6f), 0);
    lv_obj_t *label = text(row, ui::fonts.label, c.muted, name);
    lv_obj_set_width(label, lv_pct(30));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return row;
  };

  const auto &rows = menu::rows();
  menu_rows_.assign(rows.size(), nullptr);
  menu_values_.assign(rows.size(), nullptr);
  menu_groups_.assign(rows.size(), nullptr);
  menu_fields_.assign(rows.size(), nullptr);
  // A field: a key as wide as the row leaves it, squarer than the others, its label the value. A tap types in it.
  auto field_of = [&](lv_obj_t *row, int what, int step) {
    lv_obj_t *f = key_(row, "", what, step, 0, tall);
    lv_obj_set_flex_grow(f, 1);
    lv_obj_set_style_radius(f, ui::mm(1.2f), 0);
    lv_obj_t *value = lv_obj_get_child(f, 0);
    lv_obj_set_width(value, lv_pct(100));
    lv_obj_set_style_pad_hor(value, ui::mm(1), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    return f;
  };
  lv_obj_t *card = nullptr, *box = nullptr;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (const menu::Group *g = menu::group_starting(rows[i])) card = group(g->name, box);
    lv_obj_t *row = row_of(card, rows[i].name);
    // The value between its keys, whatever room they leave it: in a field where it is typed.
    const bool steps = menu::steps(rows[i]);
    if (steps) key_(row, nullptr, (int) i, -1, ui::mm(6.5f), tall, ARROW_LEFT);
    lv_obj_t *value;
    if (menu::typed(rows[i])) {
      menu_fields_[i] = field_of(row, (int) i, 0);
      value = lv_obj_get_child(menu_fields_[i], 0);
    } else {
      value = text(row, ui::fonts.label, c.ink, "");
      lv_obj_set_flex_grow(value, 1);
      lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    }
    if (steps) key_(row, nullptr, (int) i, 1, ui::mm(6.5f), tall, ARROW_RIGHT);
    menu_rows_[i] = row;
    menu_values_[i] = value;
    menu_groups_[i] = box;
  }

  // The Wi-Fi network the screen is on; its field opens the networks in reach (editor.cpp).
  card = group("Wi-Fi", box);
  menu_wifi_ = field_of(row_of(card, "Network"), MENU_WIFI, 0);
  menu_wifi_value_ = lv_obj_get_child(menu_wifi_, 0);

  // The firmware: the version running, and (with updates built in) the newest release, Check and Install.
  card = group("Firmware", box);
  lv_obj_t *version = row_of(card, "Version");
  lv_obj_t *running = text(version, ui::fonts.label, c.ink, version_.c_str());
  lv_obj_set_flex_grow(running, 1);
  lv_obj_set_style_text_align(running, LV_TEXT_ALIGN_CENTER, 0);
  menu_fw_line_ = menu_fw_install_ = menu_fw_check_ = nullptr;
#ifdef USE_UPDATE
  if (firmware_) {
    menu_fw_line_ = text(card, ui::fonts.label, c.muted, "");
    lv_obj_set_width(menu_fw_line_, lv_pct(100));
    lv_obj_set_style_text_align(menu_fw_line_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(menu_fw_line_, LV_LABEL_LONG_WRAP);
    lv_obj_t *keys = plain(card);
    lv_obj_set_size(keys, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(keys, ui::mm(1), 0);
    menu_fw_check_ = key_(keys, "Check", MENU_CHECK, 0, 0, tall);
    lv_obj_set_flex_grow(menu_fw_check_, 1);
    menu_fw_install_ = key_(keys, "Install", MENU_INSTALL, 0, 0, tall, -1, true);
    lv_obj_set_flex_grow(menu_fw_install_, 2);
  }
#endif

  // Where else they are set.
  std::string where = "Another way up restarts the screen when saved. All of this is on the screen's web page too";
  const std::string address = own_address();
  where += address.empty() ? "." : ": http://" + address + "/";
  lv_obj_t *foot = text(list, ui::fonts.small, c.muted, where.c_str());
  lv_obj_set_width(foot, lv_pct(100));
  lv_obj_set_style_pad_all(foot, ui::mm(1), 0);
  lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);

  // A page up and a page down.
  lv_obj_t *foot_keys = plain(menu_);
  lv_obj_set_size(foot_keys, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(foot_keys, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(foot_keys, ui::mm(1), 0);
  menu_up_ = key_(foot_keys, nullptr, MENU_UP, 0, 0, tall, ARROW_UP);
  menu_down_ = key_(foot_keys, nullptr, MENU_DOWN, 0, 0, tall, ARROW_DOWN);
  lv_obj_set_flex_grow(menu_up_, 1);
  lv_obj_set_flex_grow(menu_down_, 1);

  menu_install_armed_until_ = 0;
  refresh_menu_();
}

lv_obj_t *Skyscope::button_(lv_obj_t *parent, const char *label, int width, int height, int arrow_of, bool primary) {
  const Chrome c = chrome_for_(menu_settings_);
  // A primary key filled with the accent, its face in the ground's colour; the others a quiet tint of the ink.
  const uint32_t face = primary ? c.card : c.ink;
  lv_obj_t *b = plain(parent);
  lv_obj_set_size(b, width, height);
  if (width == LV_SIZE_CONTENT) lv_obj_set_style_pad_hor(b, ui::mm(2.5f), 0);
  lv_obj_set_style_radius(b, height / 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? c.accent : c.ink), 0);
  lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_bg_opa(b, primary ? LV_OPA_70 : LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *f = label ? text(b, ui::fonts.label, face, label) : arrow(b, arrow_of, face);
  lv_obj_center(f);
  return b;
}

lv_obj_t *Skyscope::key_(lv_obj_t *parent, const char *label, int row, int step, int width, int height, int arrow_of,
                         bool primary) {
  lv_obj_t *b = button_(parent, label, width, height, arrow_of, primary);
  lv_obj_set_user_data(b, (void *) (intptr_t) (row < 0 ? row : row * 3 + (step < 0 ? STEP_BACK : step > 0 ? STEP_ON : EDIT)));
  lv_obj_add_event_cb(b, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    const intptr_t what = (intptr_t) lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (what < 0) self->menu_action_((int) what);
    else self->menu_step_((int) (what / 3), what % 3 == STEP_BACK ? -1 : what % 3 == STEP_ON ? 1 : 0);
  }, LV_EVENT_CLICKED, this);
  return b;
}

void Skyscope::menu_step_(int index, int step) {
  const auto &rows = menu::rows();
  if (index < 0 || index >= (int) rows.size()) return;
  if (step == 0) {
    open_row_editor_(index);
    return;
  }
  const Chrome before = chrome_for_(menu_settings_);
  if (!menu::stepped(rows[index], menu_settings_, step)) return;
  const Chrome after = chrome_for_(menu_settings_);
  if (before.ink == after.ink && before.card == after.card && before.muted == after.muted) {
    refresh_menu_();
    return;
  }
  // Other colours (the appearance, the phosphor, or the radar against a map): the menu built again in them, where it
  // was scrolled to. After this event, as the key pressed is part of what goes. Nothing else changes until Save.
  const int32_t scrolled = lv_obj_get_scroll_y(menu_list_);
  this->defer([this, scrolled]() {
    if (!menu_) return;
    lv_obj_delete(menu_);
    menu_ = nullptr;
    build_menu_();
    lv_obj_update_layout(menu_);
    lv_obj_scroll_to_y(menu_list_, scrolled, LV_ANIM_OFF);
  });
}

void Skyscope::menu_action_(int action) {
  if (action == MENU_UP || action == MENU_DOWN) {
    // A page: the list's height less a row, so the row at the edge stays in view.
    const int32_t page = std::max<int32_t>(1, lv_obj_get_content_height(menu_list_) - ui::mm(8));
    const int32_t at = lv_obj_get_scroll_y(menu_list_), most = at + lv_obj_get_scroll_bottom(menu_list_);
    const int32_t to = std::max<int32_t>(0, std::min<int32_t>(most, at + (action == MENU_DOWN ? page : -page)));
    lv_obj_scroll_to_y(menu_list_, to, LV_ANIM_OFF);
    return;
  }
  if (action == MENU_WIFI) {
    open_picker_(PICKING_NETWORKS);
    return;
  }
#ifdef USE_UPDATE
  if (action == MENU_CHECK && firmware_) {
    menu_checking_from_ = firmware_reports_;
    menu_checking_until_ = millis() + CHECKING_MS;
    firmware_->check();
    refresh_menu_firmware_();
    return;
  }
  if (action == MENU_INSTALL && firmware_) {
    // The first tap asks for a second; the second installs, and the menu goes without its changes.
    if (!menu_install_armed_until_ || (int32_t) (millis() - menu_install_armed_until_) >= 0) {
      menu_install_armed_until_ = millis() + ARMED_MS;
      refresh_menu_firmware_();
      return;
    }
    this->defer([this]() {
      close_menu_();
      install_firmware_();
    });
    return;
  }
#endif
  if (action != MENU_SAVE && action != MENU_CANCEL) return;
  const bool apply = action == MENU_SAVE;
  const Settings changed = menu_settings_;
  // Closed after this event: the key pressed is part of what goes. The settings after it, all at once.
  this->defer([this, apply, changed]() {
    close_menu_();
    if (apply) apply_menu_(changed);
  });
}

void Skyscope::apply_menu_(const Settings &changed) {
  JsonDocument a, b;
  to_json(changed, a.to<JsonObject>());
  to_json(settings_, b.to<JsonObject>());
  std::string was, now;
  serializeJson(b, was);
  serializeJson(a, now);
  if (now != was) use_settings_(changed);
}

void Skyscope::refresh_menu_() {
  if (!menu_) return;
  const Chrome c = chrome_for_(menu_settings_);
  const auto &rows = menu::rows();
  // A group shows while one of its rows applies to the map chosen.
  std::vector<lv_obj_t *> shown_groups;
  for (size_t i = 0; i < rows.size(); ++i) {
    const bool shown = menu::applies(rows[i], menu_settings_);
    set_hidden(menu_rows_[i], !shown);
    if (shown) shown_groups.push_back(menu_groups_[i]);
    const std::string value = menu::shown(rows[i], menu_settings_);
    set_label(menu_values_[i], value);
    // Changed in the menu, not yet applied: in the accent until Save.
    set_colour(menu_values_[i], value != menu::shown(rows[i], settings_) ? c.accent : c.ink);
  }
  for (lv_obj_t *g : menu_groups_)
    set_hidden(g, std::find(shown_groups.begin(), shown_groups.end(), g) == shown_groups.end());
  refresh_menu_wifi_();
  refresh_menu_firmware_();
}

void Skyscope::refresh_menu_wifi_() {
  if (!menu_wifi_value_) return;
  std::string name = "Not connected";
  auto *wifi = wifi::global_wifi_component;
  if (wifi && wifi->is_connected()) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    name = wifi->wifi_ssid_to(ssid);
  }
  set_label(menu_wifi_value_, name);
}

void Skyscope::refresh_menu_firmware_() {
#ifdef USE_UPDATE
  if (!menu_ || !menu_fw_line_) return;
  const Chrome c = chrome_for_(menu_settings_);
  const uint32_t now = millis();
  Shared::Firmware f;
  {
    std::lock_guard<std::mutex> guard(lock_);
    f = shared_.firmware;
  }
  const bool checking = menu_checking_until_ && (int32_t) (now - menu_checking_until_) < 0 &&
                        firmware_reports_ == menu_checking_from_ && f.error.empty();
  if (!checking) menu_checking_until_ = 0;
  const firmware::Line line = firmware::menu_line(checking, f.known, f.available, f.installing, f.latest, f.error);
  set_label(menu_fw_line_, line.text);
  set_colour(menu_fw_line_, line.tone == firmware::BAD_NEWS ? colours::ERROR
                            : line.tone == firmware::GOOD_NEWS ? c.accent : c.muted);
  const bool armed = menu_install_armed_until_ && (int32_t) (now - menu_install_armed_until_) < 0;
  if (!armed) menu_install_armed_until_ = 0;
  set_hidden(menu_fw_install_, !f.available || f.installing);
  set_label(lv_obj_get_child(menu_fw_install_, 0), armed ? std::string("Tap again to install") : "Install " + f.latest);
  if (f.installing) lv_obj_add_state(menu_fw_check_, LV_STATE_DISABLED);
  else lv_obj_remove_state(menu_fw_check_, LV_STATE_DISABLED);
#endif
}

void Skyscope::close_menu_() {
  if (!menu_) return;
  close_picker_();
  close_editor_();
  lv_obj_delete(menu_);
  menu_ = nullptr;
  menu_list_ = nullptr;
  menu_rows_.clear();
  menu_values_.clear();
  menu_groups_.clear();
  menu_fields_.clear();
  menu_wifi_ = menu_wifi_value_ = nullptr;
  menu_fw_line_ = menu_fw_install_ = menu_fw_check_ = nullptr;
  menu_save_ = menu_cancel_ = menu_up_ = menu_down_ = nullptr;
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
