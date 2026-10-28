// The screen's own web page and its API, on ESPHome's web server (port 80):
//
//   GET  /                 the settings page (page.h)
//   GET  /api/settings     every setting, as settings.h writes them
//   POST /api/settings     a JSON object of the settings to change; 400 {"error": key} names the first that is wrong
//   GET  /api/status       Wi-Fi, memory, the feed and the map
//   GET  /api/aircraft     the planes of the last report
//   POST /api/reboot
//
// These run in the HTTP server's task: they read a copy of the settings and hand new ones to the loop (defer), which
// owns them.
#include "skyscope.h"
#include "page.h"
#include "ui.h"

#include <cmath>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.web";

namespace {
constexpr size_t MOST_BODY = 6000;
std::string url_of(AsyncWebServerRequest *request) {
  char buffer[AsyncWebServerRequest::URL_BUF_SIZE];
  const StringRef url = request->url_to(buffer);
  return std::string(url.c_str(), url.size());
}
void send_json(AsyncWebServerRequest *request, int code, const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  auto *response = request->beginResponse(code, "application/json", out);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}
}  // namespace

bool Skyscope::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = url_of(request);
  if (url == "/" || url == "/index.html") return request->method() == HTTP_GET;
  return url.rfind("/api/", 0) == 0;
}

void Skyscope::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) body_.clear();
  if (total > MOST_BODY) return;
  body_.append(reinterpret_cast<const char *>(data), len);
}

