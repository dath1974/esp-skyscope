// The menu's keyboard, and the lists it picks from, over the menu (menu.cpp opens them).
//
// - The keyboard (keyboard.h) types a row's value, which the settings check when Done is pressed: the row's hint shows
//   under the text, in the error colour when they refuse it. It also types a place to find, which the worker looks for
//   (places.h) and a list then offers, and a Wi-Fi network's name and password.
// - The list offers the places a search found, or the Wi-Fi networks in reach. While the screen is on a network it
//   scans for them itself; while it is not, ESPHome scans as it looks for one, and each of its scans comes here too.
//   Joining a network keeps the menu's changes, saves the network and restarts the screen to join it: ESPHome changes
//   networks only when it loses the one it is on. A network it cannot join opens the setup network after a minute, as
//   at the first start.
#include "skyscope.h"

#include "colours.h"
#include "menu_rows.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/components/wifi/scan_list.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_wifi.h>

#include <algorithm>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.editor";
using namespace widgets;

namespace {
constexpr uint32_t SCAN_MS = 10000;  // a scan unanswered for this long is waited for no more
constexpr int PICK_OTHER = -1;       // the networks' last item: one typed by its name
constexpr size_t MOST_NETWORKS = 12;

// A panel over everything, the size of the glass, in the menu's colours.
lv_obj_t *panel(int width, int height, uint32_t ground) {
  lv_obj_t *p = plain(lv_screen_active());
  lv_obj_set_size(p, width, height);
  lv_obj_set_style_bg_color(p, lv_color_hex(ground), 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(p, ui::mm(1), 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(p, ui::mm(0.8f), 0);
  lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched
  return p;
}
lv_obj_t *head_of(lv_obj_t *parent) {
  lv_obj_t *head = plain(parent);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(head, ui::mm(1), 0);
  return head;
}
lv_obj_t *title_of(lv_obj_t *head, uint32_t ink, const std::string &title) {
  lv_obj_t *t = text(head, ui::fonts.title, ink, title.c_str());
  lv_obj_set_flex_grow(t, 1);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  return t;
}
template<typename F> void on_click(lv_obj_t *o, void *self, F f) {
  lv_obj_add_event_cb(o, f, LV_EVENT_CLICKED, self);
}
Skyscope *self_of(lv_event_t *e) { return static_cast<Skyscope *>(lv_event_get_user_data(e)); }
int index_of(lv_event_t *e) { return (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target_obj(e)); }
}  // namespace

// ---------------------------------------------------------------- the keyboard

void Skyscope::open_row_editor_(int index) {
  const auto &rows = menu::rows();
  if (index < 0 || index >= (int) rows.size() || !menu::typed(rows[index])) return;
  const menu::Row &row = rows[index];
  typing_row_ = index;
  if (row.kind == menu::PLACE) {
    open_editor_(TYPING_PLACE, "Find a place", menu_settings_.place, row.pad, menu::most_typed(row), row.hint);
  } else {
    open_editor_(TYPING_ROW, row.name, menu::text_of(row, menu_settings_), row.pad, menu::most_typed(row), row.hint);
  }
}

void Skyscope::open_editor_(Typing what, const std::string &title, const std::string &text, keyboard::Pad pad,
                            size_t most, const std::string &hint) {
  close_editor_();
  typing_ = what;
  board_.start(pad, text, most);
  editor_hint_text_ = hint;
  editor_error_.clear();
  editor_masked_ = what == TYPING_PASSWORD;
  const Chrome c = chrome_for_(menu_settings_);
  const uint32_t surface = colours::mix(c.ink, c.card, 20);
  const int tall = ui::mm(8);
  editor_ = panel(width_, height_, c.card);

  // Cancel, what is typed, and Done (or what Done does next).
  lv_obj_t *head = head_of(editor_);
  lv_obj_t *cancel = button_(head, "Cancel", LV_SIZE_CONTENT, tall);
  on_click(cancel, this, [](lv_event_t *e) {
    Skyscope *self = self_of(e);
    self->defer([self]() { self->close_editor_(); });
  });
  editor_title_ = title_of(head, c.ink, title);
  const char *done = what == TYPING_PLACE ? "Search" : what == TYPING_NETWORK ? "Next" : what == TYPING_PASSWORD ? "Join"
                                                                                                                  : "Done";
  editor_done_key_ = button_(head, done, LV_SIZE_CONTENT, tall, -1, true);
  on_click(editor_done_key_, this, [](lv_event_t *e) { self_of(e)->editor_done_(); });

  // The text, with its cursor: two lines of it for words, which scroll to keep the cursor in sight.
  const int line = lv_font_get_line_height(ui::fonts.label);
  editor_field_ = plain(editor_);
  lv_obj_set_size(editor_field_, lv_pct(100), line * (pad == keyboard::WORDS ? 2 : 1) + 2 * ui::mm(1));
  lv_obj_set_style_bg_color(editor_field_, lv_color_hex(surface), 0);
  lv_obj_set_style_bg_opa(editor_field_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(editor_field_, ui::mm(1.2f), 0);
  lv_obj_set_style_pad_all(editor_field_, ui::mm(1), 0);
  lv_obj_add_flag(editor_field_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(editor_field_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(editor_field_, LV_SCROLLBAR_MODE_OFF);
  editor_text_ = widgets::text(editor_field_, ui::fonts.label, c.ink, "");
  lv_obj_set_width(editor_text_, lv_pct(100));
  lv_label_set_long_mode(editor_text_, LV_LABEL_LONG_WRAP);
  editor_cursor_ = plain(editor_field_);
  lv_obj_set_size(editor_cursor_, std::max(2, ui::mm(0.35f)), line);
  lv_obj_set_style_bg_color(editor_cursor_, lv_color_hex(c.accent), 0);
  lv_obj_set_style_bg_opa(editor_cursor_, LV_OPA_COVER, 0);

  // What the value may be (or why it was refused), and for a password a key to show it.
  lv_obj_t *under = plain(editor_);
  lv_obj_set_size(under, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(under, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(under, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(under, ui::mm(1), 0);
  editor_hint_ = widgets::text(under, ui::fonts.small, c.muted, "");
  lv_obj_set_flex_grow(editor_hint_, 1);
  lv_label_set_long_mode(editor_hint_, LV_LABEL_LONG_WRAP);
  editor_show_ = nullptr;
  if (what == TYPING_PASSWORD) {
    editor_show_ = button_(under, "Show", LV_SIZE_CONTENT, ui::mm(6));
    on_click(editor_show_, this, [](lv_event_t *e) {
      Skyscope *self = self_of(e);
      self->editor_masked_ = !self->editor_masked_;
      set_label(lv_obj_get_child(self->editor_show_, 0), self->editor_masked_ ? "Show" : "Hide");
      self->refresh_editor_();
    });
  }

  // The keys, in what room is left.
  editor_keys_ = plain(editor_);
  lv_obj_set_width(editor_keys_, lv_pct(100));
  lv_obj_set_flex_grow(editor_keys_, 1);
  lv_obj_set_flex_flow(editor_keys_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(editor_keys_, ui::mm(0.6f), 0);
  build_keys_();
  refresh_editor_();
  ESP_LOGD(TAG, "Typing %s", title.c_str());
}

void Skyscope::build_keys_() {
  if (!editor_) return;
  lv_obj_clean(editor_keys_);
  board_keys_.clear();
  board_key_objs_.clear();
  const auto rows = board_.keys();
  // Each row an equal share of the room the keyboard has, whatever the hint over it takes (it wraps, and a value
  // refused says more): shared again by the layout whenever that changes, so the last row is never cut off.
  const int gap = ui::mm(0.6f);
  for (const auto &row : rows) {
    lv_obj_t *line = plain(editor_keys_);
    lv_obj_set_width(line, lv_pct(100));
    lv_obj_set_flex_grow(line, 1);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(line, gap, 0);
    int width = 0;
    for (const auto &k : row) width += k.width;
    // A short row in the middle, as on any keyboard: an empty share at each end.
    auto spacer = [&]() {
      lv_obj_t *s = plain(line);
      lv_obj_set_size(s, 0, 1);
      lv_obj_set_flex_grow(s, (keyboard::ROW_WIDTH - width) / 2);
    };
    if (width < keyboard::ROW_WIDTH) spacer();
    for (const auto &k : row) {
      const bool drawn = k.action == keyboard::LEFT || k.action == keyboard::RIGHT;
      const int arrow_of = k.action == keyboard::LEFT ? ARROW_LEFT : ARROW_RIGHT;
      const std::string face = board_.face(k);
      lv_obj_t *b = button_(line, drawn ? nullptr : face.c_str(), 0, ui::mm(4), drawn ? arrow_of : -1,
                            k.action == keyboard::SHIFT && board_.caps);
      lv_obj_set_height(b, lv_pct(100));
      lv_obj_set_flex_grow(b, k.width);
      lv_obj_set_style_radius(b, ui::mm(1), 0);
      lv_obj_set_user_data(b, (void *) (intptr_t) board_keys_.size());
      on_click(b, this, [](lv_event_t *e) { self_of(e)->press_key_(index_of(e)); });
      board_keys_.push_back(k);
      board_key_objs_.push_back(b);
    }
    if (width < keyboard::ROW_WIDTH) spacer();
  }
}

void Skyscope::press_key_(int index) {
  if (!editor_ || index < 0 || index >= (int) board_keys_.size()) return;
  editor_error_.clear();
  // Another page or the shift: the keys built again after this event, as the key pressed is one of them.
  if (board_.press(board_keys_[index])) this->defer([this]() { build_keys_(); });
  refresh_editor_();
}

void Skyscope::refresh_editor_() {
  if (!editor_) return;
  const Chrome c = chrome_for_(menu_settings_);
  const std::string &t = board_.editor.text;
  set_label(editor_text_, editor_masked_ ? std::string(keyboard::chars_in(t, t.size()), '*') : t);
  // The cursor before the letter it is at: LVGL counts letters, not bytes.
  lv_obj_update_layout(editor_field_);
  lv_point_t at;
  lv_label_get_letter_pos(editor_text_, keyboard::chars_in(t, board_.editor.cursor), &at);
  lv_obj_set_pos(editor_cursor_, lv_obj_get_x(editor_text_) + at.x, lv_obj_get_y(editor_text_) + at.y);
  lv_obj_scroll_to_view(editor_cursor_, LV_ANIM_OFF);
  set_label(editor_hint_, editor_error_.empty() ? editor_hint_text_ : editor_error_);
  set_colour(editor_hint_, editor_error_.empty() ? c.muted : colours::ERROR);
}

void Skyscope::editor_done_() {
  if (!editor_) return;
  const std::string text = board_.editor.text;
  switch (typing_) {
    case TYPING_ROW: {
      const auto &rows = menu::rows();
      if (typing_row_ < 0 || typing_row_ >= (int) rows.size()) return;
      if (!menu::typed_in(rows[typing_row_], menu_settings_, text)) {
        editor_error_ = "The screen cannot take that. " + editor_hint_text_;
        refresh_editor_();
        return;
      }
      // Closed after this event: the key pressed is part of what goes.
      this->defer([this]() {
        close_editor_();
        refresh_menu_();
      });
      return;
    }
    case TYPING_PLACE: {
      const std::string q = keyboard::trimmed(text);
      if (q.empty()) return;
      {
        std::lock_guard<std::mutex> guard(lock_);
        shared_.search = q;
        search_rev_ = ++shared_.search_rev;
      }
      searching_ = true;
      found_.clear();
      found_error_.clear();
      this->defer([this]() { open_picker_(PICKING_PLACES); });
      return;
    }
    case TYPING_NETWORK:
      if (const char *why = menu::network_refused(text, "")) {
        editor_error_ = why;
        refresh_editor_();
        return;
      }
      network_name_ = text;
      this->defer([this]() {
        open_editor_(TYPING_PASSWORD, "Password", "", keyboard::WORDS, 64,
                     "For \"" + network_name_ + "\", none for an open network. The screen restarts to join it.");
      });
      return;
    case TYPING_PASSWORD:
      if (const char *why = menu::network_refused(network_name_, text)) {
        editor_error_ = why;
        refresh_editor_();
        return;
      }
      join_network_(network_name_, text);
      return;
  }
}

void Skyscope::close_editor_() {
  if (!editor_) return;
  lv_obj_delete(editor_);
  editor_ = editor_title_ = editor_field_ = editor_text_ = editor_cursor_ = nullptr;
  editor_hint_ = editor_keys_ = editor_done_key_ = editor_show_ = nullptr;
  board_keys_.clear();
  board_key_objs_.clear();
}

// ---------------------------------------------------------------- the lists

void Skyscope::open_picker_(Picking what) {
  close_picker_();
  picking_ = what;
  const Chrome c = chrome_for_(menu_settings_);
  const int tall = ui::mm(8);
  picker_ = panel(width_, height_, c.card);

  // Back, what is listed, and for the networks Scan.
  lv_obj_t *head = head_of(picker_);
  picker_back_ = button_(head, "Back", LV_SIZE_CONTENT, tall);
  on_click(picker_back_, this, [](lv_event_t *e) {
    Skyscope *self = self_of(e);
    self->defer([self]() { self->close_picker_(); });
  });
  title_of(head, c.ink, what == PICKING_PLACES ? "Places found" : "Wi-Fi");
  picker_scan_ = nullptr;
  if (what == PICKING_NETWORKS) {
    picker_scan_ = button_(head, "Scan", LV_SIZE_CONTENT, tall);
    on_click(picker_scan_, this, [](lv_event_t *e) { self_of(e)->scan_networks_(); });
  }
  picker_line_ = widgets::text(picker_, ui::fonts.label, c.muted, "");
  lv_obj_set_width(picker_line_, lv_pct(100));
  lv_label_set_long_mode(picker_line_, LV_LABEL_LONG_WRAP);

  picker_list_ = lv_obj_create(picker_);
  lv_obj_remove_style_all(picker_list_);
  lv_obj_set_width(picker_list_, lv_pct(100));
  lv_obj_set_flex_grow(picker_list_, 1);
  lv_obj_set_flex_flow(picker_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(picker_list_, ui::mm(0.8f), 0);
  lv_obj_add_flag(picker_list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(picker_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(picker_list_, LV_SCROLLBAR_MODE_ACTIVE);

  if (what == PICKING_PLACES) {
    // Nominatim's terms: OpenStreetMap credited wherever its places are shown.
    lv_obj_t *credit = widgets::text(picker_, ui::fonts.small, c.muted,
                                     "Airports from the FAA, places from OpenStreetMap (\xC2\xA9 OpenStreetMap contributors)");
    lv_obj_set_width(credit, lv_pct(100));
    lv_label_set_long_mode(credit, LV_LABEL_LONG_WRAP);
  } else {
    // What ESPHome found last at once, then a scan of its own.
    if (wifi::global_wifi_component) networks_from_(wifi::global_wifi_component->get_scan_result());
    scan_networks_();
  }
  fill_picker_();
  refresh_picker_();
}

void Skyscope::fill_picker_() {
  if (!picker_) return;
  const Chrome c = chrome_for_(menu_settings_);
  lv_obj_clean(picker_list_);
  // An item: its name over what more there is to say of it, all of it a key.
  auto item = [&](const std::string &name, const std::string &detail, int index) {
    lv_obj_t *b = plain(picker_list_);
    lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, ui::mm(1.2f), 0);
    lv_obj_set_style_pad_all(b, ui::mm(1), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(c.ink), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *n = widgets::text(b, ui::fonts.label, c.ink, name.c_str());
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    if (!detail.empty()) {
      lv_obj_t *d = widgets::text(b, ui::fonts.small, c.muted, detail.c_str());
      lv_obj_set_width(d, lv_pct(100));
      lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
      lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(b, (void *) (intptr_t) index);
    on_click(b, this, [](lv_event_t *e) { self_of(e)->picker_pick_(index_of(e)); });
  };
  if (picking_ == PICKING_PLACES) {
    for (size_t i = 0; i < found_.size(); ++i)
      item(found_[i].name, found_[i].detail + " (" + found_[i].source + ")", (int) i);
    return;
  }
  std::string on;
  auto *wifi = wifi::global_wifi_component;
  if (wifi && wifi->is_connected()) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    on = wifi->wifi_ssid_to(ssid);
  }
  for (size_t i = 0; i < networks_.size(); ++i) {
    const Network &n = networks_[i];
    item(n.name,
         std::to_string(n.rssi) + " dBm, " + (n.secured ? "password" : "open") + (n.name == on ? ", connected" : ""),
         (int) i);
  }
  item("Other network", "A hidden one, by its name", PICK_OTHER);
}

void Skyscope::refresh_picker_() {
  if (!picker_) return;
  if (picking_ == PICKING_PLACES) {
    if (searching_) {
      std::string q;
      {
        std::lock_guard<std::mutex> guard(lock_);
        q = shared_.search;
        if (shared_.found_rev == search_rev_) {
          found_ = shared_.found;
          found_error_ = shared_.search_error;
          searching_ = false;
        }
      }
      if (!searching_) fill_picker_();
      else set_label(picker_line_, "Looking for \"" + q + "\"...");
    }
    if (!searching_)
      set_label(picker_line_, !found_error_.empty() ? found_error_ : found_.empty() ? "Nothing found" : "Pick one:");
    return;
  }
  // The network the screen is on, and whether a scan is under way.
  const uint32_t now = millis();
  if (scanning_until_ && (int32_t) (now - scanning_until_) >= 0) scanning_until_ = 0;
  std::string line = "Not on a network.";
  auto *wifi = wifi::global_wifi_component;
  if (wifi && wifi->is_connected()) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    line = std::string("On \"") + wifi->wifi_ssid_to(ssid) + "\", " + std::to_string(wifi->wifi_rssi()) + " dBm";
    const std::string address = own_address();
    line += address.empty() ? "." : ", " + address + ".";
  }
  if (scanning_until_) line += " Looking for networks...";
  else if (networks_.empty()) line += " No networks found yet.";
  set_label(picker_line_, line);
}

void Skyscope::picker_pick_(int index) {
  if (!picker_) return;
  if (picking_ == PICKING_PLACES) {
    if (index < 0 || index >= (int) found_.size()) return;
    const places::Found &p = found_[index];
    JsonDocument change;
    change["lat"] = p.lat;
    change["lon"] = p.lon;
    change["place"] = p.name;
    std::string error;
    if (!apply(change.as<JsonObjectConst>(), menu_settings_, error)) {
      found_error_ = "The map cannot show a place that far north or south.";
      refresh_picker_();
      return;
    }
    // Back to the menu, the place in the accent until Save. After this event: the item pressed is part of what goes.
    this->defer([this]() {
      close_picker_();
      close_editor_();
      refresh_menu_();
    });
    return;
  }
  if (index == PICK_OTHER) {
    this->defer([this]() {
      open_editor_(TYPING_NETWORK, "Network name", "", keyboard::WORDS, 32,
                   "The network's name (its SSID), as it is written");
    });
    return;
  }
  if (index < 0 || index >= (int) networks_.size()) return;
  network_name_ = networks_[index].name;
  const bool secured = networks_[index].secured;
  this->defer([this, secured]() {
    open_editor_(TYPING_PASSWORD, "Password", "", keyboard::WORDS, 64,
                 "For \"" + network_name_ + "\"" + (secured ? "" : ", an open network: none") +
                     ". The screen restarts to join it.");
  });
}

void Skyscope::close_picker_() {
  if (!picker_) return;
  lv_obj_delete(picker_);
  picker_ = picker_back_ = picker_line_ = picker_list_ = picker_scan_ = nullptr;
  networks_.clear();
  networks_.shrink_to_fit();
  scanning_until_ = 0;
}

// ---------------------------------------------------------------- Wi-Fi

void Skyscope::scan_networks_() {
  auto *wifi = wifi::global_wifi_component;
  if (!wifi) return;
  scanning_until_ = millis() + SCAN_MS;
  // On a network ESPHome scans only for a stronger signal of its own, so the radio is asked here. ESPHome is told of
  // every scan that is done, keeps what it found (__init__.py asks it to) and hands it to on_wifi_scan_results. Off a
  // network ESPHome scans as it looks for one, and its next scan comes here the same way.
  if (wifi->is_connected()) {
    wifi_scan_config_t config{};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 300;
    const esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "No scan: %s", esp_err_to_name(err));
      scanning_until_ = 0;
    }
  }
  refresh_picker_();
}

void Skyscope::on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  if (!picker_ || picking_ != PICKING_NETWORKS) return;
  networks_from_(results);
  scanning_until_ = 0;
  fill_picker_();
  refresh_picker_();
}

void Skyscope::networks_from_(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  // Each network once, by its strongest signal, the strongest first; none without a name.
  networks_.clear();
  for (const auto &scan : results) {
    bool secured = false;
    if (!wifi::should_show_scan_entry(results, scan, secured)) continue;
    const StringRef ssid = scan.get_ssid();
    if (ssid.empty()) continue;
    networks_.push_back({std::string(ssid.c_str(), ssid.size()), scan.get_rssi(), secured});
  }
  std::sort(networks_.begin(), networks_.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  if (networks_.size() > MOST_NETWORKS) networks_.resize(MOST_NETWORKS);
}

void Skyscope::join_network_(const std::string &name, const std::string &password) {
  const Settings changed = menu_settings_;
  // After this event, as everything on the glass goes: the menu's changes kept and stored at once, then the network,
  // then a restart into it.
  this->defer([this, changed, name, password]() {
    close_menu_();
    apply_menu_(changed);
    if (save_pending_) save_settings_();
    joining_ = name;
    last_status_ = 0;
    ESP_LOGI(TAG, "A new Wi-Fi network: restarting to join it");
    if (wifi::global_wifi_component) wifi::global_wifi_component->save_wifi_sta(name, password);
    this->set_timeout(1500, []() { App.safe_reboot(); });
  });
}
}  // namespace esphome::skyscope
