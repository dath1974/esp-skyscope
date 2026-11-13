// The screen's own settings menu: a long press on the map opens it. At its top are the pages a swipe opens too (the
// weather, the day's tally). Then one row a setting (menu_rows.h), grouped under headings as the web page groups them:
// its name, and its value between two keys that step it back and on, or a switch for one that is on or off, or a field
// a tap opens the keyboard on (editor.cpp), or both. The changes wait in a copy of the settings and are applied
// together by Save (Cancel drops them), so the map is drawn again once, not at every step; rows that do not apply to
// the map chosen in the menu are hidden, as on the web page. A changed value shows in the accent until Save, and the
// arrows at the foot turn the list a page at a time.
//
// The Network group begins with the Wi-Fi network, whose field opens the networks in reach (editor.cpp). On a board
// with a card slot the map card follows, as on the web page: what the card is, its space as a bar (the tiles, the log
// of planes seen, everything else, and free), what the screen keeps on it map by map, and keys to look for it again,
// clear the tiles, format it, and stop a download; clearing and formatting ask first. The last group, About, has the firmware (the version running, the newest release, Check and Install: a second tap installs,
// and the menu's changes are dropped), the tour again (tour.cpp), a restart and the factory reset. The last two ask
// first, on a page of their own.
#include "skyscope.h"

#include "colors.h"
#include "firmware.h"
#include "menu_rows.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cstring>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.menu";

using namespace widgets;

