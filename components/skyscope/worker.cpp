// The worker task: the planes from the ADS-B exchange, and the map's tiles into the base frame. Everything here may
// take seconds (a TLS handshake, a slow tile server), so none of it runs on ESPHome's loop.
#include "skyscope.h"

#include "decode.h"
#include "http.h"
#include "colours.h"
#include "ui.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <list>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.worker";

namespace {
// Tiles as they came (PNG or JPEG bytes), in PSRAM, the least recently drawn going first. A tile the server has none
// of (a chart outside its country) is kept as empty, so it is not asked for again and again.
constexpr size_t CACHE_BYTES = 3 * 1024 * 1024;
constexpr uint32_t TILE_TTL_MS = 24 * 3600 * 1000;
constexpr size_t MOST_TILE_BYTES = 512 * 1024, MOST_FEED_BYTES = 768 * 1024;
// Of a report, the planes beyond the most shown that are kept (overlay.h keep_showable).
constexpr int KEEP_EXTRA = 20;
struct Cached {
  tiles::Key key;
  uint8_t *data = nullptr;
  size_t size = 0;
  uint32_t at = 0;
};
class Cache {
 public:
  Cached *find(const tiles::Key &k, uint32_t now) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
      if (!(it->key == k)) continue;
      if (now - it->at > TILE_TTL_MS) {
        drop_(it);
        return nullptr;
      }
      items_.splice(items_.begin(), items_, it);
      return &items_.front();
    }
    return nullptr;
  }
  void forget(const tiles::Key &k) {
    for (auto it = items_.begin(); it != items_.end(); ++it)
      if (it->key == k) {
        drop_(it);
        return;
      }
  }
  Cached *keep(const tiles::Key &k, const uint8_t *data, size_t size, uint32_t now) {
    while (bytes_ + size > CACHE_BYTES && !items_.empty()) drop_(std::prev(items_.end()));
    Cached c{k, nullptr, size, now};
    if (size) {
      c.data = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!c.data) return nullptr;
      std::memcpy(c.data, data, size);
    }
    bytes_ += size;
    items_.push_front(c);
    return &items_.front();
  }

 private:
  void drop_(std::list<Cached>::iterator it) {
    bytes_ -= it->size;
    heap_caps_free(it->data);
    items_.erase(it);
  }
  std::list<Cached> items_;
  size_t bytes_ = 0;

 public:
  size_t size() const { return items_.size(); }
};

// How long a source that failed is left alone: a feed that says "too many requests" longer.
// A tile that did not come is asked again after 2 s, then 4, 8 ... at most 60, while the others go on loading; one
// that came but could not be read is fetched again at most this many times.
constexpr uint32_t TILE_RETRY_FIRST_MS = 2000, TILE_RETRY_MOST_MS = 60000;
// The FAA's airspace server is shared by everyone who asks it: a request that failed waits 30 s, then twice as long
// each time it fails again, at most 15 minutes.
constexpr uint32_t AIRSPACE_RETRY_FIRST_MS = 30000, AIRSPACE_RETRY_MOST_MS = 15 * 60000;
constexpr int TILE_UNREADABLE_MOST = 3;
struct Pending {
  size_t index;          // into the plan's keys
  uint32_t due = 0;      // millis() when it may be asked again
  int failures = 0, unreadable = 0;
};
}  // namespace

void Skyscope::worker_main(void *self) { static_cast<Skyscope *>(self)->worker_(); }

