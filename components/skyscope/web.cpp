// The screen's own web page and its API, on ESPHome's web server (port 80):
//
//   GET  /                 the settings page (page.h)
//   GET  /api/settings     every setting, as settings.h writes them
//   POST /api/settings     a JSON object of the settings to change; 400 {"error": key} names the first that is wrong
//                          ({"follow": "BAW286"} follows a plane anywhere, {"follow": ""} stops)
//   GET  /api/status       Wi-Fi, memory, the feed and the map, and the plane followed (follow.h)
//   GET  /api/aircraft     the planes of the last report
//   GET  /api/stats        what the screen has seen in its range today (sightings.h): planes, by the hour, the
//                          most common types and operators, the records, and the previous daily totals
//   GET  /api/log          the latest visits of planes to the range, as CSV (?format=json for JSON)
//   GET  /metrics          Prometheus's text: planes, memory, the feed and the map
//   GET  /api/firmware     the firmware running and the newest on the update server
//   POST /api/firmware     {"check": true} asks the update server again, {"install": true} installs its firmware;
//                          with the test API, {"source": url} asks another update server until a restart
//   GET  /api/networks     the Wi-Fi networks the screen knows, first choice first: their names (never a password)
//   POST /api/networks     with the test API: {"move": [i, step]}, {"forget": i}, {"join": {"name", "password"}} (no
//                          restart: the first choice at the next start)
//   GET  /api/card         the map card (card.h): what it says of itself, its space, what the screen keeps on it, the
//                          tiles read and written, and the download under way
//   POST /api/card         {"estimate": area} the tiles an area takes, {"download": area} downloads it, {"stop": true}
//                          stops the download, {"mount": true} looks for the card again, {"clear": true} deletes the
//                          screen's tiles, {"format": true} formats the card (FAT32, erasing it), {"count": true}
//                          counts what is on it again (where tiles were written since). An area is {"map",
//                          "lat", "lon", "radius_nm", "z_max"}; its map one of sectional, ifr_low, ifr_high or topo.
//   POST /api/sound        {"cue": "watched"} plays a sound on the screen's speaker, where it has one (sound.h CUE_NAMES:
//                          watched, hijack, lost_radio, distress, overhead, click, level), whether or not that sound is
//                          on; 404 without a speaker
//   POST /api/reboot
//   POST /api/reset        {"factory": true} erases everything the screen keeps (the settings, the Wi-Fi networks, the
//                          day's tally) and restarts it as a new screen; the map card is left as it is
//   POST /api/touch, GET /api/ui   tests without a finger (probe.cpp)
//   GET  /api/screenshot   with the test API: the glass as a BMP (probe.cpp)
//   POST /api/test         with the test API: {"divert": "http://host:port"} sends every HTTPS request to a test's mock
//                          from the restart it makes on, until power off or {"divert": ""} (and its restart)
//
// With a password set (settings.h), every one of them asks for it (HTTP Basic, the user "admin").
//
// These run in the HTTP server's task: they read a copy of the settings and hand new ones to the loop (defer), which
// owns them.
#include "skyscope.h"
#include "page.h"
#include "ui.h"

#include <cmath>
#include <cstdio>
#include <ctime>

#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_RGB_PANEL
#include "esphome/components/rgb_panel/rgb_panel.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <fcntl.h>
#include <lwip/sockets.h>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.web";

// The sockets open of the CONFIG_LWIP_MAX_SOCKETS there are: when none is left, every new connection, the page's and
// the screen's own, is turned away.
static int sockets_open() {
  int open = 0;
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd)
    if (lwip_fcntl(fd, F_GETFL, 0) >= 0) ++open;
  return open;
}

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
  if (url == "/" || url == "/index.html" || url == "/metrics") return request->method() == HTTP_GET;
  return url.rfind("/api/", 0) == 0;
}

void Skyscope::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) body_.clear();
  if (total > MOST_BODY) return;
  body_.append(reinterpret_cast<const char *>(data), len);
}