void Skyscope::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = url_of(request);
  const bool post = request->method() == HTTP_POST;
  if (url == "/" || url == "/index.html") {
    auto *response = request->beginResponse(200, "text/html", reinterpret_cast<const uint8_t *>(PAGE), sizeof(PAGE) - 1);
    response->addHeader("Cache-Control", "no-cache");
    request->send(response);
    return;
  }
  if (url == "/api/settings") {
    Settings current;
    {
      std::lock_guard<std::mutex> guard(lock_);
      current = shared_.settings;
    }
    if (post) {
      JsonDocument in;
      const std::string text = std::move(body_);
      body_.clear();
      if (text.empty() || text.size() > MOST_BODY || deserializeJson(in, text) || !in.is<JsonObject>()) {
        JsonDocument out;
        out["error"] = "body";
        send_json(request, 400, out);
        return;
      }
      std::string error;
      if (!apply(in.as<JsonObjectConst>(), current, error)) {
        JsonDocument out;
        out["error"] = error;
        send_json(request, 400, out);
        return;
      }
      ESP_LOGI(TAG, "New settings from %s", url.c_str());
      {
        // Seen at once by the next GET, before the loop takes them over.
        std::lock_guard<std::mutex> guard(lock_);
        shared_.settings = current;
      }
      this->defer([this, current]() { this->use_settings_(current); });
    }
    JsonDocument out;
    to_json(current, out.to<JsonObject>());
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/status") {
    JsonDocument out;
    out["name"] = App.get_name();
    out["version"] = version_;
    out["uptime_s"] = millis() / 1000;
    out["heap_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // The largest piece of it in one block: what the largest allocation from internal RAM can have.
    out["heap_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    {
      // The records that grow as the screen runs, by entries.
      const view::Counts c = view::counts(view_);
      JsonObject n = out["records"].to<JsonObject>();
      n["symbols"] = c.symbols;
      n["label_images"] = c.label_images;
      n["spot_of"] = c.spot_of;
      n["drawn"] = c.drawn;
      n["box_of"] = c.box_of;
      n["again"] = c.again;
      n["tracks"] = tracks_.size();
      n["flights"] = flights_.size();
    }
    out["psram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out["glass"] = std::to_string(width_) + "x" + std::to_string(height_);
    // The panel's own way up (rotation 0, nothing turned) and the rotation now.
    out["glass_native"] = std::to_string(native_width_) + "x" + std::to_string(native_height_);
    out["rotation"] = lvgl_ ? (int) lvgl_->get_rotation() : 0;
    // Frames of the last five seconds: how many a second, and how long one took on average and at most.
    out["frames_per_s"] = std::round(ui::frame_stats.per_second * 10) / 10;
    out["frame_ms_avg"] = std::round(ui::frame_stats.average_ms * 10) / 10;
    out["frame_ms_max"] = std::round(ui::frame_stats.longest_ms * 10) / 10;
    out["frame_draw_ms"] = std::round(ui::frame_stats.draw_ms * 10) / 10;
    out["frame_wait_ms"] = std::round(ui::frame_stats.wait_ms * 10) / 10;
    out["frame_send_ms"] = std::round(ui::frame_stats.send_ms * 10) / 10;
    out["frame_pixels"] = (uint32_t) ui::frame_stats.pixels;
    if (wifi::global_wifi_component) {
      out["wifi_rssi"] = wifi::global_wifi_component->wifi_rssi();
      out["ip"] = own_address();
    }
    // A report's cost: on the loop (it draws the glass), and on the worker.
    out["loops_per_s"] = std::round(loops_per_s_ * 10) / 10;
    out["loop_gap_max_ms"] = std::round(loop_gap_max_ms_ * 10) / 10;
    out["sweep_steps_per_s"] = std::round(steps_per_s_ * 10) / 10;
    out["beam_paint_ms"] = std::round(paint_ms_ * 10) / 10;
    out["report_ms"] = std::round(report_ms_ * 10) / 10;
    out["report_ms_max"] = std::round(report_ms_max_ * 10) / 10;
    JsonObject slow = out["slow_frames"].to<JsonObject>();
    for (int p = 0; p < ui::PHASES; ++p) slow[ui::PHASE_NAMES[p]] = ui::slow_frames[p];
    slow["after_report"] = ui::slow_with_report;
    std::lock_guard<std::mutex> guard(lock_);
    out["feed_fetch_ms"] = shared_.fetch_ms;
    out["feed_parse_ms"] = shared_.parse_ms;
    out["feed_every_s"] = (shared_.feed_every_ms + 500) / 1000;
    out["records"]["cached_tiles"] = shared_.cached_tiles;
    out["feed_bytes"] = shared_.reply_bytes;
    out["aircraft"] = shared_.flights.size();
    out["feed_age_s"] = shared_.feed_at ? (int32_t) ((millis() - shared_.feed_at) / 1000) : -1;
    // Each exchange in the order it is tried: waiting or not, for how long, and why.
    out["feed_source"] = FEEDS[shared_.feed_source];
    JsonArray sources = out["feeds"].to<JsonArray>();
    const uint32_t now_ms = millis();
    for (const Feed f : shared_.providers.order()) {
      const providers::State &s = shared_.providers.state(f);
      JsonObject e = sources.add<JsonObject>();
      e["feed"] = FEEDS[f];
      const bool waiting = s.blocked && providers::before(now_ms, s.until);
      e["waiting"] = waiting;
      if (waiting) {
        e["error"] = s.error;
        e["retry_in_s"] = (s.until - now_ms + 999) / 1000;
        e["failures"] = s.failures;
      }
      if (s.paced_ms && providers::before(now_ms, s.paced_until)) e["paced_s"] = (s.paced_ms + 999) / 1000;
    }
    out["tiles_done"] = shared_.tiles_done;
    out["tiles_total"] = shared_.tiles_total;
    out["tile_error"] = shared_.tile_error;
    out["tile_zoom"] = shared_.plan.tile_z;
    out["zoom"] = shared_.plan.view.zoom;
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/aircraft") {
    JsonDocument out;
    JsonArray list = out["aircraft"].to<JsonArray>();
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto &f : shared_.flights) {
      JsonObject a = list.add<JsonObject>();
      a["hex"] = f.hex;
      a["callsign"] = f.callsign;
      a["registration"] = f.registration;
      a["type"] = f.type;
      a["lat"] = f.lat;
      a["lon"] = f.lon;
      if (f.on_ground) a["altitude"] = "ground";
      else if (f.has_altitude) a["altitude"] = f.altitude;
      if (f.has_speed) a["speed"] = f.speed;
      if (f.has_heading) a["heading"] = f.heading;
      if (!f.squawk.empty()) a["squawk"] = f.squawk;
    }
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/debug" && post) {
    // Measurement only, never stored: {"planes": false} hides the plane layer while the map and the beam go on.
    JsonDocument in;
    const std::string text = std::move(body_);
    body_.clear();
    if (deserializeJson(in, text) || !in["planes"].is<bool>()) {
      request->send(400, "application/json", "{\"error\":\"planes\"}");
      return;
    }
    const bool shown = in["planes"].as<bool>();
    this->defer([this, shown]() { view::set_hidden(view_, !shown); });
    request->send(200, "application/json", shown ? "{\"planes\":true}" : "{\"planes\":false}");
    return;
  }
  if (url == "/api/reboot" && post) {
    request->send(200, "application/json", "{\"ok\":true}");
    this->defer([this]() { this->set_timeout(500, []() { App.safe_reboot(); }); });
    return;
  }
  request->send(404, "text/plain", "Not found");
}
}  // namespace esphome::skyscope