void Skyscope::worker_() {
  Cache cache;
  http::Body body;
  Settings settings;
  tiles::Plan plan;
  uint32_t settings_rev = 0, plan_rev = 0, painting_rev = 0;
  std::vector<Pending> pending;  // the plan's tiles not drawn yet, in its order (nearest the middle first)
  uint32_t feed_due = 0;
  std::string feed_key;  // the place, range and sources the last request was for: a new one asks at once
  std::string receiver_url;  // the receiver's address the last request was for
  providers::Providers providers;
  std::string airspace_asked;  // the requests (airspace.h Ask) the airspace shown answered
  uint32_t airspace_due = 0, airspace_wait = 0;
  const int width = width_, height = height_;
  while (true) {
    bool online;
    {
      std::lock_guard<std::mutex> guard(lock_);
      // The screen off counts as offline: nobody sees planes or tiles, so none are asked for. So does an update: its
      // download has the network to itself.
      online = shared_.online && !shared_.asleep && !shared_.updating;
      if (shared_.settings_rev != settings_rev) {
        settings = shared_.settings;
        settings_rev = shared_.settings_rev;
      }
      if (shared_.plan_rev != plan_rev) {
        plan = shared_.plan;
        plan_rev = shared_.plan_rev;
      }
    }
    const uint32_t now = millis();
    if (!online || !settings.located || plan.view.width == 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    // ---- The planes, every refresh_s, or at once for a new place or range, from the first exchange free
    // (providers.h): the preferred one, or while it waits the next in the list.
    providers.set_order(settings.feed, settings.fallback, !settings.receiver_url.empty());
    const int radius_nm = std::max(1, (int) std::ceil(plan.view.reach_metres() / geo::NM * 1.05));
    char place[64];
    std::snprintf(place, sizeof(place), "%.4f,%.4f,%d,%d,%d,", settings.lat, settings.lon, radius_nm, (int) settings.feed,
                  (int) settings.fallback);
    const std::string key = place + settings.receiver_url;
    if (key != feed_key) {
      feed_key = key;
      feed_due = now;
    }
    if (settings.receiver_url != receiver_url) {
      // A corrected address is asked at once, not after the wait the old one earned.
      receiver_url = settings.receiver_url;
      providers.forget(RECEIVER);
    }
    if ((int32_t) (now - feed_due) >= 0) {
      Feed source;
      uint32_t wait = 0;
      if (!providers.choose(now, source, wait)) {
        // Every exchange it may ask is waiting: until the first is free.
        feed_due = now + wait;
      } else {
        const std::string url = feed::url(source, settings.lat, settings.lon, radius_nm, settings.receiver_url);
        const uint32_t asked = millis();
        ui::worker_phase = ui::FEED_FETCH;
        const http::Result r = http::get(url, body, MOST_FEED_BYTES, agent_, "application/json");
        const uint32_t fetched = millis();
        ui::worker_phase = ui::FEED_PARSE;
        std::vector<feed::Flight> flights;
        std::string error = r.error;
        bool ok = r.status == 200;
        if (ok) {
          JsonDocument doc;  // one plane at a time (feed.h): a kilobyte or two
          ok = feed::parse(reinterpret_cast<const char *>(body.data), body.size, doc, flights, error);
          if (ok) overlay::keep_showable(flights, settings, plan.view, KEEP_EXTRA);
        }
        const uint32_t done = millis();
        ui::worker_phase = ui::IDLE;
        providers::Answer answer{r.status, r.retry_after_s, r.rate_reset_s, r.rate_limit, error};
        if (ok) {
          const uint32_t every = providers.succeeded(source, done, settings.refresh_s * 1000, answer);
          feed_due = done + every;
          std::lock_guard<std::mutex> guard(lock_);
          shared_.flights = std::move(flights);
          shared_.flights_rev++;
          shared_.feed_at = done;
          shared_.feed_every_ms = every;
          shared_.feed_source = source;
        } else {
          const uint32_t left = providers.failed(source, done, answer);
          ESP_LOGW(TAG, "%s: %s (HTTP %d), left alone for %u s%s", feed::credit(source), providers.state(source).error.c_str(),
                   r.status, (unsigned) (left / 1000), r.rate_limit.empty() ? "" : (" (its limits: " + r.rate_limit + ")").c_str());
          // The next exchange in the list at once, if there is one free; else when the first one is.
          feed_due = done;
        }
        std::lock_guard<std::mutex> guard(lock_);
        shared_.fetch_ms = fetched - asked;
        shared_.parse_ms = done - fetched;
        shared_.reply_bytes = body.size;
        body.clear();
        if (body.room > 256 * 1024) body.release();  // a large reply's memory goes back
      }
      std::lock_guard<std::mutex> guard(lock_);
      shared_.providers = providers;
    }

    // ---- The airspace and runways (airspace.h), once for each place and range while they are shown.
    if (settings.airspace) {
      const airspace::Ask ask = airspace::ask(plan.view, settings.range_nm);
      if (ask.airspace + ask.runways != airspace_asked && (int32_t) (now - airspace_due) >= 0) {
        auto map = std::make_shared<airspace::Map>();
        std::vector<uint32_t> airports;
        std::string error;
        uint32_t bytes = 0;
        JsonDocument doc;  // one feature's attributes at a time
        ui::worker_phase = ui::AIRSPACE;
        http::Result r = http::get(ask.airspace, body, airspace::MOST_REPLY_BYTES, agent_, "application/json");
        const char *text = reinterpret_cast<const char *>(body.data);
        bool ok = r.status == 200 && airspace::read_airspace(text, body.size, doc, *map, airports, error);
        bytes += body.size;
        if (ok && !ask.runways.empty()) {
          body.clear();
          r = http::get(ask.runways, body, airspace::MOST_REPLY_BYTES, agent_, "application/json");
          text = reinterpret_cast<const char *>(body.data);
          ok = r.status == 200 && airspace::read_runways(text, body.size, doc, airports, *map, error);
          bytes += body.size;
        }
        if (r.status != 200) error = r.error;
        ui::worker_phase = ui::IDLE;
        body.clear();
        if (body.room > 256 * 1024) body.release();
        if (ok) {
          airspace_asked = ask.airspace + ask.runways;
          airspace_wait = 0;
          ESP_LOGI(TAG, "Airspace: %u areas (%u points), %u runways, %u bytes%s", (unsigned) map->areas.size(),
                   (unsigned) map->points.size(), (unsigned) map->runways.size(), (unsigned) bytes,
                   map->cut ? ", not all of it" : "");
        } else {
          airspace_wait = std::min(AIRSPACE_RETRY_MOST_MS, airspace_wait ? airspace_wait * 2 : AIRSPACE_RETRY_FIRST_MS);
          airspace_wait = std::max<uint32_t>(airspace_wait, r.retry_after_s * 1000);
          airspace_due = millis() + airspace_wait;
          ESP_LOGW(TAG, "Airspace: %s (HTTP %d), again in %u s", error.c_str(), r.status,
                   (unsigned) (airspace_wait / 1000));
        }
        std::lock_guard<std::mutex> guard(lock_);
        if (ok) {
          shared_.airspace = std::move(map);
          shared_.airspace_rev++;
        }
        shared_.airspace_error = ok ? std::string() : error;
        shared_.airspace_bytes = bytes;
      }
    }

    // Connections idle for half a minute are closed: each holds its TLS session's memory.
    http::close_idle((uint32_t) (esp_timer_get_time() / 1000), 30000);

    // ---- The map, one tile a turn, so the planes keep coming while it fills.
    if (painting_rev != plan_rev) {
      painting_rev = plan_rev;
      pending.clear();
      for (size_t i = 0; i < plan.keys.size(); ++i) pending.push_back({i});
      {
        std::lock_guard<std::mutex> frame(frame_lock_);
        // The ground where no tile is (yet): black in the dark appearance, a pale gray in the light one.
        const uint32_t g = colours::palette(settings.theme == LIGHT).ground;
        std::fill(base_frame_, base_frame_ + width * height, tiles::rgb565(g >> 16 & 0xFF, g >> 8 & 0xFF, g & 0xFF));
      }
      std::lock_guard<std::mutex> guard(lock_);
      shared_.frame_rev++;
      shared_.frame_plan_rev = plan_rev;
      shared_.tiles_done = 0;
      shared_.tiles_total = plan.keys.size();
      shared_.tile_error.clear();
    }
    // The first tile due, nearest the middle first; one that failed waits its turn while the others load.
    auto due = std::find_if(pending.begin(), pending.end(), [&](const Pending &p) { return (int32_t) (millis() - p.due) >= 0; });
    if (due != pending.end()) {
      Pending &job = *due;
      const tiles::Key key = plan.keys[job.index];
      Cached *tile = cache.find(key, millis());
      std::string error;
      bool fetched_failed = false;
      if (!tile) {
        ui::worker_phase = ui::TILE_FETCH;
        const http::Result r = http::get(tiles::url(settings, key), body, MOST_TILE_BYTES, agent_, "image/png,image/jpeg");
        ui::worker_phase = ui::IDLE;
        if (r.status == 200) tile = cache.keep(key, body.data, body.size, millis());
        // No tile there (outside a chart's country): remembered as empty, drawn as the ground.
        else if (r.status == 404 || r.status == 204) tile = cache.keep(key, nullptr, 0, millis());
        else error = r.error;
        fetched_failed = !tile;
        body.clear();
      }
      bool drawn = tile && !tile->size;  // an empty square is done
      if (tile && tile->size) {
        const geo::TileOrigin at = geo::tile_origin(plan.view, key.z, key.x, key.y);
        std::lock_guard<std::mutex> frame(frame_lock_);
        ui::worker_phase = ui::TILE_DECODE;
        drawn = decode::draw(tile->data, tile->size, base_frame_, width, height, at, tiles::filter_of(settings), error);
        ui::worker_phase = ui::IDLE;
        // What could not be read is not kept: the next try fetches it again.
        if (!drawn) cache.forget(key);
      }
      bool given_up = false;
      if (drawn) {
        pending.erase(due);
      } else {
        if (!fetched_failed) ++job.unreadable;
        ++job.failures;
        given_up = job.unreadable >= TILE_UNREADABLE_MOST;
        const uint32_t wait = std::min<uint32_t>(TILE_RETRY_MOST_MS, TILE_RETRY_FIRST_MS << std::min(job.failures - 1, 5));
        ESP_LOGW(TAG, "Tile %u/%u/%u%s: %s; %s", key.z, (unsigned) key.x, (unsigned) key.y, key.layer ? " (names)" : "",
                 error.c_str(), given_up ? "given up" : ("again in " + std::to_string(wait / 1000) + " s").c_str());
        if (given_up) pending.erase(due);
        else job.due = millis() + wait;
      }
      std::lock_guard<std::mutex> guard(lock_);
      shared_.cached_tiles = cache.size();
      if (shared_.frame_plan_rev == plan_rev) {
        if (drawn) shared_.tiles_done++;
        if (!drawn) shared_.tile_error = error.empty() ? std::string("tile server") : error;
        else if (pending.empty() || std::none_of(pending.begin(), pending.end(), [](const Pending &p) { return p.failures; }))
          shared_.tile_error.clear();
        shared_.frame_rev++;
      }
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}  // namespace esphome::skyscope