// The web server's task has a small stack (4.5 KB), and every request comes down through this function. Kept to the
// password and where to send the request, it takes little of it: all the API in one function was a frame of 1.3 KB,
// and the log's numbers printed under it ran the stack out (the P4 restarted at "Download the log"). The answers that
// print numbers have functions of their own, and the rest share answer_; none of them is folded into this one.
void Skyscope::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = url_of(request);
  const bool post = request->method() == HTTP_POST;
  // What was sent with it, taken here once: left in body_ by a request that had no use for it, it was read as the body
  // of the next request that came with none.
  const std::string body = std::move(body_);
  body_.clear();
  {
    std::string password;
    {
      std::lock_guard<std::mutex> guard(lock_);
      password = shared_.settings.password;
    }
    if (!password.empty() && !request->authenticate(WEB_USER, password.c_str())) {
      request->requestAuthentication();
      return;
    }
  }
  if (url == "/api/log") return log_reply_(request);
  if (url == "/metrics") return metrics_reply_(request);
  answer_(request, url, post, body);
}

void Skyscope::answer_(AsyncWebServerRequest *request, const std::string &url, bool post, const std::string &body) {
  if (url == "/" || url == "/index.html") {
    auto *response = request->beginResponse(200, "text/html", reinterpret_cast<const uint8_t *>(PAGE),
                                            sizeof(PAGE) - 1);
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
      const std::string &text = body;
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
      this->defer([this, current]() {
        // A view previewed with two fingers is what the page showed, and so is kept.
        if (this->previewing_) this->end_preview_(true);
        this->use_settings_(current);
      });
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
    // Why it last started: a crash or a watchdog shows here, as the screen keeps no log.
    static const char *const REASONS[] = {"unknown", "power_on", "external", "software", "panic", "interrupt_watchdog",
                                          "task_watchdog", "watchdog", "deep_sleep", "brownout", "sdio"};
    const unsigned reason = esp_reset_reason();
    out["reset_reason"] = reason < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[reason] : "other";
    out["asleep"] = asleep_;
#ifdef USE_TIME
    if (clock_) {
      const ESPTime t = clock_->now();
      if (t.is_valid()) {
        char hhmm[8];
        std::snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.hour, t.minute);
        out["time"] = hhmm;  // the screen's local time, by its time zone
      }
    }
#endif
    out["heap_free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // The largest piece of it in one block: what the largest allocation from internal RAM can have.
    out["heap_largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out["sockets"] = sockets_open();
    out["sockets_most"] = CONFIG_LWIP_MAX_SOCKETS;
    {
      // The records that grow as the screen runs, by entries.
      const view::Counts c = view::counts(view_);
      JsonObject n = out["records"].to<JsonObject>();
      n["symbols"] = c.symbols;
      n["symbol_kb"] = c.symbol_kb;  // the plane symbols' PSRAM, held to a budget (view.cpp)
      n["label_images"] = c.label_images;
      n["spot_of"] = c.spot_of;
      n["drawn"] = c.drawn;
      n["box_of"] = c.box_of;
      n["again"] = c.again;
      n["tracks"] = tracks_.size();
      n["flights"] = flights_.size();
    }
    out["psram_free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#ifdef SKYSCOPE_TEST_API
    out["divert"] = divert_;  // where a test sends every request, "" for nowhere
    // Seconds left of a test's lock on the fingers, 0 for none.
    out["touch_locked_s"] = touch_locked_until_ ? (int32_t) (touch_locked_until_ - millis()) / 1000 : 0;
#endif
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
#ifdef USE_RGB_PANEL
    // An RGB panel's refills (rgb_panel.h): a late one is a mark on the glass.
    {
      const rgb_panel::Refills r = rgb_panel::refills();
      JsonObject refills = out["refills"].to<JsonObject>();
      refills["count"] = r.count;
      refills["late"] = r.late;
      refills["late_recent"] = r.late_recent;
      refills["fill_us_max"] = r.fill_us_max;
      refills["span_us_max"] = r.span_us_max;
      refills["budget_us"] = r.budget_us;
      refills["near_recent"] = r.near_recent;
      refills["lateness_us_max"] = r.lateness_us_max;
    }
#endif
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
    out["airspace_draw_ms"] = std::round(airspace_draw_ms_ * 10) / 10;
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
    out["feed_simulated"] = shared_.feed_simulated;  // a receiver's made-up planes (feed.h credit)
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
#ifdef USE_SKYSCOPE_SPEAKER
    // A speaker: the page shows the sound settings.
    if (speaker_) out["speaker"] = true;
#endif
#ifdef USE_SKYSCOPE_RADIO
    // The screen's own radio: what it is doing, and what it hears.
    if (radio_) {
      const Radio::Stats r = radio_->stats();
      JsonObject o = out["radio"].to<JsonObject>();
      o["state"] = r.state;
      o["streaming"] = r.streaming;
      o["tuner"] = r.tuner;
      o["gain_db"] = r.gain_tenths < 0 ? -1.0f : r.gain_tenths / 10.0f;
      o["samples_per_s"] = (uint32_t) r.samples_per_s;
      o["messages_per_s"] = r.messages_per_s;
      o["messages"] = r.messages;
      o["mended"] = r.mended;
      o["rejected"] = r.rejected;
      o["unknown"] = r.unknown;
      o["positions"] = r.positions;
      o["bad_positions"] = r.bad_positions;
      o["dropped_kb"] = (uint32_t) (r.dropped_bytes / 1024);
      o["transfer_errors"] = r.transfer_errors;
      o["noise"] = r.noise;
      o["planes"] = r.planes;
      o["placed"] = r.placed;
    }
#endif
    out["tiles_done"] = shared_.tiles_done;
    out["tiles_total"] = shared_.tiles_total;
    out["tile_error"] = shared_.tile_error;
    out["tile_zoom"] = shared_.plan.tile_z;
    // Where the map's tiles came from since the screen started: their servers or the cache, and frames shown whole
    // again from the one kept, with no tile drawn.
    JsonObject cache = out["tile_cache"].to<JsonObject>();
    cache["tiles"] = shared_.cached_tiles;
    cache["kb"] = shared_.cache_bytes / 1024;
    cache["fetched"] = shared_.tiles_fetched;
    cache["fetched_kb"] = (uint32_t) (shared_.tile_bytes_fetched / 1024);
    cache["from_cache"] = shared_.tiles_from_cache;
    cache["frames_again"] = shared_.frames_again;
    // How long the map takes (skyscope.h TileTimes): each kind of work's count, mean and longest, in milliseconds.
    {
      const Shared::TileTimes &t = shared_.tile_times;
      JsonObject times = out["tile_times"].to<JsonObject>();
      auto spent = [&](const char *name, const tiles::Spent &x) {
        JsonObject o = times[name].to<JsonObject>();
        o["count"] = x.count;
        o["mean_ms"] = std::round(x.mean_ms() * 10) / 10;
        o["most_ms"] = std::round(x.most_us / 100.0) / 10;
      };
      spent("fetch", t.fetch);
      spent("card", t.card);
      spent("png", t.png);
      spent("jpeg", t.jpeg);
      spent("lay", t.lay);
      spent("frames", t.frames);
      times["last_frame_ms"] = t.frame_ms;
      times["last_frame_tiles"] = t.frame_tiles;
      times["last_frame_from"] = t.frame_from;
      times["frames_ahead"] = t.frames_ahead;
      times["tiles_ahead"] = t.tiles_ahead;
    }
    // The weather radar's scan drawn over the map (radar.h), as IEM names it; "" for none.
    out["radar_scan"] = shared_.radar_scan ? radar::stamp(shared_.radar_scan) : std::string();
    // The airspace shown (airspace.h): its areas and runways, its replies' bytes, why the last request failed, and
    // whether it is the one kept in flash from before the start.
    out["airspace_areas"] = shared_.airspace ? shared_.airspace->areas.size() : 0;
    out["airspace_runways"] = shared_.airspace ? shared_.airspace->runways.size() : 0;
    out["airspace_bytes"] = shared_.airspace_bytes;
    out["airspace_rev"] = shared_.airspace_rev;  // counts the replies taken: a new place's is a new one
    out["airspace_error"] = shared_.airspace_error;
    out["airspace_kept"] = shared_.airspace_kept;
    out["zoom"] = shared_.plan.view.zoom;
    // The plane followed (follow.h): how it is going, and where it is, or is likely to be while out of reach.
    if (!shared_.settings.follow.empty()) {
      JsonObject fo = out["follow"].to<JsonObject>();
      const follow::Follower &fl = shared_.follower;
      const bool answered = shared_.follower_for == shared_.settings.follow;
      fo["asked"] = shared_.settings.follow;
      fo["state"] = answered ? follow::PHASES[fl.phase(now_ms)] : "looking";
      const std::string &asked = shared_.settings.follow;
      fo["status"] = answered ? follow::status(fl, asked, now_ms) : "Looking for " + asked;
      if (answered && fl.known()) {
        const geo::Place p = fl.where(now_ms);
        fo["hex"] = fl.hex();
        fo["callsign"] = fl.last().callsign;
        fo["registration"] = fl.last().registration;
        fo["lat"] = std::round(p.lat * 1e4) / 1e4;
        fo["lon"] = std::round(p.lon * 1e4) / 1e4;
        fo["estimated"] = fl.estimating(now_ms);
        fo["seen_s"] = fl.age_ms(now_ms) / 1000;
        if (shared_.route.known) {
          fo["route"]["from"] = shared_.route.from;
          fo["route"]["to"] = shared_.route.to;
        }
      }
    }
    if (card_fitted_) card_json_(out["card"].to<JsonObject>(), shared_.card);
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/card") {
    JsonDocument out;
    Shared::Card state;
    {
      std::lock_guard<std::mutex> guard(lock_);
      state = shared_.card;
    }
    if (!post) {
      card_json_(out.to<JsonObject>(), state);
      send_json(request, card_fitted_ ? 200 : 404, out);
      return;
    }
    JsonDocument in;
    const std::string &text = body;
    if (text.empty() || text.size() > MOST_BODY || deserializeJson(in, text) || !in.is<JsonObject>()) {
      out["error"] = "body";
      send_json(request, 400, out);
      return;
    }
    if (!card_fitted_) {
      out["error"] = "This board has no card slot";
      send_json(request, 404, out);
      return;
    }
    // An area as the page sends it, or why it is not one.
    auto area_of = [](JsonObjectConst a, card::Area &area, card::Plan &plan, std::string &error) {
      if (!card::style_of(a["map"] | "", area.style)) {
        error = "Only the FAA's charts (sectional, ifr_low, ifr_high) and the USGS map (topo) are downloaded";
        return false;
      }
      area.lat = a["lat"] | 1000.0;
      area.lon = a["lon"] | 1000.0;
      area.radius_nm = a["radius_nm"] | 0;
      area.z_max = a["z_max"] | card::default_zoom(area.style);
      return card::plan(area, plan, error);
    };
    const char *ask = in["estimate"].is<JsonObject>() ? "estimate" : in["download"].is<JsonObject>() ? "download" : "";
    if (*ask) {
      card::Area area;
      card::Plan plan;
      std::string error;
      const bool ok = area_of(in[ask].as<JsonObjectConst>(), area, plan, error);
      out["tiles"] = plan.total;
      // What it downloads, and what it takes of the card (whole clusters), with what the card has free.
      auto mb = [](uint64_t bytes) { return (uint32_t) ((bytes + 512 * 1024) / (1024 * 1024)); };
      out["mb"] = mb(card::estimate_bytes(plan));
      out["card_mb"] = mb(card::card_bytes(plan, state.cluster));
      if (state.mounted) out["free_mb"] = (uint32_t) (state.free / (1024 * 1024));
      out["z_min"] = card::first_zoom(area.style);
      out["z_max"] = area.z_max;
      if (!ok) {
        out["error"] = error;
        send_json(request, 400, out);
        return;
      }
      if (std::string(ask) == "download") {
        if (!state.mounted) {
          out["error"] = "No map card";
          send_json(request, 409, out);
          return;
        }
        ESP_LOGI(TAG, "Download asked: %s, %d NM, zoom %d", card::map_of(area.style), area.radius_nm, area.z_max);
        std::lock_guard<std::mutex> guard(lock_);
        shared_.download = area;
        shared_.download_ask++;
      }
      send_json(request, 200, out);
      return;
    }
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (in["stop"] | false) shared_.stop_ask++;
      if (in["mount"] | false) shared_.mount_ask++;
      if (in["clear"] | false) shared_.clear_ask++;
      if (in["count"] | false) shared_.count_ask++;
      if (in["format"] | false) {
        ESP_LOGI(TAG, "Formatting the map card, asked from the page");
        shared_.format_ask++;
      }
    }
    card_json_(out.to<JsonObject>(), state);
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
  if (url == "/api/stats") {
    JsonDocument out;
    {
      std::lock_guard<std::mutex> guard(lock_);
      sightings::stats_json(shared_.book, out.to<JsonObject>());
    }
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/debug" && post) {
    // Measurement only, never stored: {"planes": false} hides the plane layer while the map and the beam go on.
    JsonDocument in;
    const std::string &text = body;
    if (deserializeJson(in, text) || !in["planes"].is<bool>()) {
      request->send(400, "application/json", "{\"error\":\"planes\"}");
      return;
    }
    const bool shown = in["planes"].as<bool>();
    this->defer([this, shown]() { view::set_hidden(view_, !shown); });
    request->send(200, "application/json", shown ? "{\"planes\":true}" : "{\"planes\":false}");
    return;
  }
  if (url == "/api/firmware") {
#ifdef USE_UPDATE
    if (post && firmware_) {
      JsonDocument in;
      const std::string &text = body;
      const bool broken = (bool) deserializeJson(in, text);
      bool source = false;
#ifdef SKYSCOPE_TEST_API
      source = !broken && in["source"].is<const char *>();  // another update server, for tests/device_test.py's mock
#endif
      if (broken || !(source || in["check"].as<bool>() || in["install"].as<bool>())) {
        request->send(400, "application/json", "{\"error\":\"body\"}");
        return;
      }
      if (source) {
        // Asked at once, and until the screen restarts.
        std::string url = in["source"].as<std::string>();
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
          request->send(400, "application/json", "{\"error\":\"source\"}");
          return;
        }
        this->defer([this, url]() {
          firmware_->set_source_url(url);
          firmware_->check();
        });
      } else if (in["install"].as<bool>()) {
        this->defer([this]() { this->install_firmware_(); });
      } else {
        this->defer([this]() { firmware_->check(); });
      }
    }
#endif
    JsonDocument out;
    out["version"] = version_;
#ifdef USE_UPDATE
    out["updates"] = firmware_ != nullptr;
    // The manifest it asks, its board's own: where tests/device_test.py finds the release this screen is offered.
    if (firmware_ && firmware_->get_source_url()) out["source"] = firmware_->get_source_url();
#else
    out["updates"] = false;
#endif
    std::lock_guard<std::mutex> guard(lock_);
    const Shared::Firmware &f = shared_.firmware;
    out["known"] = f.known;
    if (f.known) out["latest"] = f.latest;
    out["available"] = f.available;
    out["installing"] = f.installing || shared_.updating;
    if (f.installing) out["progress"] = std::round(f.progress);
    if (!f.summary.empty()) out["summary"] = f.summary;
    if (!f.release_url.empty()) out["release_url"] = f.release_url;
    if (!f.error.empty()) out["error"] = f.error;
    send_json(request, 200, out);
    return;
  }
#ifdef SKYSCOPE_TEST_API
  if (url == "/api/test" && post) {
    JsonDocument in;
    const std::string &text = body;
    const bool unreadable = (bool) deserializeJson(in, text);
    // The fingers locked out for N seconds while a test runs (0 lets them back), with no restart.
    if (!unreadable && in["lock_touch_s"].is<int>() && in["divert"].isNull()) {
      const int seconds = in["lock_touch_s"].as<int>();
      if (seconds < 0 || seconds > 24 * 3600) {
        request->send(400, "application/json", "{\"error\":\"lock_touch_s\"}");
        return;
      }
      this->defer([this, seconds]() { this->lock_touch_((uint32_t) seconds); });
      request->send(200, "application/json", seconds ? "{\"touch_locked\":true}" : "{\"touch_locked\":false}");
      return;
    }
    // The splash again, as at the start: to look at it, and for its picture.
    if (!unreadable && in["splash"] == true && in["divert"].isNull()) {
      this->defer([this]() {
        this->end_splash_();
        this->start_splash_();
      });
      request->send(200, "application/json", "{\"splash\":true}");
      return;
    }
    const bool broken = unreadable || !in["divert"].is<const char *>();
    const std::string origin = broken ? std::string() : in["divert"].as<std::string>();
    if (broken || (!origin.empty() && (origin.rfind("http://", 0) != 0 || origin.size() >= 96 ||
                                       origin.back() == '/'))) {
      request->send(400, "application/json", "{\"error\":\"divert\"}");
      return;
    }
    keep_divert_(origin);
    request->send(200, "application/json", "{\"restarting\":true}");
    this->defer([this]() { this->set_timeout(500, []() { App.safe_reboot(); }); });
    return;
  }
  if (url == "/api/bench") {
    // The tile decoder's benchmark (worker.cpp): POST {"runs": 3} begins it on the map shown, GET answers with it.
    if (post) {
      JsonDocument in;
      const std::string &text = body;
      const int runs = deserializeJson(in, text) ? 3 : (in["runs"] | 3);
      std::lock_guard<std::mutex> guard(lock_);
      shared_.bench_runs = runs;
      shared_.bench_rev++;
      shared_.bench_busy = true;
      shared_.bench_result.clear();
      request->send(200, "application/json", "{\"started\":true}");
      return;
    }
    std::string answer;
    {
      std::lock_guard<std::mutex> guard(lock_);
      answer = std::string("{\"busy\":") + (shared_.bench_busy ? "true" : "false") + ",\"result\":" +
               (shared_.bench_result.empty() ? "null" : shared_.bench_result) + "}";
    }
    auto *response = request->beginResponse(200, "application/json", answer);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    return;
  }
  if ((url == "/api/touch" && post) || url == "/api/ui" || url == "/api/screenshot") {
    const std::string &text = body;
    this->handle_probe_(request, url, text);
    return;
  }
#endif
  if (url == "/api/networks" && known_networks_) {
#ifdef SKYSCOPE_TEST_API
    if (post) {
      JsonDocument in;
      const std::string &text = body;
      if (deserializeJson(in, text)) {
        request->send(400, "application/json", "{\"error\":\"json\"}");
        return;
      }
      // Changed on the loop, which owns the menu's list too; answered with the list as it will be.
      auto ask = std::make_shared<JsonDocument>(std::move(in));
      this->defer([this, ask]() {
        JsonDocument &in = *ask;
        if (in["move"].is<JsonArrayConst>()) known_networks_->move(in["move"][0] | 99, in["move"][1] | 0);
        if (in["forget"].is<int>()) known_networks_->forget(in["forget"].as<int>());
        if (in["join"].is<JsonObjectConst>())
          known_networks_->joined(in["join"]["name"] | "", in["join"]["password"] | "");
      });
      request->send(200, "application/json", "{\"ok\":true}");
      return;
    }
#endif
    JsonDocument out;
    JsonArray list = out["networks"].to<JsonArray>();
    for (const auto &n : known_networks_->list().networks) list.add(n.name);
    send_json(request, 200, out);
    return;
  }
  if (url == "/api/reset" && post) {
    // Only when it says what it means: a body sent here by mistake erases nothing.
    JsonDocument in;
    if (deserializeJson(in, body) || in["factory"] != true) {
      request->send(400, "application/json", "{\"error\":\"factory\"}");
      return;
    }
    request->send(200, "application/json", "{\"resetting\":true}");
    this->defer([this]() { this->factory_reset_(); });
    return;
  }
  if (url == "/api/sound" && post) {
#ifdef USE_SKYSCOPE_SPEAKER
    JsonDocument in;
    const int cue = deserializeJson(in, body) ? -1 : sound::cue_named(in["cue"] | "");
    if (!speaker_) {
      request->send(404, "application/json", "{\"error\":\"speaker\"}");
    } else if (cue < 0) {
      request->send(400, "application/json", "{\"error\":\"cue\"}");
    } else {
      request->send(200, "application/json", "{\"ok\":true}");
      this->defer([this, cue]() { this->play_((sound::Cue) cue, true); });
    }
#else
    request->send(404, "application/json", "{\"error\":\"speaker\"}");
#endif
    return;
  }
  if (url == "/api/reboot" && post) {
    request->send(200, "application/json", "{\"ok\":true}");
    this->defer([this]() { this->set_timeout(500, []() { App.safe_reboot(); }); });
    return;
  }
  request->send(404, "text/plain", "Not found");
}

// The latest visits, as CSV (the page's download) or as JSON with ?format=json.
void Skyscope::log_reply_(AsyncWebServerRequest *request) {
  std::vector<sightings::Visit> log;
  bool metric;
  {
    std::lock_guard<std::mutex> guard(lock_);
    log = shared_.book.log();
    metric = shared_.settings.metric;
  }
  const bool json = request->arg("format") == "json";
  auto local = [](int64_t unix_s) {
    const time_t t = (time_t) unix_s;
    struct tm l {};
    localtime_r(&t, &l);
    char text[24];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &l);
    return std::string(text);
  };
  if (json) {
    JsonDocument out;
    JsonArray list = out["visits"].to<JsonArray>();
    for (auto it = log.rbegin(); it != log.rend(); ++it) {
      const sightings::Visit &v = *it;
      JsonObject o = list.add<JsonObject>();
      o["first"] = v.first;
      o["last"] = v.last;
      o["hex"] = v.hex;
      o["callsign"] = v.callsign;
      o["registration"] = v.registration;
      o["type"] = v.type;
      o["operator"] = v.operator_name;
      o["closest_nm"] = std::round(v.closest_m / geo::NM * 10) / 10;
      if (v.lowest_ft != INT32_MAX) {
        o["lowest_ft"] = v.lowest_ft;
        o["highest_ft"] = v.highest_ft;
      }
      if (v.watched) o["watched"] = true;
      if (v.emergency) o["emergency"] = v.squawk;
    }
    send_json(request, 200, out);
    return;
  }
  std::string text = sightings::csv_header(metric);
  for (const auto &v : log) text += sightings::csv_line(v, metric, local);
  auto *response = request->beginResponse(200, "text/csv", text);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Content-Disposition", "attachment; filename=\"skyscope-log.csv\"");
  request->send(response);
}

