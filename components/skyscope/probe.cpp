// Tests on a real screen without a finger (tests/device_test.py), built in unless `test_api: false`:
//
//   POST /api/touch   {"gesture": "tap" | "hold" | "drag", "x", "y", ["x1", "y1"], ["ms"]}: a touch on the glass,
//                     at the screen's own coordinates (as LVGL takes them, the rotation applied); answers how long
//                     it takes
//   GET  /api/ui      what the glass shows: the status line, the plane's card, the brightness level, the menu (its
//                     rows, values and keys, where each key is), its keyboard (the text, the hint and every key) and
//                     the list over it (its line and items), and where each plane is, to touch them by
//
// A touch goes in as a second input device beside the touch panel: LVGL reads it as it reads a finger, so the same
// objects get the same events (a long press, a drag along the brightness strip, a click on a key). While the screen
// is asleep a touch wakes it first, as a finger does (lvgl: resume_on_input). /api/ui is read on the loop, which owns
// LVGL; the web server's task waits for it.
#include "skyscope.h"

#ifdef SKYSCOPE_TEST_API
#include "menu_rows.h"
#include "touch.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <freertos/semphr.h>

#include <memory>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.probe";

namespace {
// The middle of an object on the glass, and whether a finger there reaches it: shown, and its middle inside every
// parent (a row scrolled out of the menu's list is not).
void place(JsonObject out, lv_obj_t *o) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
  out["x"] = x;
  out["y"] = y;
  bool reachable = !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
  for (lv_obj_t *p = lv_obj_get_parent(o); p && reachable; p = lv_obj_get_parent(p)) {
    lv_area_t pa;
    lv_obj_get_coords(p, &pa);
    reachable = !lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN) && x >= pa.x1 && x <= pa.x2 && y >= pa.y1 && y <= pa.y2;
  }
  out["reachable"] = reachable;
}
bool shown(lv_obj_t *o) { return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }
const char *text_of(lv_obj_t *label) { return label ? lv_label_get_text(label) : ""; }
}  // namespace

