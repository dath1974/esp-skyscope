// The weather page: aviation weather for the place watched (weather.h), over everything on the glass. A swipe up from
// the strip along the bottom of the map opens it (swipe.cpp), and so does the menu's Weather group. Its tabs: the
// METAR of the station (set in the menu, or the nearest to the place that reports), in words under it; the TAF a line a
// change; the GFS MOS an hour a line; and over the area the SIGMETs, G-AIRMETs and pilot reports. The station's flight
// category shows in its color beside its name.
//
// The worker asks for the weather only while the page is shown: when it opens with nothing newer than REFRESH_MS, at
// Refresh, and every REFRESH_MS while it stays open.
#include "skyscope.h"

#include "colors.h"
#include "ui.h"
#include "widgets.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::skyscope {
static const char *const TAG = "skyscope.weather";
using namespace widgets;

namespace {
enum { TAB_METAR, TAB_TAF, TAB_MOS, TAB_AREA, TABS };
const char *const TAB_NAMES[TABS] = {"METAR", "TAF", "MOS", "Area"};
constexpr int WX_CLOSE = -1, WX_REFRESH = -2;
// The flight categories' colors, as the charts and the Aviation Weather Center draw them.
uint32_t category_color(weather::Category c, uint32_t otherwise) {
  switch (c) {
    case weather::VFR: return 0x2ECC40;
    case weather::MVFR: return 0x3D8BFF;
    case weather::IFR: return 0xFF4136;
    case weather::LIFR: return 0xE040E0;
    default: return otherwise;
  }
}
}  // namespace

int64_t Skyscope::unix_now_() const {
#ifdef USE_TIME
  if (clock_) {
    const ESPTime t = clock_->now();
    if (t.is_valid()) return t.timestamp;
  }
#endif
  return 0;
}

