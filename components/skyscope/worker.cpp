// The worker task: the planes from the ADS-B exchange, the map's tiles into the base frame, and the places typed in the
// menu (places.h). Everything here may take seconds (a TLS handshake, a slow tile server), so none of it runs on
// ESPHome's loop.
#include "skyscope.h"

#include "decode.h"
#include "http.h"
#include "radar.h"
#include "colours.h"
#include "ui.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <ctime>
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
// The radio's planes are in memory: taken every second while it listens; one with no stick is looked at again soon.
constexpr uint32_t RADIO_EVERY_MS = 1000;
constexpr int RADIO_RETRY_S = 5;
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
  void clear() {
    while (!items_.empty()) drop_(items_.begin());
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
  size_t bytes() const { return bytes_; }
};

// How long a source that failed is left alone: a feed that says "too many requests" longer.
// A tile that did not come is asked again after 2 s, then 4, 8 ... at most 60, while the others go on loading; one
// that came but could not be read is fetched again at most this many times.
constexpr uint32_t TILE_RETRY_FIRST_MS = 2000, TILE_RETRY_MOST_MS = 60000;
// The FAA's airspace server is shared by everyone who asks it: a request that failed waits 30 s, then twice as long
// each time it fails again, at most 15 minutes.
constexpr uint32_t AIRSPACE_RETRY_FIRST_MS = 30000, AIRSPACE_RETRY_MOST_MS = 15 * 60000;
constexpr int TILE_UNREADABLE_MOST = 3;
// The map card (card.h): a download asks a tile every DOWNLOAD_GAP_MS at most, only while the map shown is whole; a
// tile that fails waits 2 s, then twice as long each time, at most 5 minutes, and after DOWNLOAD_TRIES is left out.
// Its record on the card is written every DOWNLOAD_SAVE_EVERY tiles. Nothing more is written to a card with less than
// CARD_RESERVE_BYTES free, and its free space is looked at again every CARD_SPACE_EVERY tiles written.
constexpr uint32_t DOWNLOAD_GAP_MS = 400, DOWNLOAD_RETRY_FIRST_MS = 2000, DOWNLOAD_RETRY_MOST_MS = 5 * 60000;
constexpr int DOWNLOAD_TRIES = 6;
constexpr size_t DOWNLOAD_SAVE_EVERY = 25, DOWNLOAD_SKIPS_A_TURN = 64;
constexpr uint64_t CARD_RESERVE_BYTES = 64ULL * 1024 * 1024;
constexpr uint32_t CARD_SPACE_EVERY = 100;
// Tiles cleared are deleted this many files at a turn, while the map shown is whole; a turn that deletes none (a file
// the card will not let go) counts, and after CLEAR_STALLS of them the rest is left.
constexpr int CLEAR_A_TURN = 24, CLEAR_STALLS = 50;
const std::string JOB_PATH = std::string(card::ROOT) + "/tiles/job.txt";
// The clock's Unix seconds for the card's tiles, 0 while it is not set (no network has set it yet).
uint32_t unix_now() {
  const time_t t = ::time(nullptr);
  return t > 1600000000 ? (uint32_t) t : 0;
}

struct Pending {
  size_t index;          // into the plan's keys
  uint32_t due = 0;      // millis() when it may be asked again
  int failures = 0, unreadable = 0;
};

// The places a search typed in the menu finds (places.h): an airport by its code, and nothing more when there is one;
// else airports by their name and OpenStreetMap's places. Nominatim is asked at most once a second, however quickly
// searches come; `nominatim_at` is when it was last asked.
std::vector<places::Found> find_places(const std::string &q, http::Body &body, const std::string &agent,
                                       uint32_t &nominatim_at, std::string &error) {
  using namespace places;
  std::vector<Found> named, osm;
  JsonDocument doc;
  std::string why;
  auto ask = [&](const std::string &url) {
    body.clear();
    const http::Result r = http::get(url, body, MOST_REPLY_BYTES, agent, "application/json");
    if (r.status != 200 && why.empty()) why = "The search had no answer (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + ")";
    return r.status == 200;
  };
  const char *text = nullptr;
  const std::string code = code_url(q);
  if (!code.empty() && ask(code)) {
    text = reinterpret_cast<const char *>(body.data);
    if (!read_faa(text, body.size, doc, named, why)) named.clear();
    if (!named.empty()) {
      named.resize(1);
      return named;
    }
  }
  const std::string by_name = named_url(q);
  if (!by_name.empty() && ask(by_name)) {
    text = reinterpret_cast<const char *>(body.data);
    if (!read_faa(text, body.size, doc, named, why)) named.clear();
  }
  const uint32_t since = millis() - nominatim_at;
  if (nominatim_at && since < 1100) vTaskDelay(pdMS_TO_TICKS(1100 - since));
  nominatim_at = millis() | 1;
  if (ask(nominatim_url(q))) {
    text = reinterpret_cast<const char *>(body.data);
    read_nominatim(text, body.size, q, doc, osm, why);
  }
  body.clear();
  std::vector<Found> all = in_order(named, osm);
  if (all.empty()) error = why;
  return all;
}