void Skyscope::make_probe_input_() {
  probe_input_ = lv_indev_create();
  lv_indev_set_type(probe_input_, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(probe_input_, lv_display_get_default());
  lv_indev_set_user_data(probe_input_, this);
  lv_indev_set_read_cb(probe_input_, [](lv_indev_t *indev, lv_indev_data_t *data) {
    auto *self = static_cast<Skyscope *>(lv_indev_get_user_data(indev));
    const touch::Step s = self->probe_touch_.at(millis());
    data->point.x = s.x;
    data->point.y = s.y;
    data->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  });
}

void Skyscope::probe_touch_start_(const touch::Script &s) {
  if (!probe_input_) return;
  // Asleep, LVGL reads no input: woken as a finger wakes it, and this touch is the one that wakes it.
  if (lvgl_ && lvgl_->is_paused()) lvgl_->set_paused(false, false);
  probe_touch_.play(s);
  ESP_LOGD(TAG, "Touch of %u ms", (unsigned) touch::length(s));
}

void Skyscope::describe_ui_(JsonObject out) {
  out["width"] = width_;
  out["height"] = height_;
  out["asleep"] = asleep_;
  out["touching"] = probe_touch_.busy();  // a touch from /api/touch still being played
  out["status"] = text_of(status_);
  if (shown(setup_)) out["setup"] = text_of(setup_title_);
  out["brightness"] = drag_level_ >= 0 ? drag_level_ : settings_.brightness;
  if (shown(level_)) out["level"] = text_of(level_text_);
  out["picked"] = picked_;
  if (shown(card_)) {
    JsonObject card = out["card"].to<JsonObject>();
    card["title"] = text_of(card_title_);
    JsonArray rows = card["rows"].to<JsonArray>();
    for (lv_obj_t *r : card_rows_) rows.add(text_of(r));
  }
  JsonArray planes = out["planes"].to<JsonArray>();
  // Where a finger finds each now (view.h spots): on a sweeping scope, only the planes its beam has painted.
  for (const auto &p : view::spots(view_)) {
    if (p.x < 0 || p.y < 0 || p.x >= width_ || p.y >= height_) continue;
    JsonObject o = planes.add<JsonObject>();
    o["id"] = p.id;
    o["x"] = p.x;
    o["y"] = p.y;
    o["kind"] = shapes::NAMES[p.kind < shapes::KINDS ? p.kind : 0];
  }
  if (!menu_) return;
  JsonObject menu = out["menu"].to<JsonObject>();
  menu["scroll_y"] = lv_obj_get_scroll_y(menu_list_);
  JsonObject keys = menu["keys"].to<JsonObject>();
  const std::pair<const char *, lv_obj_t *> named[] = {{"cancel", menu_cancel_}, {"save", menu_save_},
                                                       {"up", menu_up_},         {"down", menu_down_},
                                                       {"check", menu_fw_check_}, {"install", menu_fw_install_}};
  for (const auto &k : named) {
    if (!k.second) continue;
    JsonObject key = keys[k.first].to<JsonObject>();
    place(key, k.second);
  }
  if (menu_fw_line_) menu["firmware"] = text_of(menu_fw_line_);
  if (shown(menu_fw_install_)) menu["install"] = text_of(lv_obj_get_child(menu_fw_install_, 0));
  JsonArray rows = menu["rows"].to<JsonArray>();
  const auto &table = menu::rows();
  for (size_t i = 0; i < table.size() && i < menu_rows_.size(); ++i) {
    JsonObject r = rows.add<JsonObject>();
    r["key"] = table[i].key;
    r["shown"] = shown(menu_rows_[i]);
    r["value"] = text_of(menu_values_[i]);
    r["pending"] = menu::shown(table[i], menu_settings_) != menu::shown(table[i], settings_);
    // The row's children: its name, the key back, its value (or the field it is typed in), the key on.
    if (menu::steps(table[i])) {
      place(r["back"].to<JsonObject>(), lv_obj_get_child(menu_rows_[i], 1));
      place(r["on"].to<JsonObject>(), lv_obj_get_child(menu_rows_[i], 3));
    }
    if (menu_fields_[i]) place(r["edit"].to<JsonObject>(), menu_fields_[i]);
  }
  if (menu_wifi_) {
    menu["wifi"] = text_of(menu_wifi_value_);
    place(keys["wifi"].to<JsonObject>(), menu_wifi_);
  }
  if (editor_) {
    JsonObject ed = menu["editor"].to<JsonObject>();
    ed["title"] = text_of(editor_title_);
    ed["text"] = text_of(editor_text_);
    ed["hint"] = text_of(editor_hint_);
    ed["refused"] = !editor_error_.empty();
    place(ed["done"].to<JsonObject>(), editor_done_key_);
    JsonArray ks = ed["keys"].to<JsonArray>();
    for (size_t i = 0; i < board_keys_.size() && i < board_key_objs_.size(); ++i) {
      JsonObject k = ks.add<JsonObject>();
      // A key by what it types, or what it does: "Del", "<", ">", "Aa", "123".
      k["key"] = board_.face(board_keys_[i]);
      place(k, board_key_objs_[i]);
    }
  }
  if (picker_) {
    JsonObject pk = menu["picker"].to<JsonObject>();
    pk["line"] = text_of(picker_line_);
    place(pk["back"].to<JsonObject>(), picker_back_);
    JsonArray items = pk["items"].to<JsonArray>();
    for (uint32_t i = 0; i < lv_obj_get_child_count(picker_list_); ++i) {
      lv_obj_t *item = lv_obj_get_child(picker_list_, i);
      JsonObject it = items.add<JsonObject>();
      it["name"] = text_of(lv_obj_get_child(item, 0));
      place(it, item);
    }
  }
}

void Skyscope::handle_probe_(AsyncWebServerRequest *request, const std::string &url, const std::string &body) {
  if (url == "/api/touch") {
    JsonDocument in;
    const touch::Script script =
        deserializeJson(in, body)
            ? touch::Script{}
            : touch::gesture(in["gesture"] | "", in["x"] | -1, in["y"] | -1, in["x1"] | -1, in["y1"] | -1, in["ms"] | 0);
    const int x = in["x"] | -1, y = in["y"] | -1;
    if (script.empty() || x < 0 || y < 0 || x >= width_ || y >= height_ ||
        (in["gesture"] == "drag" && ((in["x1"] | -1) < 0 || (in["y1"] | -1) < 0))) {
      request->send(400, "application/json", "{\"error\":\"gesture\"}");
      return;
    }
    // One touch at a time (this task alone keeps this).
    const uint32_t now = millis();
    if ((int32_t) (now - probe_busy_until_) < 0) {
      request->send(409, "application/json", "{\"error\":\"busy\"}");
      return;
    }
    probe_busy_until_ = now + touch::length(script) + 100;
    this->defer([this, script]() { this->probe_touch_start_(script); });
    const std::string answer = "{\"ms\":" + std::to_string(touch::length(script)) + "}";
    request->send(200, "application/json", answer.c_str());
    return;
  }
  // /api/ui: described on the loop, waited for here.
  struct Answer {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::string json;
    ~Answer() { vSemaphoreDelete(done); }
  };
  auto answer = std::make_shared<Answer>();
  this->defer([this, answer]() {
    JsonDocument doc;
    describe_ui_(doc.to<JsonObject>());
    serializeJson(doc, answer->json);
    xSemaphoreGive(answer->done);
  });
  if (xSemaphoreTake(answer->done, pdMS_TO_TICKS(3000)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }
  auto *response = request->beginResponse(200, "application/json", answer->json);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}
}  // namespace esphome::skyscope
#endif
