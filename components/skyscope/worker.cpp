// The worker task: the planes from the ADS-B exchange, and the map's tiles into the base frame. Everything here may
// take seconds (a TLS handshake, a slow tile server), so none of it runs on ESPHome's loop.
#include "skyscope.h"

#include "decode.h"
#include "http.h"
#include "ui.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>
#include <list>

namespace esphome::skyscope {
static const char *const TAG = "skyscope.worker";

namespace {
// ArduinoJson's memory in PSRAM: a reply of two hundred planes takes some 100 KB while it is read.
struct SpiRam : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
SpiRam spiram;

// Tiles as they came (PNG or JPEG bytes), in PSRAM, the least recently drawn going first. A tile the server has none
// of (a chart outside its country) is kept as empty, so it is not asked for again and again.
constexpr size_t CACHE_BYTES = 3 * 1024 * 1024;
constexpr uint32_t TILE_TTL_MS = 24 * 3600 * 1000;
constexpr size_t MOST_TILE_BYTES = 512 * 1024, MOST_FEED_BYTES = 768 * 1024;
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
};

// How long a source that failed is left alone: a feed that says "too many requests" longer.
constexpr uint32_t FEED_BACKOFF_MS = 30000, FEED_LIMITED_MS = 120000, FEED_LIMITED_MOST_MS = 600000;
constexpr uint32_t TILE_BACKOFF_MS = 60000;
}  // namespace

void Skyscope::worker_main(void *self) { static_cast<Skyscope *>(self)->worker_(); }