void Skyscope::open_weather_() {
  if (wx_) return;
  menu_settings_ = menu_ ? menu_settings_ : settings_;  // the keys' colors (menu.cpp button_)
  const Chrome c = chrome_for_(menu_settings_);
  const int tall = ui::mm(8);
  wx_ = plain(lv_screen_active());
  lv_obj_set_size(wx_, width_, height_);
  lv_obj_set_style_bg_color(wx_, lv_color_hex(c.card), 0);
  lv_obj_set_style_bg_opa(wx_, LV_OPA_COVER, 0);
  ui::pad_page(wx_, ui::mm(1));
  lv_obj_set_flex_flow(wx_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wx_, ui::mm(0.8f), 0);
  lv_obj_add_flag(wx_, LV_OBJ_FLAG_CLICKABLE);  // nothing under it is touched

  auto key = [&](lv_obj_t *parent, const char *label, int what, bool primary) {
    lv_obj_t *b = button_(parent, label, LV_SIZE_CONTENT, tall, -1, primary);
    lv_obj_set_user_data(b, (void *) (intptr_t) what);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      auto *self = static_cast<Skyscope *>(lv_event_get_user_data(e));
      const int what = (int) (intptr_t) lv_obj_get_user_data(lv_event_get_current_target_obj(e));
      self->defer([self, what]() {
        if (what == WX_CLOSE) self->close_weather_();
        else if (what == WX_REFRESH) self->ask_weather_();
        else self->weather_tab_(what);
      });
    }, LV_EVENT_CLICKED, this);
    return b;
  };
  // Close, the station and its category, and Refresh.
  lv_obj_t *head = plain(wx_);
  lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(head, ui::mm(1), 0);
  key(head, "Close", WX_CLOSE, false);
  wx_title_ = text(head, ui::fonts.title, c.ink, "Weather");
  lv_obj_set_flex_grow(wx_title_, 1);
  lv_obj_set_style_text_align(wx_title_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(wx_title_, LV_LABEL_LONG_DOT);
  key(head, "Refresh", WX_REFRESH, false);
  // The tabs.
  lv_obj_t *tabs = plain(wx_);
  lv_obj_set_size(tabs, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(tabs, ui::mm(0.8f), 0);
  for (int t = 0; t < TABS; ++t) {
    wx_tabs_[t] = key(tabs, TAB_NAMES[t], t, false);
    lv_obj_set_width(wx_tabs_[t], 0);
    lv_obj_set_flex_grow(wx_tabs_[t], 1);
  }
  // When it was, or what is happening.
  wx_line_ = text(wx_, ui::fonts.small, c.muted, "");
  lv_obj_set_width(wx_line_, lv_pct(100));
  lv_label_set_long_mode(wx_line_, LV_LABEL_LONG_WRAP);
  // What the tab shows, scrolling.
  wx_list_ = lv_obj_create(wx_);
  lv_obj_remove_style_all(wx_list_);
  lv_obj_set_width(wx_list_, lv_pct(100));
  lv_obj_set_flex_grow(wx_list_, 1);
  lv_obj_set_flex_flow(wx_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wx_list_, ui::mm(0.6f), 0);
  lv_obj_add_flag(wx_list_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(wx_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(wx_list_, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_t *credit = text(wx_, ui::fonts.small, c.muted,
                          "aviationweather.gov, Iowa Environmental Mesonet (MOS). Not for flight planning.");
  lv_obj_set_width(credit, lv_pct(100));
  lv_label_set_long_mode(credit, LV_LABEL_LONG_DOT);

  // The report kept if it is new enough, else another asked for.
  if (!wx_report_ || millis() - wx_at_ > weather::REFRESH_MS) ask_weather_();
  weather_tab_(wx_tab_);
  ESP_LOGD(TAG, "Opened");
}

void Skyscope::ask_weather_() {
  std::lock_guard<std::mutex> guard(lock_);
  wx_asked_ = ++shared_.weather_ask;
}

void Skyscope::weather_tab_(int tab) {
  if (!wx_ || tab < 0 || tab >= TABS) return;
  wx_tab_ = tab;
  const Chrome c = chrome_for_(menu_settings_);
  for (int t = 0; t < TABS; ++t) {
    // The tab shown filled with the accent, as a primary key.
    lv_obj_set_style_bg_color(wx_tabs_[t], lv_color_hex(t == tab ? c.accent : c.ink), 0);
    lv_obj_set_style_bg_opa(wx_tabs_[t], t == tab ? LV_OPA_COVER : LV_OPA_20, 0);
    set_color(lv_obj_get_child(wx_tabs_[t], 0), t == tab ? c.card : c.ink);
  }
  fill_weather_();
}

void Skyscope::fill_weather_() {
  if (!wx_) return;
  const Chrome c = chrome_for_(menu_settings_);
  lv_obj_clean(wx_list_);
  lv_obj_scroll_to_y(wx_list_, 0, LV_ANIM_OFF);
  auto line = [&](const std::string &s, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = text(wx_list_, font, color, s.c_str());
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
  };
  const weather::Report *r = wx_report_.get();
  // The station and its category in the title.
  std::string title = "Weather";
  uint32_t title_color = c.ink;
  if (r && !r->station.empty()) {
    title = r->station;
    if (r->has_metar && r->metar.category != weather::UNKNOWN) {
      title += std::string("  ") + weather::CATEGORY_NAMES[r->metar.category];
      title_color = category_color(r->metar.category, c.ink);
    }
  }
  set_label(wx_title_, title);
  set_color(wx_title_, title_color);
  if (!r) {
    line("Asking the Aviation Weather Center...", ui::fonts.label, c.muted);
    return;
  }
  const int64_t now = unix_now_();
  switch (wx_tab_) {
    case TAB_METAR:
      if (!r->has_metar) {
        line("No METAR", ui::fonts.label, c.muted);
        break;
      }
      if (!r->metar.name.empty()) line(r->metar.name, ui::fonts.small, c.muted);
      line(r->metar.raw, ui::fonts.label, c.ink);
      for (const std::string &l : weather::decoded(r->metar, settings_.metric)) line(l, ui::fonts.label, c.muted);
      break;
    case TAB_TAF:
      if (r->taf.raw.empty()) {
        line("No TAF for " + (r->station.empty() ? std::string("the station") : r->station), ui::fonts.label, c.muted);
        break;
      }
      if (r->taf.issued) line("Issued " + weather::when(r->taf.issued, now), ui::fonts.small, c.muted);
      for (const std::string &l : weather::taf_lines(r->taf.raw)) line(l, ui::fonts.label, c.ink);
      break;
    case TAB_MOS:
      if (r->mos.empty()) {
        line("No MOS for " + (r->station.empty() ? std::string("the station") : r->station) +
                 " (the GFS MOS is for stations in the United States)",
             ui::fonts.label, c.muted);
        break;
      }
      line("GFS MOS: hour, temperature/dew point, wind, sky, category", ui::fonts.small, c.muted);
      for (const weather::MosRow &m : r->mos)
        line(weather::mos_line(m), ui::fonts.label, category_color(weather::category_of(m), c.ink));
      break;
    case TAB_AREA:
      if (r->advisories.empty()) line("No SIGMET or G-AIRMET over the area", ui::fonts.label, c.muted);
      for (const weather::Advisory &a : r->advisories) line(weather::advisory_line(a), ui::fonts.label, c.ink);
      if (r->pireps.empty()) {
        line("No pilot reports in the last two hours", ui::fonts.label, c.muted);
      } else {
        line("Pilot reports, the latest first", ui::fonts.small, c.accent);
        for (size_t i = 0; i < r->pireps.size() && i < 12; ++i) {
          line(weather::when(r->pireps[i].observed, now), ui::fonts.small, c.muted);
          line(r->pireps[i].raw, ui::fonts.label, c.ink);
        }
      }
      break;
  }
}

void Skyscope::refresh_weather_() {
  if (!wx_) return;
  bool answered, fresh = false;
  {
    std::lock_guard<std::mutex> guard(lock_);
    answered = shared_.weather_rev == wx_asked_;
    if (answered && shared_.weather && shared_.weather != wx_report_) {
      wx_report_ = shared_.weather;
      wx_at_ = millis();
      fresh = true;
    }
  }
  if (fresh) fill_weather_();
  // Again while it stays open, as the METARs change.
  if (answered && wx_report_ && millis() - wx_at_ > weather::REFRESH_MS) {
    ask_weather_();
    answered = false;
  }
  const bool asking = !answered;
  std::string text;
  if (wx_report_ && wx_report_->has_metar) text = "Observed " + weather::when(wx_report_->metar.observed, unix_now_());
  if (wx_report_ && !wx_report_->error.empty()) text += (text.empty() ? "" : ". ") + wx_report_->error;
  if (asking) text += (text.empty() ? "" : ". ") + std::string("Asking...");
  set_label(wx_line_, text);
}

void Skyscope::close_weather_() {
  if (!wx_) return;
  lv_obj_delete(wx_);
  wx_ = wx_title_ = wx_line_ = wx_list_ = nullptr;
  for (auto &t : wx_tabs_) t = nullptr;
  ESP_LOGD(TAG, "Closed");
}
}  // namespace esphome::skyscope