// The weather for the weather page (weather.h): the station set, or the nearest to the place that reports, its METAR,
// TAF and MOS, and the advisories and pilot reports over the map. A part that fails leaves the rest.
std::shared_ptr<weather::Report> find_weather(const Settings &settings, http::Body &body, const std::string &agent) {
  using namespace weather;
  auto report = std::make_shared<Report>();
  JsonDocument doc;
  auto note = [&](const std::string &e) {
    if (report->error.empty()) report->error = e;
  };
  // A reply in `body`, or why not.
  auto ask = [&](const std::string &url) {
    body.clear();
    const http::Result r = http::get(url, body, MOST_REPLY_BYTES, agent, "application/json");
    if (r.status == 200 || r.status == 204) {
      if (r.status == 204) body.clear();
      return true;
    }
    note("No answer from the weather service (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + ")");
    return false;
  };
  auto text = [&]() { return reinterpret_cast<const char *>(body.data); };
  std::string error;
  std::vector<Metar> metars;
  const std::string set = station_id(settings.weather_station);
  if (ask(set.empty() ? nearby_url(settings.lat, settings.lon) : metar_url(set))) {
    if (!read_metars(text(), body.size, doc, metars, error)) note(error);
    const int i = set.empty() ? nearest(metars, settings.lat, settings.lon) : (metars.empty() ? -1 : 0);
    if (i >= 0) {
      report->has_metar = true;
      report->metar = metars[i];
      report->station = metars[i].id;
    } else if (error.empty()) {
      note(set.empty() ? "No station reports near the place: set one in the menu" : "No METAR from " + set);
    }
  }
  if (report->station.empty()) report->station = set;
  if (!report->station.empty()) {
    if (ask(taf_url(report->station)) && !read_taf(text(), body.size, doc, report->taf, error)) note(error);
    // The MOS is for stations of the United States: none elsewhere is no error.
    body.clear();
    const http::Result r = http::get(mos_url(report->station), body, MOST_REPLY_BYTES, agent, "application/json");
    if (r.status == 200) read_mos(text(), body.size, doc, report->mos, error);
  }
  // As far as the planes come from at most: a range out over an ocean asks no more of the weather than that.
  const int range_nm = std::min(settings.range_nm, FEED_REACH_NM);
  const double reach = std::max(50, range_nm);
  if (ask(airsigmet_url()) &&
      !read_sigmets(text(), body.size, doc, settings.lat, settings.lon, reach, report->advisories, error))
    note(error);
  if (ask(gairmet_url()) &&
      !read_gairmets(text(), body.size, doc, settings.lat, settings.lon, reach, report->advisories, error))
    note(error);
  if (ask(pirep_url(settings.lat, settings.lon, range_nm)) &&
      !read_pireps(text(), body.size, doc, report->pireps, error))
    note(error);
  body.clear();
  if (body.room > 256 * 1024) body.release();
  return report;
}
}  // namespace

void Skyscope::worker_main(void *self) { static_cast<Skyscope *>(self)->worker_(); }

