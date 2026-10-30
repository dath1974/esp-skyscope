#pragma once
// The weather radar over the map: the composite of the NWS's NEXRAD radars (base reflectivity, N0Q) as the Iowa
// Environmental Mesonet serves it, in the same 256-pixel squares as the maps, a new scan every five minutes. A scan's
// tiles are asked for by its time, so each is fetched once (the tile cache keeps it) and only the newest is new; the
// worker draws them over a copy of the map kept without them (worker.cpp), the latest scan, or the last hour of them
// in turn as a loop. The United States only.
#include <ArduinoJson.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "tiles.h"
#include "weather.h"

namespace sky::radar {

constexpr uint32_t STEP_MINUTES = 5;      // between two scans
constexpr int LOOP_FRAMES = 12;           // an hour of them
constexpr uint32_t CHECK_MS = 5 * 60 * 1000;  // the latest scan asked for this often
constexpr uint32_t LOOP_STEP_MS = 700, LOOP_HOLD_MS = 2500;  // each scan of the loop, and the last one longer
constexpr uint8_t LAYER = 2;              // the tiles' layer (tiles.h Key): over the map, under its place names
constexpr uint8_t OPACITY = 166;          // of 255: the map shows through
inline const char *const LATEST_URL = "https://mesonet.agron.iastate.edu/data/gis/images/4326/USCOMP/n0q_0.json";
inline const char *const CREDIT = "Radar: NWS NEXRAD via Iowa Environmental Mesonet";

// The latest scan's time from IEM's note of it ({"meta": {"valid": "2026-10-30T08:10:00Z"}}), in minutes since 1970;
// 0 when it cannot be read.
inline uint32_t read_latest(const char *text, size_t length, JsonDocument &doc) {
  if (deserializeJson(doc, text, length)) return 0;
  const int64_t s = weather::utc_seconds(doc["meta"]["valid"] | "");
  return s > 0 ? (uint32_t) (s / 60) : 0;
}

// A scan's time as IEM names it: "202610300810" (UTC).
inline std::string stamp(uint32_t minutes) {
  // The civil date of a day since 1970 (Howard Hinnant's civil_from_days).
  const int64_t z = (int64_t) minutes / 1440 + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned) (z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t y = (int64_t) yoe + era * 400 + (m <= 2);
  const unsigned of_day = minutes % 1440;
  char text[16];
  std::snprintf(text, sizeof(text), "%04d%02u%02u%02u%02u", (int) y, m, d, of_day / 60, of_day % 60);
  return text;
}

// The scans to show, the oldest first: the latest alone, or the last hour of them.
inline std::vector<uint32_t> frames(uint32_t latest, bool loop) {
  std::vector<uint32_t> out;
  if (!latest) return out;
  const int count = loop ? LOOP_FRAMES : 1;
  for (int i = count - 1; i >= 0; --i) out.push_back(latest - (uint32_t) i * STEP_MINUTES);
  return out;
}

inline std::string tile_url(const tiles::Key &k) {
  char text[160];
  std::snprintf(text, sizeof(text), "https://mesonet.agron.iastate.edu/cache/tile.py/1.0.0/ridge::USCOMP-N0Q-%s/%u/%u/%u.png",
                stamp(k.time).c_str(), k.z, (unsigned) k.x, (unsigned) k.y);
  return text;
}

// The radar's tiles for a plan's map, at scan `minutes`: one for each of its map's squares (not its place names').
inline std::vector<tiles::Key> keys(const tiles::Plan &p, uint32_t minutes) {
  std::vector<tiles::Key> out;
  for (tiles::Key k : p.keys) {
    if (k.layer != 0) continue;
    k.layer = LAYER;
    k.time = minutes;
    out.push_back(k);
  }
  return out;
}

// How a scan's tiles are painted: as they are, part see-through.
inline tiles::Filter filter() {
  tiles::Filter f;
  f.opacity = OPACITY;
  return f;
}

// What a scan changed in a frame, as runs of the pixels that differ from the map without it: a loop plays a scan it has
// drawn once by putting these back over the map, not by decoding its tiles again. Light rain changes a little of the
// frame; a frame of storms may not fit MOST_OVERLAY_PIXELS, and is decoded each time.
constexpr size_t MOST_OVERLAY_PIXELS = 600 * 1000;  // of all the loop's scans together: 1.2 MB of PSRAM at most
// What a scan's runs reserve at first: more than 4 KB each, so the heap gives them PSRAM, not internal RAM.
constexpr size_t RESERVE_RUNS = 1280, RESERVE_PIXELS = 2560;
static_assert(RESERVE_RUNS * sizeof(uint32_t) > 4096 && RESERVE_PIXELS * sizeof(uint16_t) > 4096, "PSRAM's, not internal");
struct Overlay {
  uint32_t scan = 0;
  std::vector<uint32_t> starts, lengths;  // each run: where it starts in the frame, and how many pixels
  std::vector<uint16_t> pixels;           // the runs' pixels, one after another
};
// The runs where `with` differs from `without` (both `count` pixels); false, with `out` left empty, past `most` pixels.
inline bool difference(const uint16_t *with, const uint16_t *without, size_t count, size_t most, Overlay &out) {
  out.starts.clear();
  out.lengths.clear();
  out.pixels.clear();
  for (size_t i = 0; i < count;) {
    if (with[i] == without[i]) {
      ++i;
      continue;
    }
    const size_t from = i;
    while (i < count && with[i] != without[i]) ++i;
    if (out.pixels.size() + (i - from) > most) {
      out = Overlay{};
      return false;
    }
    out.starts.push_back((uint32_t) from);
    out.lengths.push_back((uint32_t) (i - from));
    out.pixels.insert(out.pixels.end(), with + from, with + i);
  }
  return true;
}
// The runs put into `frame` (a copy of the map without the scan).
inline void apply(const Overlay &o, uint16_t *frame) {
  size_t at = 0;
  for (size_t r = 0; r < o.starts.size(); ++r) {
    std::copy(o.pixels.begin() + at, o.pixels.begin() + at + o.lengths[r], frame + o.starts[r]);
    at += o.lengths[r];
  }
}

// "0810Z", the time of the scan shown, for the credits.
inline std::string clock(uint32_t minutes) {
  char text[8];
  std::snprintf(text, sizeof(text), "%02u%02uZ", (minutes % 1440) / 60, minutes % 60);
  return text;
}
}  // namespace sky::radar