// The screen's health for Prometheus.
void Skyscope::metrics_reply_(AsyncWebServerRequest *request) {
  std::string text;
  // Put together by hand, not with snprintf: a float printed takes more stack than the web server's task can spare.
  auto metric = [&](const char *name, const char *help, double value, const char *type = "gauge") {
    text.append("# HELP skyscope_").append(name).append(" ").append(help).append("\n# TYPE skyscope_").append(name);
    text.append(" ").append(type).append("\nskyscope_").append(name).append(" ");
    text.append(sky::numbers::text(value));
    text += '\n';
  };
  metric("uptime_seconds", "Seconds since the screen started", millis() / 1000.0, "counter");
  metric("internal_free_bytes", "Internal RAM free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  metric("internal_largest_bytes", "The largest block of internal RAM free",
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  metric("psram_free_bytes", "PSRAM free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  metric("sockets_open", "Sockets open", sockets_open());
  metric("loop_gap_max_ms", "The longest gap in ESPHome's loop over the last five seconds", loop_gap_max_ms_);
  metric("frames_per_second", "Frames drawn a second", ui::frame_stats.per_second);
  metric("asleep", "1 while the screen is off", asleep_ ? 1 : 0);
  if (wifi::global_wifi_component) metric("wifi_rssi_dbm", "Wi-Fi signal", wifi::global_wifi_component->wifi_rssi());
  std::lock_guard<std::mutex> guard(lock_);
  metric("aircraft", "Planes in the last report kept", shared_.flights.size());
  metric("planes_in_range", "Planes within the range in the last report around the place", shared_.book.in_range());
  metric("planes_today", "Planes seen in range since local midnight", shared_.book.today().planes);
  metric("visits_today", "Visits of planes to the range since local midnight", shared_.book.today().visits);
  metric("feed_age_seconds", "Seconds since the last good report",
         shared_.feed_at ? (millis() - shared_.feed_at) / 1000.0 : -1);
  metric("feed_fetch_ms", "The last report's download", shared_.fetch_ms);
  metric("feed_parse_ms", "The last report's reading", shared_.parse_ms);
  metric("tiles_fetched_total", "Map tiles fetched from their servers", shared_.tiles_fetched, "counter");
  metric("tiles_from_cache_total", "Map tiles found in the cache", shared_.tiles_from_cache, "counter");
  metric("tile_cache_bytes", "The tile cache's bytes", shared_.cache_bytes);
  auto *response = request->beginResponse(200, "text/plain; version=0.0.4", text);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

// On the loop: what the update entity knows, for the page. While firmware is flashing the loop stands still, but the
// entity still reports its progress here, and the web server (its own task) still answers.
#ifdef USE_UPDATE
static_assert((int) firmware::INSTALLING == (int) update::UPDATE_STATE_INSTALLING &&
                  (int) firmware::AVAILABLE == (int) update::UPDATE_STATE_AVAILABLE,
              "firmware.h's states are ESPHome's");
#endif

void Skyscope::firmware_changed_() {
#ifdef USE_UPDATE
  if (!firmware_) return;
  const firmware::State state = (firmware::State) firmware_->state;
  const update::UpdateInfo &info = firmware_->update_info;
  if (install_.seen(state, firmware_->status_has_error())) ESP_LOGW(TAG, "The firmware update failed");
  std::lock_guard<std::mutex> guard(lock_);
  Shared::Firmware &f = shared_.firmware;
  f.known = !info.latest_version.empty();
  f.latest = info.latest_version;
  f.available = state == firmware::AVAILABLE;
  f.installing = state == firmware::INSTALLING;
  f.progress = info.has_progress ? info.progress : 0;
  f.summary = info.summary;
  f.release_url = info.release_url;
  f.error = install_.error;
  shared_.updating = install_.busy() || resetting_;
#endif
}

// On the loop: the screen says it is updating, the worker rests, and a moment later (the screen drawn, the worker's
// request done) the firmware is downloaded into the other half of the flash, checked and booted. Nothing is written
// over the running firmware, so a broken download leaves it as it is.
void Skyscope::install_firmware_() {
#ifdef USE_UPDATE
  if (!firmware_ || !install_.start((firmware::State) firmware_->state)) return;
  ESP_LOGI(TAG, "Installing firmware %s", firmware_->update_info.latest_version.c_str());
  close_menu_();  // the screen says it is updating, over the map
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.updating = true;
    shared_.firmware.error.clear();
  }
  update_status_(millis());
  this->set_timeout(1500, [this]() { firmware_->perform(false); });
#endif
}

#ifdef USE_OTA_STATE_LISTENER
// On the loop. Firmware sent from a computer (esphome run or upload) holds the loop from its first byte to its last,
// so the updating page is drawn on the glass here and now, and the worker rests: without this the map stood still
// through the upload with nothing said. One that fails or is broken off puts the screen back as it was.
void Skyscope::on_ota_global_state(ota::OTAState state, float, uint8_t, ota::OTAComponent *) {
  if (state == ota::OTA_STARTED) {
    if (!install_.upload_started()) return;
    ESP_LOGI(TAG, "Firmware coming in: the screen says it is updating");
    close_menu_();
  } else if (state == ota::OTA_ERROR || state == ota::OTA_ABORT) {
    if (!install_.uploading) return;
    install_.upload_ended();
    ESP_LOGW(TAG, "The firmware sent to the screen did not all come in");
  } else {
    return;
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.updating = install_.busy() || resetting_;
  }
  if (!ui_built_) return;
  update_status_(millis());
  if (install_.uploading) lv_refr_now(nullptr);
}
#endif
void Skyscope::card_json_(JsonObject out, const Shared::Card &c) const {
  out["mounted"] = c.mounted;
  out["error"] = c.error;
  out["unformatted"] = c.unformatted;
  out["clearing"] = c.clearing;
  out["cleared"] = c.cleared;
  out["total_mb"] = (uint32_t) (c.total / (1024 * 1024));
  out["free_mb"] = (uint32_t) (c.free / (1024 * 1024));
  out["cluster_kb"] = c.cluster / 1024;
  out["formatting"] = c.formatting;
  if (c.mounted) {
    // What the card says of itself.
    JsonObject about = out["about"].to<JsonObject>();
    about["name"] = c.about.name;
    if (const char *maker = card::maker_name(c.about.maker)) about["maker"] = maker;
    about["kind"] = card::kind_of(c.about);
    about["made"] = card::made_words(c.about.made);
    about["mhz"] = (c.about.khz + 500) / 1000;
    about["lines"] = c.about.lines;
    // What the screen keeps on it, as last counted (whole clusters, in KB).
    JsonObject kept = out["kept"].to<JsonObject>();
    kept["counted"] = c.tally.counted;
    kept["counting"] = c.tally.counting;
    kept["tiles"] = c.tally.tile_count();
    kept["tiles_kb"] = (uint32_t) (c.tally.tile_bytes() / 1024);
    kept["log_kb"] = (uint32_t) (c.tally.log_bytes / 1024);
    kept["logs"] = c.tally.logs;
    JsonArray maps = kept["maps"].to<JsonArray>();
    for (const card::MapShare &m : card::by_map(c.tally)) {
      JsonObject o = maps.add<JsonObject>();
      o["map"] = m.map;
      o["label"] = m.label;
      o["tiles"] = m.tiles;
      o["kb"] = (uint32_t) (m.bytes / 1024);
    }
  }
  // Tiles since the start: read from the card, written to it, and writes that failed; off a network, the frame's
  // tiles the card does not have.
  out["read"] = c.read;
  out["written"] = c.written;
  out["failed"] = c.failed;
  out["missing"] = c.missing;
  JsonObject d = out["download"].to<JsonObject>();
  d["running"] = c.downloading;
  if (c.downloading || c.total_tiles) {
    d["map"] = card::map_of(c.area.style);
    d["radius_nm"] = c.area.radius_nm;
    d["z_max"] = c.area.z_max;
    d["next"] = c.next;
    d["total"] = c.total_tiles;
    d["fetched"] = c.fetched;
    d["skipped"] = c.skipped;
    d["given_up"] = c.given_up;
    d["waiting_s"] = c.waiting_s;
  }
  d["error"] = c.download_error;
}


}  // namespace esphome::skyscope