void Skyscope::worker_() {
  Cache cache;
  http::Body body;
  Settings settings;
  tiles::Plan plan;
  uint32_t settings_rev = 0, plan_rev = 0, painting_rev = 0;
  std::vector<Pending> pending;  // the plan's tiles not drawn yet, in its order (nearest the middle first)
  // What the frame being drawn shows (tiles.h frame_key), and whether a tile of it was given up; the frame kept, what
  // it shows and when it was drawn. Counters for /api/status.
  std::string painting_key, kept_key;
  bool gave_up = false;
  uint32_t kept_at = 0, tiles_fetched = 0, tiles_from_cache = 0, frames_again = 0;
  uint64_t tile_bytes_fetched = 0;
  // The radar (radar.h): the latest scan and when to ask again; what the map kept without it shows (frame_key);
  // whether the frame has a scan on it, which, which of the loop's, and when the next is due.
  uint32_t radar_latest = 0, radar_check_due = 0, radar_shown = 0, radar_step_due = 0;
  size_t radar_index = 0;
  std::string clean_key;
  bool radar_drawn = false, radar_looping = false;
  std::vector<radar::Overlay> overlays;  // what each scan drawn over the clean map changed (radar.h)
  uint32_t feed_due = 0;
  std::string feed_key;  // the place, range and sources the last request was for: a new one asks at once
  std::string receiver_url;  // the receiver's address the last request was for
  providers::Providers providers;
  std::string airspace_asked;  // the requests (airspace.h Ask) the airspace shown answered
  uint32_t airspace_due = 0, airspace_wait = 0;
  const int width = width_, height = height_;
  uint32_t search_rev = 0, nominatim_at = 0, weather_ask = 0;
  bool was_offline = false, was_card = false;
  // The plane followed (follow.h): the settings before the view's follow (`settings` is them as shown), the view's
  // middle the loop gave, the plane as the reports have seen it and for which follow, the asks still to make for it and
  // when the next is due. exchange_at: when an exchange was last asked, so two requests are never too close.
  Settings saved;
  follow::Centre centre;
  uint32_t centre_rev = 0;
  follow::Follower follower;
  std::string follow_for, follow_hex_for;
  std::vector<follow::Ask> follow_asks;
  size_t follow_next = 0;
  uint32_t follow_due = 0, exchange_at = 0;
  // Its route (follow.h, a build with routes): the callsign it was asked for, and when it may be asked again.
  std::string route_for;
  uint32_t route_due = 0;
  auto publish_follower = [&]() {
    std::lock_guard<std::mutex> guard(lock_);
    shared_.follower = follower;
    shared_.follower_for = follow_for;
    shared_.follower_rev++;
  };
  // Waits out the gap an exchange asks between two requests, from the last one to either.
  auto space_out = [&]() {
    const uint32_t since = millis() - exchange_at;
    if (exchange_at && since < follow::ASK_GAP_MS) vTaskDelay(pdMS_TO_TICKS(follow::ASK_GAP_MS - since));
  };

  // ---- The map card: mounted at the start, and a download left unfinished (by a restart) carried on.
  uint32_t download_ask = 0, stop_ask = 0, mount_ask = 0, clear_ask = 0, format_ask = 0;
  card::Plan job;
  bool downloading = false;
  int job_failures = 0;
  uint32_t job_due = 0, card_writes = 0;
  bool card_room = true;
  int clear_stalls = 0;
  Shared::Card card_state;
  auto card_space = [&]() {
    uint64_t total = 0, free = 0;
    uint32_t cluster = 0;
    if (card_io::space(total, free, cluster)) {
      card_state.total = total;
      card_state.free = free;
      card_state.cluster = cluster;
      card_room = free >= CARD_RESERVE_BYTES;
    }
  };
  auto publish_card = [&]() {
    card_state.mounted = card_io::mounted();
    card_state.downloading = downloading;
    card_state.area = job.area;
    card_state.total_tiles = job.total;
    std::lock_guard<std::mutex> guard(lock_);
    const int missing = shared_.card.missing;  // the tile loop's own
    shared_.card = card_state;
    shared_.card.missing = missing;
  };
  auto save_job = [&]() {
    if (downloading) card_io::write_text(JOB_PATH, card::job_text(job.area, card_state.next));
  };
  auto stop_job = [&](const std::string &why) {
    downloading = false;
    card_state.download_error = why;
    card_state.waiting_s = 0;
    card_io::remove(JOB_PATH);
  };
  auto start_job = [&](const card::Area &area, size_t next) {
    std::string error;
    if (!card_io::mounted()) error = "No map card";
    else if (!card::plan(area, job, error)) job.area = area;
    if (!error.empty()) {
      downloading = false;
      card_state.download_error = error;
      return;
    }
    downloading = true;
    job_failures = 0;
    job_due = millis();
    card_state.next = std::min(next, job.total);
    card_state.fetched = card_state.skipped = card_state.given_up = 0;
    card_state.download_error.clear();
    save_job();
    ESP_LOGI(TAG, "Download: %s, %d NM, zoom %d, %u tiles from %u", card::map_of(area.style), area.radius_nm,
             area.z_max, (unsigned) job.total, (unsigned) card_state.next);
  };
  auto mount_card = [&]() {
    std::string error;
    const bool mounted = card_io::mount(card_config_, error);
    card_state.unformatted = card_io::unformatted();
    if (mounted) {
      card_state.error.clear();
      card_space();
      // Tiles cleared before a restart, not all deleted yet.
      card_state.clearing = card_io::remove_some(card::trash_path(), 0) >= 0;
      std::string text;
      card::Area area;
      size_t next = 0;
      if (!downloading && card_io::read_text(JOB_PATH, text) && card::read_job(text.c_str(), area, next))
        start_job(area, next);
    } else {
      card_state.error = error;
    }
    publish_card();
  };
  if (card_fitted_) mount_card();

  while (true) {
    bool online, updating;
    std::string search;
    bool connected, wants_weather = false;
    bool new_download = false, stop_download = false, remount = false, clear_tiles = false, format_card = false;
    card::Area download;
    {
      std::lock_guard<std::mutex> guard(lock_);
      updating = shared_.updating;
      if (shared_.download_ask != download_ask) {
        download_ask = shared_.download_ask;
        download = shared_.download;
        new_download = true;
      }
      if (shared_.stop_ask != stop_ask) {
        stop_ask = shared_.stop_ask;
        stop_download = true;
      }
      if (shared_.mount_ask != mount_ask) {
        mount_ask = shared_.mount_ask;
        remount = true;
      }
      if (shared_.clear_ask != clear_ask) {
        clear_ask = shared_.clear_ask;
        clear_tiles = true;
      }
      if (shared_.format_ask != format_ask) {
        format_ask = shared_.format_ask;
        format_card = true;
      }
      // The screen off counts as offline: nobody sees planes or tiles, so none are asked for. So does an update: its
      // download has the network to itself. A screen running on its radio alone (offline.h) needs no network.
      const bool radio_only = shared_.offline;
      online = (shared_.online || radio_only) && !shared_.asleep && !shared_.updating;
      connected = shared_.online;
      // Off a network, the settings as the loop shows them: the radio's planes over the card's map or the radar,
      // nothing to fetch.
      const bool has_card = card_io::mounted();
      if (radio_only != was_offline || has_card != was_card) {
        was_offline = radio_only;
        was_card = has_card;
        settings_rev = 0;
      }
      if (shared_.search_rev != search_rev) {
        search = shared_.search;
        search_rev = shared_.search_rev;
      }
      if (shared_.weather_ask != weather_ask) {
        weather_ask = shared_.weather_ask;
        wants_weather = true;
      }
      bool reshow = false;
      if (shared_.settings_rev != settings_rev) {
        saved = radio_only ? offline::shown(shared_.settings, has_card) : shared_.settings;
        settings_rev = shared_.settings_rev;
        reshow = true;
      }
      if (shared_.centre_rev != centre_rev) {
        centre = shared_.centre;
        centre_rev = shared_.centre_rev;
        reshow = true;
      }
      if (reshow) settings = follow::shown(saved, radio_only ? follow::Centre{} : centre);
      if (shared_.plan_rev != plan_rev) {
        plan = shared_.plan;
        plan_rev = shared_.plan_rev;
      }
    }
    // ---- A place to find, typed in the menu: before anything else, as someone is waiting for it, and with no place
    // set yet (it is how one is set).
    if (!search.empty()) {
      std::vector<places::Found> found;
      std::string error;
      if (connected) found = find_places(search, body, agent_, nominatim_at, error);
      else error = "The screen is not on a network";
      if (found.empty() && error.empty()) error = "Nothing found";
      ESP_LOGI(TAG, "Search: %u places found", (unsigned) found.size());
      std::lock_guard<std::mutex> guard(lock_);
      shared_.found = std::move(found);
      shared_.search_error = error;
      shared_.found_rev = search_rev;
    }
    // ---- The weather, when the weather page asks: someone is looking at it.
    if (wants_weather) {
      std::shared_ptr<weather::Report> report;
      if (!connected) {
        report = std::make_shared<weather::Report>();
        report->error = "The screen is not on a network";
      } else if (!settings.located) {
        report = std::make_shared<weather::Report>();
        report->error = "No place is set";
      } else {
        report = find_weather(settings, body, agent_);
      }
      ESP_LOGI(TAG, "Weather: %s%s", report->has_metar ? "a METAR" : "no METAR",
               report->error.empty() ? "" : (", " + report->error).c_str());
      std::lock_guard<std::mutex> guard(lock_);
      shared_.weather = std::move(report);
      shared_.weather_rev = weather_ask;
    }
    // ---- The map card: looked for again when the page asks, and a download started or stopped.
    if (remount && card_fitted_ && !card_io::mounted()) mount_card();
    if (stop_download && downloading) {
      ESP_LOGI(TAG, "Download stopped at %u of %u", (unsigned) card_state.next, (unsigned) job.total);
      stop_job("");
      publish_card();
    }
    if (new_download) {
      start_job(download, 0);
      publish_card();
    }
    // Formatted: everything on the card goes, a card with no FAT32 on it too. The tiles in memory go with it.
    if (format_card && card_fitted_) {
      if (downloading) stop_job("");
      std::string error;
      const bool ok = card_io::mounted() ? card_io::format(error) : card_io::mount(card_config_, error, true);
      card_state.unformatted = !ok && card_io::unformatted();
      card_state.error = ok ? std::string() : error;
      if (ok) {
        cache.clear();
        card_state.clearing = false;
        card_state.cleared = 0;
        card_state.total_tiles = 0;
        job = card::Plan{};
        card_space();
        ESP_LOGI(TAG, "Map card formatted: %u MB free", (unsigned) (card_state.free / (1024 * 1024)));
      }
      publish_card();
    }
    // Cleared: the folder of tiles moved aside at once (a new empty one in its place), its files deleted a few at a
    // time from here on. A download under way stops: its record was among them.
    if (clear_tiles && card_io::mounted()) {
      if (downloading) stop_job("");
      const std::string tiles_folder = std::string(card::ROOT) + "/tiles";
      if (card_io::make_folder(card::trash_path()) &&
          card_io::rename(tiles_folder, card::trash_name(millis() ^ (uint32_t) esp_timer_get_time()))) {
        card_io::make_folder(tiles_folder);
        cache.clear();
        card_state.clearing = true;
        clear_stalls = 0;
        ESP_LOGI(TAG, "Map card: the tiles cleared");
      } else {
        card_state.error = "the tiles could not be cleared";
      }
      publish_card();
    }
    if (card_state.clearing && card_io::mounted() && (pending.empty() || !online)) {
      const int n = card_io::remove_some(card::trash_path(), CLEAR_A_TURN);
      if (n > 0) {
        card_state.cleared += n;
        clear_stalls = 0;
      }
      if (n < 0 || (n == 0 && ++clear_stalls >= CLEAR_STALLS)) {
        ESP_LOGI(TAG, "Map card: %u old files and folders deleted%s", (unsigned) card_state.cleared,
                 n < 0 ? "" : ", some would not go");
        card_state.clearing = false;
        card_space();
      }
      if (n != 0) publish_card();
    }
    // A tile of the download, while on a network and the map shown is whole (the screen asleep too: it was asked for).
    if (downloading && connected && !updating && (pending.empty() || !online) && (int32_t) (millis() - job_due) >= 0) {
      const uint32_t clock = unix_now();
      // What the card has already, and still good, is skipped: several at a turn, as they cost no request.
      size_t skips = 0;
      card::Header have;
      while (card_state.next < job.total && skips < DOWNLOAD_SKIPS_A_TURN &&
             card_io::read_header(job.at(card_state.next), have) && card::fresh(have, clock)) {
        ++card_state.next;
        ++card_state.skipped;
        ++skips;
      }
      if (!card_io::mounted()) {
        stop_job("The map card is gone");
      } else if (!card_room) {
        stop_job("The map card is full");
      } else if (card_state.next < job.total && !skips) {
        const tiles::Key key = job.at(card_state.next);
        body.clear();
        ui::worker_phase = ui::TILE_FETCH;
        const http::Result r = http::get(tiles::url(settings, key), body, MOST_TILE_BYTES, agent_, "image/png,image/jpeg");
        ui::worker_phase = ui::IDLE;
        const bool empty = r.status == 404 || r.status == 204;
        if (r.status == 200 || empty) {
          const card::Header h = card::header_for(clock, card::lifetime_s(key.style, r.max_age_s), empty);
          if (card_io::write(key, h, empty ? nullptr : body.data, empty ? 0 : body.size)) {
            ++card_state.written;
            if (++card_writes % CARD_SPACE_EVERY == 0) card_space();
          } else {
            ++card_state.failed;
          }
          if (r.status == 200) {
            ++tiles_fetched;
            tile_bytes_fetched += body.size;
          }
          ++card_state.next;
          ++card_state.fetched;
          job_failures = 0;
          card_state.waiting_s = 0;
          job_due = millis() + DOWNLOAD_GAP_MS;
        } else {
          // The server is busy or the network down: the same tile again later, then on without it.
          ++job_failures;
          uint32_t wait =
              std::min<uint32_t>(DOWNLOAD_RETRY_MOST_MS, DOWNLOAD_RETRY_FIRST_MS << std::min(job_failures - 1, 8));
          wait = std::max<uint32_t>(wait, (uint32_t) r.retry_after_s * 1000);
          ESP_LOGW(TAG, "Download %u/%u/%u: %s, again in %u s", key.z, (unsigned) key.x, (unsigned) key.y,
                   r.error.c_str(), (unsigned) (wait / 1000));
          card_state.download_error = r.error;
          if (job_failures >= DOWNLOAD_TRIES) {
            ++card_state.next;
            ++card_state.given_up;
            job_failures = 0;
          }
          card_state.waiting_s = wait / 1000;
          job_due = millis() + wait;
        }
        body.clear();
      }
      if (downloading && card_state.next >= job.total) {
        ESP_LOGI(TAG, "Download done: %u fetched, %u on the card already, %u left out", (unsigned) card_state.fetched,
                 (unsigned) card_state.skipped, (unsigned) card_state.given_up);
        stop_job("");
        card_space();
      } else if (downloading && (card_state.next % DOWNLOAD_SAVE_EVERY == 0 || skips)) {
        save_job();
      }
      publish_card();
    }

    // ---- The plane followed: a new one looked for afresh; until it is found, each way of asking for it in turn and
    // then again a while later; once found, by its hex whenever the reports round it have not had it for a while.
    if (saved.follow != follow_for ||
        (!saved.follow_hex.empty() && saved.follow_hex != follow_hex_for && saved.follow_hex != follower.hex())) {
      follow_for = saved.follow;
      follow_hex_for = saved.follow_hex;
      follower.reset();
      follow_asks = follow::asks(saved.follow, saved.follow_hex);
      follow_next = 0;
      follow_due = millis();
      if (!follow_for.empty()) ESP_LOGI(TAG, "Following %s: %u ways to ask", follow_for.c_str(), (unsigned) follow_asks.size());
      route_for.clear();
      route_due = 0;
      {
        std::lock_guard<std::mutex> guard(lock_);
        shared_.route = follow::Route{};
      }
      publish_follower();
    }
    if (!follow_for.empty() && online && connected && !was_offline &&
        (int32_t) (millis() - follow_due) >= 0) {
      const bool looking = !follower.known();
      const uint32_t at = millis();
      if (!looking && follower.age_ms(at) < follow::HEX_EVERY_MS) {
        // The reports round it have it: nothing to ask.
        follow_due = at + follow::HEX_EVERY_MS - follower.age_ms(at);
      } else if (looking && follow_asks.empty()) {
        follow_due = at + follow::LOOK_EVERY_MS;
      } else {
        const follow::Ask ask = looking ? follow_asks[follow_next] : follow::Ask{follow::BY_HEX, follower.hex()};
        Feed exchange = ADSB_LOL;
        bool free = false;
        for (const Feed f : follow::lookup_feeds(saved.feed)) {
          const providers::State &st = providers.state(f);
          if (!st.blocked || !providers::before(at, st.until)) {
            exchange = f;
            free = true;
            break;
          }
        }
        if (!free) {
          follow_due = at + 5000;  // both exchanges are waiting: soon again
        } else {
          space_out();
          ui::worker_phase = ui::FEED_FETCH;
          const http::Result r = http::get(follow::url(exchange, ask), body, MOST_FEED_BYTES, agent_, "application/json");
          ui::worker_phase = ui::IDLE;
          exchange_at = millis() | 1;
          std::vector<feed::Flight> flights;
          std::string error = r.error;
          JsonDocument doc;
          const bool ok = r.status == 200 &&
                          feed::parse(reinterpret_cast<const char *>(body.data), body.size, doc, flights, error, 16);
          body.clear();
          if (!ok) {
            providers.failed(exchange, millis(), providers::Answer{r.status, r.retry_after_s, r.rate_reset_s, r.rate_limit, error});
            ESP_LOGW(TAG, "Following %s: %s did not answer (%s)", follow_for.c_str(), feed::credit(exchange), error.c_str());
            follow_due = millis() + 5000;
          } else {
            const feed::Flight *found = follow::pick(flights);
            if (found && (looking || found->hex == follower.hex())) {
              if (looking) ESP_LOGI(TAG, "Following %s: found as %s", follow_for.c_str(), found->hex.c_str());
              follower.seen(*found, millis());
              publish_follower();
            }
            if (looking && !found && ++follow_next >= follow_asks.size()) {
              follow_next = 0;
              follow_due = millis() + follow::LOOK_EVERY_MS;
              ESP_LOGI(TAG, "Following %s: not up now, asked again in %u s", follow_for.c_str(),
                       (unsigned) (follow::LOOK_EVERY_MS / 1000));
            } else if (!looking || found) {
              follow_due = millis() + follow::HEX_EVERY_MS;
            }
          }
          std::lock_guard<std::mutex> guard(lock_);
          shared_.providers = providers;
        }
      }
    }

#ifdef SKYSCOPE_ROUTES
    // ---- Its route: asked once for each callsign it flies under, once it is found; again ten minutes after a failure.
    if (follower.known() && !follower.last().callsign.empty() && follower.last().callsign != route_for && online &&
        connected && !was_offline && (int32_t) (millis() - route_due) >= 0) {
      follow::Route route;
      std::string error;
      body.clear();
      const http::Result r = http::get(follow::route_url(follower.last().callsign), body, 16 * 1024, agent_,
                                       "application/json");
      JsonDocument doc;
      const bool ok = (r.status == 200 || r.status == 404) &&
                      follow::read_route(reinterpret_cast<const char *>(body.data), body.size, doc, route, error);
      body.clear();
      if (ok) {
        route.callsign = route_for = follower.last().callsign;
        ESP_LOGI(TAG, "Following %s: %s", route_for.c_str(),
                 route.known ? (route.from + " to " + route.to).c_str() : "no route known");
        std::lock_guard<std::mutex> guard(lock_);
        shared_.route = route;
        shared_.follower_rev++;
      } else {
        route_due = millis() + 10 * 60 * 1000;
        ESP_LOGW(TAG, "Route: %s", (r.error.empty() ? error : r.error).c_str());
      }
    }
    if (follow_for.empty() && !route_for.empty()) {
      route_for.clear();
      std::lock_guard<std::mutex> guard(lock_);
      shared_.route = follow::Route{};
    }
#endif

    const uint32_t now = millis();
    if (!online || !settings.located || plan.view.width == 0) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }

    // ---- The planes, every refresh_s, or at once for a new place or range, from the first exchange free
    // (providers.h): the preferred one, or while it waits the next in the list.
    providers.set_order(settings.feed, settings.fallback, !settings.receiver_url.empty(), radio_fitted);
    // The plane followed, wherever a report has it: seen again, by its hex.
    auto see_followed = [&](const std::vector<feed::Flight> &flights) {
      if (!follower.known()) return;
      for (const auto &f : flights)
        if (f.hex == follower.hex()) {
          follower.seen(f, millis());
          publish_follower();
          return;
        }
    };
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
      } else if (source == RADIO) {
        // The screen's own radio: the planes it heard, straight from memory.
        std::vector<feed::Flight> flights;
        providers::Answer answer;
        bool ok = false;
#ifdef USE_SKYSCOPE_RADIO
        if (radio_) {
          radio_->set_reference(settings.lat, settings.lon);
          ok = radio_->streaming();
          if (ok) flights = radio_->flights(millis());
          else answer.error = radio_->state();
        }
#endif
        if (!ok && answer.error.empty()) answer.error = "no radio";
        const uint32_t done = millis();
        if (ok) {
          see_followed(flights);
          overlay::keep_showable(flights, settings, plan.view, KEEP_EXTRA);
          const uint32_t every = providers.succeeded(RADIO, done, RADIO_EVERY_MS, answer);
          feed_due = done + every;
          std::lock_guard<std::mutex> guard(lock_);
          shared_.flights = std::move(flights);
          shared_.flights_rev++;
          shared_.feed_at = done;
          shared_.feed_every_ms = every;
          shared_.feed_source = RADIO;
        } else {
          // No stick, or one not yet listening: the next source at once, and the radio again in a few seconds.
          answer.retry_after_s = RADIO_RETRY_S;
          providers.failed(RADIO, done, answer);
          feed_due = done;
        }
      } else {
        // Following a plane: the planes round it, out to the view's farthest corner from it, as far as the exchanges go.
        double ask_lat = settings.lat, ask_lon = settings.lon;
        int ask_nm = radius_nm;
        if (centre.on && follower.known()) {
          const geo::Place p = follower.where(millis());
          ask_lat = p.lat;
          ask_lon = p.lon;
          ask_nm = follow::reach_nm(plan.view, p.lat, p.lon);
        }
        const std::string url = feed::url(source, ask_lat, ask_lon, ask_nm, settings.receiver_url);
        if (source != RECEIVER) space_out();
        const uint32_t asked = millis();
        ui::worker_phase = ui::FEED_FETCH;
        const http::Result r = http::get(url, body, MOST_FEED_BYTES, agent_, "application/json");
        const uint32_t fetched = millis();
        if (source != RECEIVER) exchange_at = fetched | 1;
        ui::worker_phase = ui::FEED_PARSE;
        std::vector<feed::Flight> flights;
        std::string error = r.error;
        bool ok = r.status == 200;
        if (ok) {
          JsonDocument doc;  // one plane at a time (feed.h): a kilobyte or two
          // As many planes as the largest piece of PSRAM holds, with as much again to spare for their words.
          const size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / sizeof(feed::Flight) / 2;
          ok = feed::parse(reinterpret_cast<const char *>(body.data), body.size, doc, flights, error, room);
          if (ok && flights.size() == room) ESP_LOGW(TAG, "Only %u planes of the reply kept: no room for more", (unsigned) room);
          if (ok) see_followed(flights);
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
    if (settings.airspace && settings.range_nm <= airspace::MOST_RANGE_NM) {
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
      // The frame just drawn is kept if it is whole; the frame wanted, if it is the one kept, comes back at once, with
      // no tile decoded (a map swiped away and back, a preview dropped). Frames older than the tiles kept are not.
      const std::string wanted = tiles::frame_key(settings, plan);
      // A frame with the radar on it is not kept: its scan goes old.
      const bool whole = !painting_key.empty() && pending.empty() && !gave_up && !radar_drawn;
      radar_drawn = false;
      radar_shown = 0;
      const uint32_t at = millis();
      bool again = false;
      {
        std::lock_guard<std::mutex> frame(frame_lock_);
        if (!kept_frame_ && whole)
          kept_frame_ = static_cast<uint16_t *>(
              heap_caps_malloc((size_t) width * height * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (kept_frame_ && !wanted.empty() && wanted == kept_key && at - kept_at < TILE_TTL_MS) {
          std::swap(base_frame_, kept_frame_);
          kept_key = whole ? painting_key : "";
          kept_at = at;
          again = true;
        } else {
          if (kept_frame_ && whole) {
            std::swap(base_frame_, kept_frame_);
            kept_key = painting_key;
            kept_at = at;
          }
          // The ground where no tile is (yet): black in the dark appearance, a pale gray in the light one.
          const uint32_t g = colours::palette(settings.theme == LIGHT).ground;
          std::fill(base_frame_, base_frame_ + width * height, tiles::rgb565(g >> 16 & 0xFF, g >> 8 & 0xFF, g & 0xFF));
        }
      }
      painting_key = wanted;
      gave_up = false;
      pending.clear();
      if (!again)
        for (size_t i = 0; i < plan.keys.size(); ++i) pending.push_back({i});
      else ++frames_again;
      std::lock_guard<std::mutex> guard(lock_);
      shared_.frame_rev++;
      shared_.frame_plan_rev = plan_rev;
      shared_.tiles_done = again ? plan.keys.size() : 0;
      shared_.tiles_total = plan.keys.size();
      shared_.tile_error.clear();
      shared_.card.missing = 0;
      shared_.frames_again = frames_again;
    }
    // The first tile due, nearest the middle first; one that failed waits its turn while the others load.
    auto due = std::find_if(pending.begin(), pending.end(), [&](const Pending &p) { return (int32_t) (millis() - p.due) >= 0; });
    if (due != pending.end()) {
      Pending &job = *due;
      const tiles::Key key = plan.keys[job.index];
      Cached *tile = cache.find(key, millis());
      std::string error;
      bool fetched_failed = false, missing = false;
      if (tile) ++tiles_from_cache;
      // The map card next: a tile still good, or any it has while off a network. One gone old is fetched again, and
      // shown as it is if that fails.
      const uint32_t clock = unix_now();
      const bool on_card = card::cacheable(key) && card_io::mounted();
      bool stale = false;
      auto from_card = [&](bool any_age) {
        card::Header h;
        if (!card_io::read(key, h, body)) return false;
        if (any_age || card::fresh(h, clock)) {
          tile = cache.keep(key, h.empty ? nullptr : body.data, h.empty ? 0 : body.size, millis());
          if (tile) ++card_state.read;
        } else {
          stale = true;
        }
        body.clear();
        return tile != nullptr;
      };
      if (!tile && on_card) from_card(!connected);
      if (!tile && !connected) {
        // Off a network, not on the card: its square stays empty until the network is back.
        missing = true;
      } else if (!tile) {
        ui::worker_phase = ui::TILE_FETCH;
        const http::Result r = http::get(tiles::url(settings, key), body, MOST_TILE_BYTES, agent_, "image/png,image/jpeg");
        ui::worker_phase = ui::IDLE;
        const bool empty = r.status == 404 || r.status == 204;
        if (r.status == 200) {
          ++tiles_fetched;
          tile_bytes_fetched += body.size;
          tile = cache.keep(key, body.data, body.size, millis());
        }
        // No tile there (outside a chart's country): remembered as empty, drawn as the ground.
        else if (empty) tile = cache.keep(key, nullptr, 0, millis());
        else error = r.error;
        // Kept on the card too, for as long as its server lets it be.
        const uint32_t lifetime = card::lifetime_s(key.style, r.max_age_s);
        if (tile && on_card && card_room && lifetime) {
          if (card_io::write(key, card::header_for(clock, lifetime, empty), empty ? nullptr : body.data,
                             empty ? 0 : body.size)) {
            ++card_state.written;
            if (++card_writes % CARD_SPACE_EVERY == 0) card_space();
          } else {
            ++card_state.failed;
          }
        }
        body.clear();
        // The network failed it: the card's old one rather than none.
        if (!tile && stale) from_card(true);
        fetched_failed = !tile;
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
      } else if (missing) {
        // Done as far as the card goes; the frame is not whole, so it is never kept as one.
        pending.erase(due);
        gave_up = true;
      } else {
        if (!fetched_failed) ++job.unreadable;
        ++job.failures;
        given_up = job.unreadable >= TILE_UNREADABLE_MOST;
        const uint32_t wait = std::min<uint32_t>(TILE_RETRY_MOST_MS, TILE_RETRY_FIRST_MS << std::min(job.failures - 1, 5));
        ESP_LOGW(TAG, "Tile %u/%u/%u%s: %s; %s", key.z, (unsigned) key.x, (unsigned) key.y, key.layer ? " (names)" : "",
                 error.c_str(), given_up ? "given up" : ("again in " + std::to_string(wait / 1000) + " s").c_str());
        if (given_up) {
          pending.erase(due);
          gave_up = true;
        }
        else job.due = millis() + wait;
      }
      std::lock_guard<std::mutex> guard(lock_);
      shared_.cached_tiles = cache.size();
      shared_.cache_bytes = cache.bytes();
      shared_.tiles_fetched = tiles_fetched;
      shared_.tiles_from_cache = tiles_from_cache;
      shared_.tile_bytes_fetched = tile_bytes_fetched;
      shared_.card.read = card_state.read;
      shared_.card.written = card_state.written;
      shared_.card.failed = card_state.failed;
      shared_.card.free = card_state.free;
      shared_.card.mounted = card_io::mounted();
      if (shared_.frame_plan_rev == plan_rev) {
        if (missing) shared_.card.missing++;
        if (drawn || missing) shared_.tiles_done++;
        if (!drawn && !missing) shared_.tile_error = error.empty() ? std::string("tile server") : error;
        else if (pending.empty() || std::none_of(pending.begin(), pending.end(), [](const Pending &p) { return p.failures; }))
          shared_.tile_error.clear();
        shared_.frame_rev++;
      }
      continue;
    }

    // ---- The radar over the map (radar.h), once the map is whole: each scan drawn over a copy of the map kept
    // without it, so the map is never drawn again for it.
    const bool radar_wanted = settings.radar_overlay && !plan.keys.empty();
    if (radar_wanted && (int32_t) (millis() - radar_check_due) >= 0) {
      body.clear();
      const http::Result r = http::get(radar::LATEST_URL, body, 4096, agent_, "application/json");
      JsonDocument doc;
      const uint32_t latest =
          r.status == 200 ? radar::read_latest(reinterpret_cast<const char *>(body.data), body.size, doc) : 0;
      body.clear();
      if (latest) radar_latest = latest;
      else ESP_LOGW(TAG, "Radar: no latest scan (%s)", r.error.empty() ? "unreadable" : r.error.c_str());
      radar_check_due = millis() + (latest ? radar::CHECK_MS : 60000);
    }
    if (pending.empty() && radar_wanted && radar_latest) {
      const size_t bytes = (size_t) width * height * 2;
      if (clean_key != painting_key || !clean_frame_) {
        if (!clean_frame_)
          clean_frame_ = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (clean_frame_) {
          std::lock_guard<std::mutex> frame(frame_lock_);
          std::memcpy(clean_frame_, base_frame_, bytes);
          clean_key = painting_key;
        }
        overlays.clear();  // of another map
      }
      const std::vector<uint32_t> frames = radar::frames(radar_latest, settings.radar_loop);
      // The loop switched on or off: on at once, from its first scan; off, what its scans changed goes.
      if (settings.radar_loop != radar_looping) {
        radar_looping = settings.radar_loop;
        radar_step_due = millis();
        radar_index = frames.size() - 1;
        if (!radar_looping) std::vector<radar::Overlay>().swap(overlays);
      }
      if (clean_frame_ && !frames.empty() && (int32_t) (millis() - radar_step_due) >= 0) {
        // The loop's next scan, or the latest alone.
        radar_index = settings.radar_loop ? (radar_index + 1) % frames.size() : frames.size() - 1;
        const uint32_t scan = frames[radar_index];
        // Scans gone out of the hour are forgotten.
        overlays.erase(std::remove_if(overlays.begin(), overlays.end(), [&](const radar::Overlay &o) {
                         return std::find(frames.begin(), frames.end(), o.scan) == frames.end();
                       }),
                       overlays.end());
        auto known = std::find_if(overlays.begin(), overlays.end(), [&](const radar::Overlay &o) { return o.scan == scan; });
        if (known != overlays.end() && (scan != radar_shown || !radar_drawn)) {
          // A scan drawn before: what it changed, put back over the map.
          {
            std::lock_guard<std::mutex> frame(frame_lock_);
            std::memcpy(base_frame_, clean_frame_, bytes);
            radar::apply(*known, base_frame_);
          }
          radar_shown = scan;
          radar_drawn = true;
          std::lock_guard<std::mutex> guard(lock_);
          shared_.frame_rev++;
          shared_.radar_scan = scan;
        } else if (scan != radar_shown || !radar_drawn) {
          const std::vector<tiles::Key> keys = radar::keys(plan, scan);
          // Its tiles first (fetched once: the cache keeps them), then all of them at once over the map.
          for (const tiles::Key &key : keys) {
            if (cache.find(key, millis())) {
              ++tiles_from_cache;
              continue;
            }
            body.clear();
            ui::worker_phase = ui::TILE_FETCH;
            const http::Result r = http::get(radar::tile_url(key), body, MOST_TILE_BYTES, agent_, "image/png");
            ui::worker_phase = ui::IDLE;
            if (r.status == 200) {
              ++tiles_fetched;
              tile_bytes_fetched += body.size;
              cache.keep(key, body.data, body.size, millis());
            } else if (r.status == 404 || r.status == 204) {
              cache.keep(key, nullptr, 0, millis());
            }
            body.clear();
          }
          {
            std::lock_guard<std::mutex> frame(frame_lock_);
            std::memcpy(base_frame_, clean_frame_, bytes);
            ui::worker_phase = ui::TILE_DECODE;
            for (const tiles::Key &key : keys) {
              Cached *tile = cache.find(key, millis());
              std::string error;
              if (tile && tile->size)
                decode::draw(tile->data, tile->size, base_frame_, width, height,
                             geo::tile_origin(plan.view, key.z, key.x, key.y), radar::filter(), error);
            }
            ui::worker_phase = ui::IDLE;
            // What it changed, kept for the loop's next time round while the loop's scans together fit.
            if (settings.radar_loop) {
              size_t used = 0;
              for (const radar::Overlay &o : overlays) used += o.pixels.size();
              radar::Overlay o;
              // Each more than 4 KB from the start, so they are PSRAM's: allocations of 4 KB or less are internal
              // RAM's (__init__.py), and twelve scans' runs took 140 KB of it.
              o.starts.reserve(radar::RESERVE_RUNS);
              o.lengths.reserve(radar::RESERVE_RUNS);
              o.pixels.reserve(radar::RESERVE_PIXELS);
              if (used < radar::MOST_OVERLAY_PIXELS &&
                  radar::difference(base_frame_, clean_frame_, (size_t) width * height, radar::MOST_OVERLAY_PIXELS - used, o)) {
                o.scan = scan;
                overlays.push_back(std::move(o));
              }
            }
          }
          radar_shown = scan;
          radar_drawn = true;
          std::lock_guard<std::mutex> guard(lock_);
          shared_.frame_rev++;
          shared_.radar_scan = scan;
          shared_.tiles_fetched = tiles_fetched;
          shared_.tiles_from_cache = tiles_from_cache;
          shared_.tile_bytes_fetched = tile_bytes_fetched;
          shared_.cache_bytes = cache.bytes();
          shared_.cached_tiles = cache.size();
        }
        const bool last = radar_index + 1 == frames.size();
        radar_step_due = millis() + (settings.radar_loop ? (last ? radar::LOOP_HOLD_MS : radar::LOOP_STEP_MS) : 30000);
      }
    } else if (pending.empty() && radar_drawn && clean_frame_ && clean_key == painting_key) {
      // Switched off: the map as it was, and no scan kept.
      std::vector<radar::Overlay>().swap(overlays);
      {
        std::lock_guard<std::mutex> frame(frame_lock_);
        std::memcpy(base_frame_, clean_frame_, (size_t) width * height * 2);
      }
      radar_drawn = false;
      radar_shown = 0;
      std::lock_guard<std::mutex> guard(lock_);
      shared_.frame_rev++;
      shared_.radar_scan = 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}  // namespace esphome::skyscope