void Skyscope::worker_() {
  Cache cache;
  http::Body body;
  Settings settings;
  tiles::Plan plan;
  uint32_t settings_rev = 0, plan_rev = 0, painting_rev = 0;
  size_t next_tile = 0;
  uint32_t feed_due = 0, tiles_quiet_until = 0;
  std::string feed_key;  // what the last feed request asked for: a new place or range asks at once
  int limited = 0;       // "too many requests" in a row: each waits twice as long as the one before
  uint32_t paced_ms = 0; // the fewest ms between two requests the feed's own limits allow (x-ratelimit-limit)
  const int width = width_, height = height_;
  while (true) {
    bool online;
    {
      std::lock_guard<std::mutex> guard(lock_);
      online = shared_.online;
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

    // ---- The planes, every refresh_s, or at once for a new place, range or feed.
    const int radius_nm = std::max(1, (int) std::ceil(plan.view.reach_metres() / geo::NM * 1.05));
    const std::string url = feed::url(settings.feed, settings.lat, settings.lon, radius_nm);
    if (url != feed_key) {
      // Another feed or place: asked at once. Its limits are its own.
      if (feed_key.rfind(url.substr(0, url.find('/', 8)), 0) != 0) paced_ms = 0;
      feed_due = now;
    }
    if ((int32_t) (now - feed_due) >= 0) {
      feed_key = url;
      const uint32_t asked = millis();
      ui::worker_phase = ui::FEED_FETCH;
      const http::Result r = http::get(url, body, MOST_FEED_BYTES, agent_, "application/json");
      const uint32_t fetched = millis();
      ui::worker_phase = ui::FEED_PARSE;
      std::vector<feed::Flight> flights;
      std::string error = r.error;
      bool ok = r.status == 200;
      if (ok) {
        JsonDocument doc(&spiram);
        ok = feed::parse(reinterpret_cast<const char *>(body.data), body.size, doc, flights, error);
      }
      const uint32_t done = millis();
      ui::worker_phase = ui::IDLE;
      {
        std::lock_guard<std::mutex> guard(lock_);
        shared_.fetch_ms = fetched - asked;
        shared_.parse_ms = done - fetched;
        shared_.reply_bytes = body.size;
        if (ok) {
          shared_.flights = std::move(flights);
          shared_.flights_rev++;
          shared_.feed_at = done;
          shared_.feed_error.clear();
        } else {
          shared_.feed_error = error;
        }
      }
      auto in_words = [](uint32_t ms) {
        char out[24];
        if (ms >= 90 * 60000) std::snprintf(out, sizeof(out), "%u h", (unsigned) ((ms + 1800000) / 3600000));
        else std::snprintf(out, sizeof(out), "%u min", (unsigned) ((ms + 59999) / 60000));
        return std::string(out);
      };
      if (!r.rate_limit.empty()) paced_ms = feed::min_interval_ms(r.rate_limit);
      if (ok) {
        limited = 0;
        // As often as asked, but never more often than the feed's own limits allow (adsb.lol: 500 a day).
        const uint32_t every = std::max<uint32_t>(settings.refresh_s * 1000, paced_ms);
        feed_due = done + every;
        std::lock_guard<std::mutex> guard(lock_);
        shared_.feed_every_ms = every;
      } else if ((r.status == 429 || r.status == 403) && r.rate_reset_s > 0) {
        // The feed says when its quota comes back: then, and not before.
        const uint32_t wait = (uint32_t) std::min(r.rate_reset_s, 24 * 3600) * 1000 + 5000;
        error = "limit reached, again in " + in_words(wait);
        ESP_LOGW(TAG, "%s: %s (its limits: %s)", feed::credit(settings.feed), error.c_str(), r.rate_limit.c_str());
        feed_due = done + wait;
        ++limited;
        std::lock_guard<std::mutex> guard(lock_);
        shared_.feed_error = error;
      } else {
        uint32_t wait = FEED_BACKOFF_MS;
        if (r.status == 429 || r.status == 403) {
          // The exchange asks for fewer requests: as long as it says (Retry-After), else 2, 4, 8 minutes, at most 10.
          wait = std::min<uint32_t>(FEED_LIMITED_MOST_MS, FEED_LIMITED_MS << std::min(limited, 3));
          if (r.retry_after_s > 0) wait = std::max<uint32_t>(wait / 4, r.retry_after_s * 1000);
          ++limited;
          error = "too many requests, again in " + in_words(wait);
        }
        ESP_LOGW(TAG, "%s: %s", feed::credit(settings.feed), error.c_str());
        feed_due = done + wait;
        std::lock_guard<std::mutex> guard(lock_);
        shared_.feed_error = error;
      }
      body.clear();
      if (body.room > 256 * 1024) body.release();  // a large reply's memory goes back
    }

    // ---- The map, one tile a turn, so the planes keep coming while it fills.
    if (painting_rev != plan_rev) {
      painting_rev = plan_rev;
      next_tile = 0;
      {
        std::lock_guard<std::mutex> frame(frame_lock_);
        std::fill(base_frame_, base_frame_ + width * height, (uint16_t) 0);
      }
      std::lock_guard<std::mutex> guard(lock_);
      shared_.frame_rev++;
      shared_.frame_plan_rev = plan_rev;
      shared_.tiles_done = 0;
      shared_.tiles_total = plan.keys.size();
      shared_.tile_error.clear();
    }
    if (next_tile < plan.keys.size() && (int32_t) (millis() - tiles_quiet_until) >= 0) {
      const tiles::Key key = plan.keys[next_tile];
      Cached *tile = cache.find(key, millis());
      std::string error;
      if (!tile) {
        ui::worker_phase = ui::TILE_FETCH;
        const http::Result r = http::get(tiles::url(settings, key), body, MOST_TILE_BYTES, agent_, "image/png,image/jpeg");
        ui::worker_phase = ui::IDLE;
        if (r.status == 200) tile = cache.keep(key, body.data, body.size, millis());
        // No tile there (outside a chart's country): remembered as empty, drawn as the ground.
        else if (r.status == 404 || r.status == 204) tile = cache.keep(key, nullptr, 0, millis());
        else error = r.error;
        body.clear();
      }
      bool failed = !tile;
      if (tile && tile->size) {
        const geo::TileOrigin at = geo::tile_origin(plan.view, key.z, key.x, key.y);
        std::lock_guard<std::mutex> frame(frame_lock_);
        ui::worker_phase = ui::TILE_DECODE;
        failed = !decode::draw(tile->data, tile->size, base_frame_, width, height, at, tiles::filter_of(settings), error);
        ui::worker_phase = ui::IDLE;
      }
      if (failed && !error.empty()) ESP_LOGW(TAG, "Tile %u/%u/%u: %s", key.z, (unsigned) key.x, (unsigned) key.y, error.c_str());
      if (failed && !tile) {
        // The server gave nothing: try this tile again after a while, and say so on the glass.
        tiles_quiet_until = millis() + TILE_BACKOFF_MS;
      } else {
        ++next_tile;
      }
      std::lock_guard<std::mutex> guard(lock_);
      if (shared_.frame_plan_rev == plan_rev) {
        if (failed) {
          shared_.tile_error = error.empty() ? std::string("tile server") : error;
        } else {
          shared_.tiles_done++;
          shared_.tile_error.clear();
        }
        shared_.frame_rev++;
      }
      continue;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}  // namespace esphome::skyscope