namespace {
// The menu's own keys, by negative ids in their user data.
constexpr int MENU_SAVE = -1, MENU_CANCEL = -2, MENU_UP = -3, MENU_DOWN = -4, MENU_CHECK = -5, MENU_INSTALL = -6,
              MENU_WIFI = -7, MENU_WEATHER = -8, MENU_TODAY = -9, MENU_TOUR = -10, MENU_RESTART = -11,
              MENU_RESET = -12, MENU_CARD_MOUNT = -13, MENU_CARD_CLEAR = -14, MENU_CARD_FORMAT = -15,
              MENU_CARD_STOP = -16;
// A row's keys, by row * 3 and one of these in their user data.
enum { STEP_BACK, STEP_ON, EDIT };
constexpr uint32_t ARMED_MS = 4000;     // Install's second tap, within this
constexpr uint32_t CHECKING_MS = 20000;  // a check unanswered for this long is said no more
constexpr uint32_t LOOKING_MS = 3000;    // the card looked for again: said this long
// A question's keys take no tap this long after it comes up: the second tap of a double tap on the key that asked
// would land on one of them.
constexpr uint32_t ASK_DEAF_MS = 700;
// A switch: a pill with a knob in it, at its right end and filled with the accent when on.
constexpr float SWITCH_WIDE_MM = 11, SWITCH_TALL_MM = 6, SWITCH_PAD_MM = 0.7f;

void set_switch(lv_obj_t *track, bool on, uint32_t accent, uint32_t ink, uint32_t ground) {
  lv_obj_t *knob = lv_obj_get_child(track, 0);
  lv_obj_set_style_bg_color(track, lv_color_hex(on ? accent : ink), 0);
  lv_obj_set_style_bg_opa(track, on ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_bg_color(knob, lv_color_hex(on ? ground : ink), 0);
  lv_obj_align(knob, on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
}
}  // namespace

void Skyscope::open_menu_() {
  if (menu_) return;
  end_preview_(false);  // the view two fingers chose goes, as the menu shows the settings saved
  finish_swipe_();      // the map swiped to stays, and the menu shows it
  menu_settings_ = settings_;
  // What is on the map card, counted again if tiles were written since (worker.cpp), for the menu's card part.
  if (card_fitted_) {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.count_ask++;
  }
  build_menu_();
  // The planes stop being drawn under it.
  view::set_hidden(view_, true);
  ESP_LOGI(TAG, "Opened");
}

void Skyscope::build_menu_() {
  // In the colors of what is being chosen: another appearance or phosphor shows on the menu at once, and on the map
  // only with Save.
  const Chrome c = chrome_for_(menu_settings_);
  const uint32_t surface = colors::mix(c.ink, c.card, 20);  // the groups' cards, a shade off the ground
  const int tall = ui::mm(8);
  make_arrows(ui::mm(2.6f), std::max(2, ui::mm(0.55f)));
  menu_ = plain(lv_screen_active());
  lv_obj_set_size(menu_, width_, height_);
  lv_obj_set_style_bg_color(menu_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(menu_, LV_OPA_COVER, 0);
  ui::pad_page(menu_, ui::mm(1));
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
  // Where the same settings are in a browser, at the top, for anyone who would rather type them there.
  const std::string address = own_address();
  if (!address.empty()) {
    lv_obj_t *web = text(menu_, ui::fonts.small, c.muted, ("Or in a browser: http://" + address + "/").c_str());
    lv_obj_set_width(web, lv_pct(100));
    lv_obj_set_style_text_align(web, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(web, LV_LABEL_LONG_DOT);
  }

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

  // Keys side by side, each an equal share of a card's width.
  auto keys_of = [&](lv_obj_t *card) {
    lv_obj_t *keys = plain(card);
    lv_obj_set_size(keys, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(keys, ui::mm(1), 0);
    return keys;
  };
  lv_obj_t *card = nullptr, *box = nullptr;

  // The pages first: they are what a menu is most often opened for, and a swipe opens each too (wxpage.cpp,
  // todaypage.cpp). Not on a screen with no place yet, which has neither: its menu begins with the place.
  menu_weather_ = menu_today_ = nullptr;
  if (settings_.located) {
    card = group("Show", box);
    lv_obj_t *keys = keys_of(card);
    menu_weather_ = key_(keys, "Weather", MENU_WEATHER, 0, 0, tall);
    menu_today_ = key_(keys, "Today's planes", MENU_TODAY, 0, 0, tall);
    lv_obj_set_flex_grow(menu_weather_, 1);
    lv_obj_set_flex_grow(menu_today_, 1);
  }

  const auto &rows = menu::rows();
  menu_rows_.assign(rows.size(), nullptr);
  menu_values_.assign(rows.size(), nullptr);
  menu_groups_.assign(rows.size(), nullptr);
  menu_fields_.assign(rows.size(), nullptr);
  menu_switches_.assign(rows.size(), nullptr);
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
  // A switch (set_switch): a tap turns the row over, as either of a row's arrows would.
  auto switch_of = [&](lv_obj_t *row, int index) {
    lv_obj_t *track = key_(row, "", index, 1, ui::mm(SWITCH_WIDE_MM), ui::mm(SWITCH_TALL_MM));
    lv_obj_delete(lv_obj_get_child(track, 0));  // a key's face: a switch has a knob instead
    lv_obj_set_style_pad_all(track, ui::mm(SWITCH_PAD_MM), 0);
    lv_obj_set_ext_click_area(track, ui::mm(1));
    lv_obj_t *knob = plain(track);
    const int side = ui::mm(SWITCH_TALL_MM) - 2 * ui::mm(SWITCH_PAD_MM);
    lv_obj_set_size(knob, side, side);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    return track;
  };
  for (size_t i = 0; i < rows.size(); ++i) {
    if (const menu::Group *g = menu::group_starting(rows[i])) card = group(g->name, box);
    lv_obj_t *row = row_of(card, rows[i].name);
    // The value between its keys, whatever room they leave it: in a field where it is typed. One that is on or off
    // says so beside its switch.
    const bool steps = menu::steps(rows[i]) && !menu::switched(rows[i]);
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
    if (menu::switched(rows[i])) menu_switches_[i] = switch_of(row, (int) i);
    menu_rows_[i] = row;
    menu_values_[i] = value;
    menu_groups_[i] = box;
  }

  // The last group of rows is the network's: the Wi-Fi network the screen is on comes first in it, and its field
  // opens the networks in reach (editor.cpp).
  {
    lv_obj_t *row = row_of(card, "Wi-Fi");
    lv_obj_move_to_index(row, 0);
    menu_wifi_ = field_of(row, MENU_WIFI, 0);
    menu_wifi_value_ = lv_obj_get_child(menu_wifi_, 0);
  }

  // The map card, on a board with a slot: its lines, its space and its keys (refresh_menu_card_ fills them).
  menu_card_ = MenuCard{};
  if (card_fitted_) {
    card = group("Map card", box);
    menu_card_.box = box;
    auto line = [&](const char *name) {
      lv_obj_t *row = row_of(card, name);
      lv_obj_t *value = text(row, ui::fonts.label, c.ink, "");
      lv_obj_set_flex_grow(value, 1);
      lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
      menu_card_.shown.push_back(row);
      return value;
    };
    menu_card_.about = line("Card");
    menu_card_.bus = line("Bus");
    menu_card_.space = line("Space");
    // The bar: the free part is its ground, the other three drawn over it from the left.
    const uint32_t part_colors[4] = {c.accent, colors::mix(c.accent, surface, 120), colors::mix(c.ink, surface, 110),
                                     colors::mix(c.ink, surface, 40)};
    menu_card_.bar = plain(card);
    lv_obj_set_size(menu_card_.bar, lv_pct(100), ui::mm(3));
    lv_obj_set_style_radius(menu_card_.bar, ui::mm(1.5f), 0);
    lv_obj_set_style_clip_corner(menu_card_.bar, true, 0);
    lv_obj_set_style_bg_color(menu_card_.bar, lv_color_hex(part_colors[3]), 0);
    lv_obj_set_style_bg_opa(menu_card_.bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(menu_card_.bar, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 3; ++i) {
      menu_card_.parts[i] = plain(menu_card_.bar);
      lv_obj_set_size(menu_card_.parts[i], 0, lv_pct(100));
      lv_obj_set_style_bg_color(menu_card_.parts[i], lv_color_hex(part_colors[i]), 0);
      lv_obj_set_style_bg_opa(menu_card_.parts[i], LV_OPA_COVER, 0);
    }
    menu_card_.shown.push_back(menu_card_.bar);
    // Its legend under it: a swatch and words a part, wrapping onto a second line on a narrow glass.
    lv_obj_t *legend = plain(card);
    lv_obj_set_size(legend, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(legend, ui::mm(2), 0);
    lv_obj_set_style_pad_row(legend, ui::mm(0.4f), 0);
    for (int i = 0; i < 4; ++i) {
      lv_obj_t *item = plain(legend);
      lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(item, ui::mm(0.6f), 0);
      lv_obj_t *swatch = plain(item);
      lv_obj_set_size(swatch, ui::mm(1.8f), ui::mm(1.8f));
      lv_obj_set_style_radius(swatch, ui::mm(0.4f), 0);
      lv_obj_set_style_bg_color(swatch, lv_color_hex(part_colors[i]), 0);
      lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
      menu_card_.legend[i] = text(item, ui::fonts.small, c.muted, "");
      menu_card_.legend_items[i] = item;
    }
    menu_card_.shown.push_back(legend);
    menu_card_.kept = line("Kept");
    menu_card_.since = line("Since start");
    // A download under way, and its Stop.
    menu_card_.download_row = row_of(card, "Download");
    menu_card_.download = text(menu_card_.download_row, ui::fonts.label, c.ink, "");
    lv_obj_set_flex_grow(menu_card_.download, 1);
    lv_label_set_long_mode(menu_card_.download, LV_LABEL_LONG_WRAP);
    menu_card_.stop = key_(menu_card_.download_row, "Stop", MENU_CARD_STOP, 0, LV_SIZE_CONTENT, tall);
    // What it is doing, or why there is none.
    menu_card_.note = text(card, ui::fonts.label, c.muted, "");
    lv_obj_set_width(menu_card_.note, lv_pct(100));
    lv_obj_set_style_text_align(menu_card_.note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(menu_card_.note, LV_LABEL_LONG_WRAP);
    lv_obj_t *keys = keys_of(card);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(keys, ui::mm(0.6f), 0);
    menu_card_.mount = key_(keys, "Look again", MENU_CARD_MOUNT, 0, LV_SIZE_CONTENT, tall);
    menu_card_.clear = key_(keys, "Clear tiles", MENU_CARD_CLEAR, 0, LV_SIZE_CONTENT, tall);
    menu_card_.format = key_(keys, "Format", MENU_CARD_FORMAT, 0, LV_SIZE_CONTENT, tall);
    set_color(lv_obj_get_child(menu_card_.format, 0), colors::ERROR);
    for (lv_obj_t *k : {menu_card_.mount, menu_card_.clear, menu_card_.format}) lv_obj_set_flex_grow(k, 1);
  }

  // About: the version running and (with updates built in) the newest release, Check and Install; then the tour
  // again, a restart, and the factory reset, which ask first.
  card = group("About", box);
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
    lv_obj_t *keys = keys_of(card);
    menu_fw_check_ = key_(keys, "Check", MENU_CHECK, 0, 0, tall);
    lv_obj_set_flex_grow(menu_fw_check_, 1);
    menu_fw_install_ = key_(keys, "Install", MENU_INSTALL, 0, 0, tall, -1, true);
    lv_obj_set_flex_grow(menu_fw_install_, 2);
  }
#endif
  auto action = [&](const char *name, const char *label, int what) {
    lv_obj_t *field = field_of(row_of(card, name), what, 0);
    set_label(lv_obj_get_child(field, 0), label);
    return field;
  };
  menu_tour_ = action("Tour", "Take the tour", MENU_TOUR);
  menu_restart_ = action("Restart", "Restart the screen", MENU_RESTART);
  menu_reset_ = action("Start over", "Factory reset", MENU_RESET);
  set_color(lv_obj_get_child(menu_reset_, 0), colors::ERROR);

  // Where else they are set.
  std::string where = "Another way up restarts the screen when saved.";
  if (!address.empty()) where += " All of this is on the screen's web page too, at the address at the top.";
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
  // The card's bar is shared out by its width, known once the menu is laid out.
  if (menu_card_.bar) {
    lv_obj_update_layout(menu_);
    refresh_menu_card_();
  }
}

lv_obj_t *Skyscope::button_(lv_obj_t *parent, const char *label, int width, int height, int arrow_of, bool primary) {
  const Chrome c = chrome_for_(menu_settings_);
  // A primary key filled with the accent, its face in the ground's color; the others a quiet tint of the ink.
  const uint32_t face = primary ? c.card : c.ink;
  lv_obj_t *b = plain(parent);
  lv_obj_set_size(b, width, height);
  if (width == LV_SIZE_CONTENT) lv_obj_set_style_pad_hor(b, ui::mm(2.5f), 0);
  lv_obj_set_style_radius(b, height / 2, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? c.accent : c.ink), 0);
  lv_obj_set_style_bg_opa(b, primary ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_bg_opa(b, primary ? LV_OPA_70 : LV_OPA_50, LV_STATE_PRESSED);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *f = label ? text(b, ui::fonts.label, face, label) : drawn(b, arrow_of, face);
  lv_obj_center(f);
  return b;
}

lv_obj_t *Skyscope::key_(lv_obj_t *parent, const char *label, int row, int step, int width, int height, int arrow_of,
                         bool primary) {
  lv_obj_t *b = button_(parent, label, width, height, arrow_of, primary);
  // An action by its own (negative) id; a row's key by its row and which of its three keys it is.
  const int which = step < 0 ? STEP_BACK : step > 0 ? STEP_ON : EDIT;
  lv_obj_set_user_data(b, (void *) (intptr_t) (row < 0 ? row : row * 3 + which));
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
  // Other colors (the appearance, the phosphor, or the radar against a map): the menu built again in them, where it
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
  if (action == MENU_WEATHER) {
    open_weather_();
    return;
  }
  if (action == MENU_TODAY) {
    open_today_();
    return;
  }
  if (action == MENU_TOUR) {
    // The menu's changes kept, as by Save, and the tour from its beginning over the map.
    const Settings changed = menu_settings_;
    this->defer([this, changed]() {
      close_menu_();
      apply_menu_(changed);
      close_tour_();
      Settings s = settings_;
      s.tour = true;
      use_settings_(s);
    });
    return;
  }
  if (action == MENU_RESTART) {
    const Settings changed = menu_settings_;
    ask_("Restart the screen?", "It is back in a few seconds. The menu's changes are kept.", "Restart",
         [this, changed]() {
           close_menu_();
           apply_menu_(changed);
           ESP_LOGI(TAG, "Restarting, asked in the menu");
           this->set_timeout(400, []() { App.safe_reboot(); });
         });
    return;
  }
  if (action == MENU_CARD_MOUNT) {
    ask_card_(&Shared::mount_ask);
    menu_card_.looking_until = millis() + LOOKING_MS;
    refresh_menu_card_();
    return;
  }
  if (action == MENU_CARD_STOP) {
    ask_card_(&Shared::stop_ask);
    return;
  }
  if (action == MENU_CARD_CLEAR) {
    ask_("Clear the map tiles?",
         "Every map tile the screen keeps on the card is deleted, a few at a time while it has nothing else to do, and "
         "a download under way stops. Maps are fetched again as they are shown. The log of the planes seen and "
         "anything else on the card stay.",
         "Clear tiles", [this]() { ask_card_(&Shared::clear_ask); });
    return;
  }
  if (action == MENU_CARD_FORMAT) {
    ask_("Format the map card?",
         "Everything on the card is erased, not only the screen's tiles: the log of the planes seen and any other "
         "files too. The card is made FAT32, which takes some seconds.",
         "Format", [this]() { ask_card_(&Shared::format_ask); });
    return;
  }
  if (action == MENU_RESET) {
    ask_("Factory reset?",
         "Everything the screen keeps is erased: every setting and the place, the Wi-Fi networks, the web password, "
         "the day's tally and Home Assistant's key. It restarts as a new screen and opens its setup network. The map "
         "card is left as it is.",
         "Erase everything", [this]() { factory_reset_(); });
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

void Skyscope::apply_menu_(const Settings &menu) {
  // The tour is no row of the menu: whether it is still to be shown is as the screen has it now, not as it was when
  // the menu opened (the tour's last step is the menu itself).
  Settings changed = menu;
  changed.tour = settings_.tour;
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
    set_color(menu_values_[i], value != menu::shown(rows[i], settings_) ? c.accent : c.ink);
    if (menu_switches_[i])
      set_switch(menu_switches_[i], menu::text_of(rows[i], menu_settings_) == "true", c.accent, c.ink, c.card);
  }
  for (lv_obj_t *g : menu_groups_)
    set_hidden(g, std::find(shown_groups.begin(), shown_groups.end(), g) == shown_groups.end());
  refresh_menu_wifi_();
  refresh_menu_firmware_();
  refresh_menu_card_();
}

void Skyscope::ask_card_(uint32_t Shared::*ask) {
  std::lock_guard<std::mutex> guard(lock_);
  (shared_.*ask)++;
}

void Skyscope::refresh_menu_card_() {
  if (!menu_ || !menu_card_.box) return;
  Shared::Card k;
  {
    std::lock_guard<std::mutex> guard(lock_);
    k = shared_.card;
  }
  for (lv_obj_t *o : menu_card_.shown) set_hidden(o, !k.mounted);
  if (k.mounted) {
    set_label(menu_card_.about, card::about_words(k.about));
    std::string bus = card::bus_words(k.about);
    if (k.cluster) bus += "; FAT32, " + card::size_words(k.cluster) + " clusters";
    set_label(menu_card_.bus, bus);
    set_label(menu_card_.space, card::space_words(k.total, k.free));
    const card::Space space = card::space_of(k.total, k.free, k.tally);
    const uint64_t sizes[4] = {space.tiles, space.log, space.other, space.free};
    static const char *const NAMES[4] = {"Tiles", "Log", "Other", "Free"};
    const std::vector<int> wide =
        card::widths({space.tiles, space.log, space.other, space.free}, lv_obj_get_content_width(menu_card_.bar));
    for (int i = 0; i < 3; ++i)
      if (lv_obj_get_width(menu_card_.parts[i]) != wide[i]) lv_obj_set_width(menu_card_.parts[i], wide[i]);
    // Before the first count the tiles are not told from the rest: no tiles nor log, and the used part all other.
    for (int i = 0; i < 4; ++i) {
      set_hidden(menu_card_.legend_items[i], i == 1 && !sizes[i]);
      set_label(menu_card_.legend[i], std::string(NAMES[i]) + " " + card::size_words(sizes[i]));
    }
    set_label(menu_card_.kept, card::kept_words(k.tally));
    set_label(menu_card_.since, card::since_words(k.read, k.written, k.failed));
  }
  set_hidden(menu_card_.download_row, !k.downloading);
  if (k.downloading) set_label(menu_card_.download, card::download_words(k.area.style, k.next, k.total_tiles));
  // Look again: said for a few seconds, while the worker looks (it says nothing new when it finds none again).
  const bool looking = !k.mounted && menu_card_.looking_until && (int32_t) (millis() - menu_card_.looking_until) < 0;
  if (!looking) menu_card_.looking_until = 0;
  const std::string note = looking ? std::string("Looking for the card...")
                                   : card::note_words(k.mounted, k.error, k.formatting, k.clearing, k.cleared);
  set_label(menu_card_.note, note);
  set_hidden(menu_card_.note, note.empty());
  set_color(menu_card_.note, !k.mounted || (!k.error.empty() && !k.clearing) ? colors::ERROR
                                                                               : chrome_for_(menu_settings_).muted);
  set_hidden(menu_card_.mount, k.mounted || k.formatting || looking);
  set_hidden(menu_card_.clear, !k.mounted || k.clearing || k.formatting);
  set_hidden(menu_card_.format, k.formatting || (!k.mounted && !k.unformatted));
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
  set_color(menu_fw_line_, line.tone == firmware::BAD_NEWS ? colors::ERROR
                            : line.tone == firmware::GOOD_NEWS ? c.accent : c.muted);
  const bool armed = menu_install_armed_until_ && (int32_t) (now - menu_install_armed_until_) < 0;
  if (!armed) menu_install_armed_until_ = 0;
  set_hidden(menu_fw_install_, !f.available || f.installing);
  set_label(lv_obj_get_child(menu_fw_install_, 0), armed ? std::string("Tap again to install") : "Install " + f.latest);
  if (f.installing) lv_obj_add_state(menu_fw_check_, LV_STATE_DISABLED);
  else lv_obj_remove_state(menu_fw_check_, LV_STATE_DISABLED);
#endif
}

// A question over everything, in the menu's colors: what would happen, in words, with Cancel and the key that does it
// (in the error color: what is asked first is what cannot be undone). `then` runs after the page has gone.
void Skyscope::ask_(const std::string &title, const std::string &body, const char *yes, std::function<void()> &&then) {
  close_ask_();
  ask_then_ = std::move(then);
  ask_at_ = millis();
  const Chrome c = chrome_for_(menu_settings_);
  const int tall = ui::mm(8);
  ask_page_ = plain(lv_screen_active());
  lv_obj_set_size(ask_page_, width_, height_);
  lv_obj_set_style_bg_color(ask_page_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(ask_page_, LV_OPA_COVER, 0);
  ui::pad_page(ask_page_, ui::mm(2));
  lv_obj_set_flex_flow(ask_page_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(ask_page_, ui::mm(1.5f), 0);
  lv_obj_add_flag(ask_page_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched
  ask_title_ = text(ask_page_, ui::fonts.title, c.ink, title.c_str());
  lv_obj_set_width(ask_title_, lv_pct(100));
  lv_label_set_long_mode(ask_title_, LV_LABEL_LONG_WRAP);
  // The words in what room the title and the keys leave them, scrolling where a small glass has too little.
  lv_obj_t *words = lv_obj_create(ask_page_);
  lv_obj_remove_style_all(words);
  lv_obj_set_width(words, lv_pct(100));
  lv_obj_set_flex_grow(words, 1);
  lv_obj_add_flag(words, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(words, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(words, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_t *said = text(words, ui::fonts.label, c.muted, body.c_str());
  lv_obj_set_width(said, lv_pct(100));
  lv_label_set_long_mode(said, LV_LABEL_LONG_WRAP);
  lv_obj_t *keys = plain(ask_page_);
  lv_obj_set_size(keys, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(keys, ui::mm(1), 0);
  ask_no_ = button_(keys, "Cancel", 0, tall);
  lv_obj_set_flex_grow(ask_no_, 1);
  ask_yes_ = button_(keys, yes, 0, tall, -1, true);
  lv_obj_set_flex_grow(ask_yes_, 1);
  lv_obj_set_style_bg_color(ask_yes_, lv_color_hex(colors::ERROR), 0);
  // After the event: the key pressed is part of what goes.
  lv_obj_add_event_cb(ask_no_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (millis() - self->ask_at_ < ASK_DEAF_MS) return;
    self->defer([self]() { self->close_ask_(); });
  }, LV_EVENT_CLICKED, this);
  lv_obj_add_event_cb(ask_yes_, [](lv_event_t *e) {
    auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
    if (millis() - self->ask_at_ < ASK_DEAF_MS) return;
    self->defer([self]() {
      if (!self->ask_page_) return;
      const std::function<void()> then = std::move(self->ask_then_);
      self->close_ask_();
      if (then) then();
    });
  }, LV_EVENT_CLICKED, this);
  ESP_LOGI(TAG, "Asked: %s", title.c_str());
}

void Skyscope::close_ask_() {
  ask_then_ = nullptr;
  if (!ask_page_) return;
  lv_obj_delete(ask_page_);
  ask_page_ = ask_yes_ = ask_no_ = ask_title_ = nullptr;
}

void Skyscope::close_menu_() {
  if (!menu_) return;
  close_ask_();
  close_picker_();
  close_editor_();
  close_today_();
  close_weather_();
  lv_obj_delete(menu_);
  menu_ = nullptr;
  menu_list_ = nullptr;
  menu_rows_.clear();
  menu_values_.clear();
  menu_groups_.clear();
  menu_fields_.clear();
  menu_switches_.clear();
  menu_wifi_ = menu_wifi_value_ = nullptr;
  menu_weather_ = menu_today_ = menu_tour_ = menu_restart_ = menu_reset_ = nullptr;
  menu_card_ = MenuCard{};
  menu_fw_line_ = menu_fw_install_ = menu_fw_check_ = nullptr;
  menu_save_ = menu_cancel_ = menu_up_ = menu_down_ = nullptr;
  // The radar's scope clean again (its beam stopped where it was while the menu was open), then the planes and beam.
  if (shown_().map == RADAR && scope_background_ && canvas_buffer_) {
    std::memcpy(canvas_buffer_, scope_background_, (size_t) width_ * height_ * 2);
    lv_obj_invalidate(canvas_);
  }
  view::set_hidden(view_, false);
  if (view_ && planes_) lv_obj_invalidate(planes_);
  last_status_ = 0;
  ESP_LOGI(TAG, "Closed");
}
}  // namespace esphome::skyscope
