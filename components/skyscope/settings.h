#pragma once
// Everything a person can set over HTTP, in one table: its defaults, its limits and its JSON. The web page, the API and
// the stored copy all go through to_json() and apply(), so a key exists in one place only.
#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace sky {

template<size_t N> struct Names {
  const char *names[N];
  static constexpr size_t size = N;
  int find(const char *s) const {
    if (!s) return -1;
    for (size_t i = 0; i < N; ++i) if (std::strcmp(names[i], s) == 0) return (int) i;
    return -1;
  }
  const char *operator[](int i) const { return names[i >= 0 && i < (int) N ? i : 0]; }
};

enum MapStyle : uint8_t { RADAR, OSM, CARTO_LIGHT, CARTO_DARK, SATELLITE, TOPO, SECTIONAL, IFR_LOW, IFR_HIGH, CUSTOM };
inline constexpr Names<10> MAP_STYLES{{"radar", "osm", "carto_light", "carto_dark", "satellite", "topo", "sectional",
                                       "ifr_low", "ifr_high", "custom"}};
enum MapFilter : uint8_t { FILTER_NONE, FILTER_DIM, FILTER_INVERT };
inline constexpr Names<3> MAP_FILTERS{{"none", "dim", "invert"}};
enum Feed : uint8_t { ADSB_LOL, ADSB_FI };
inline constexpr Names<2> FEEDS{{"adsb_lol", "adsb_fi"}};
enum Tint : uint8_t { GREEN, AMBER, BLUE, WHITE };
inline constexpr Names<4> TINTS{{"green", "amber", "blue", "white"}};
enum MarkerKind : uint8_t { MARKER_DOT, MARKER_ARROW, MARKER_PLANE };
inline constexpr Names<3> MARKERS{{"dot", "arrow", "plane"}};
enum LabelKind : uint8_t { LABEL_AUTO, LABEL_NONE, LABEL_CALLSIGN, LABEL_ALTITUDE, LABEL_FULL };
inline constexpr Names<5> LABELS{{"auto", "none", "callsign", "altitude", "full"}};
inline constexpr Names<6> WATCH_COLOURS{{"red", "orange", "amber", "green", "blue", "purple"}};

inline constexpr int RANGES_NM[] = {2, 3, 5, 8, 10, 15, 20, 30, 40, 60, 80, 120, 160, 250};
constexpr int MIN_RANGE_NM = 1, MAX_RANGE_NM = 250;
constexpr int ALTITUDE_CEILING = 60000;
constexpr size_t MOST_WATCHED = 12;

struct Watch {
  std::string id;  // a registration, callsign or ICAO hex, upper case
  int colour = 0;  // index into WATCH_COLOURS
};

struct Settings {
  // Where and how far.
  bool located = false;
  double lat = 0, lon = 0;
  int range_nm = 20;
  // The map under the planes.
  MapStyle map = RADAR;
  std::string custom_url;  // {z} {x} {y} template, PNG or JPEG
  MapFilter filter = FILTER_NONE;
  int dim = 60;            // percent of the map's light kept by FILTER_DIM
  int tile_scale = 0;      // 1 or 2 screen pixels per tile pixel; 0 the board's default
  bool rings = true;       // range rings and a compass rose over any map, the scope on `radar`
  Tint tint = GREEN;
  bool sweep = true;       // the radar's beam turns
  // The aircraft.
  // adsb.fi: adsb.lol allows 500 requests a day per address, one every three minutes (feed.h min_interval_ms).
  Feed feed = ADSB_FI;
  int refresh_s = 10;
  int alt_min = 0, alt_max = ALTITUDE_CEILING;
  bool ground = false;
  int most = 30;
  MarkerKind marker = MARKER_PLANE;
  LabelKind label = LABEL_AUTO;
  bool by_altitude = true;
  bool glide = true;
  int track_s = 60;
  std::vector<Watch> watch;
  // The screen.
  int rotation = -1;  // degrees: 0, 90, 180 or 270, or -1 for the board's own (its YAML's lvgl: rotation)
  bool metric = false;
  int brightness = 80;
};

inline std::string upper_id(const char *s) {
  std::string out;
  for (; s && *s; ++s) {
    const char c = *s;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c >= 'a' && c <= 'z') out += (char) (c - 32);
  }
  return out;
}

inline void to_json(const Settings &s, JsonObject o) {
  o["located"] = s.located;
  o["lat"] = s.lat;
  o["lon"] = s.lon;
  o["range_nm"] = s.range_nm;
  o["map"] = MAP_STYLES[s.map];
  o["custom_url"] = s.custom_url;
  o["filter"] = MAP_FILTERS[s.filter];
  o["dim"] = s.dim;
  o["tile_scale"] = s.tile_scale;
  o["rings"] = s.rings;
  o["tint"] = TINTS[s.tint];
  o["sweep"] = s.sweep;
  o["feed"] = FEEDS[s.feed];
  o["refresh_s"] = s.refresh_s;
  o["alt_min"] = s.alt_min;
  o["alt_max"] = s.alt_max;
  o["ground"] = s.ground;
  o["most"] = s.most;
  o["marker"] = MARKERS[s.marker];
  o["label"] = LABELS[s.label];
  o["by_altitude"] = s.by_altitude;
  o["glide"] = s.glide;
  o["track_s"] = s.track_s;
  JsonArray w = o["watch"].to<JsonArray>();
  for (const auto &item : s.watch) {
    JsonObject e = w.add<JsonObject>();
    e["id"] = item.id;
    e["colour"] = WATCH_COLOURS[item.colour];
  }
  o["rotation"] = s.rotation;
  o["metric"] = s.metric;
  o["brightness"] = s.brightness;
}

// Applies the keys `o` has to `s`, leaving the others as they are. Nothing changes and `error` names the first key that
// is wrong when one is.
inline bool apply(JsonObjectConst o, Settings &s, std::string &error) {
  Settings n = s;
  auto fail = [&](const char *key) { error = key; return false; };
  auto number = [&](const char *key, int &out, int low, int high) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<double>()) return false;
    const double d = v.as<double>();
    if (!std::isfinite(d) || d < low || d > high) return false;
    out = (int) std::lround(d);
    return true;
  };
  auto flag = [&](const char *key, bool &out) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    if (!v.is<bool>()) return false;
    out = v.as<bool>();
    return true;
  };
  auto choice = [&](const char *key, auto &out, const auto &names) {
    JsonVariantConst v = o[key];
    if (v.isNull()) return true;
    const int i = names.find(v.as<const char *>());
    if (i < 0) return false;
    out = static_cast<std::remove_reference_t<decltype(out)>>(i);
    return true;
  };
  for (const char *key : {"lat", "lon"}) {
    JsonVariantConst v = o[key];
    if (v.isNull()) continue;
    if (!v.is<double>()) return fail(key);
    const double d = v.as<double>();
    const double limit = key[1] == 'a' ? 85.0 : 180.0;
    if (!std::isfinite(d) || std::fabs(d) > limit) return fail(key);
    (key[1] == 'a' ? n.lat : n.lon) = d;
    n.located = true;
  }
  if (!flag("located", n.located)) return fail("located");
  if (!number("range_nm", n.range_nm, MIN_RANGE_NM, MAX_RANGE_NM)) return fail("range_nm");
  if (!choice("map", n.map, MAP_STYLES)) return fail("map");
  if (!o["custom_url"].isNull()) {
    const char *url = o["custom_url"].as<const char *>();
    if (!url || std::strlen(url) > 200) return fail("custom_url");
    n.custom_url = url;
    if (!n.custom_url.empty() && (n.custom_url.rfind("http", 0) != 0 || n.custom_url.find("{z}") == std::string::npos))
      return fail("custom_url");
  }
  if (!choice("filter", n.filter, MAP_FILTERS)) return fail("filter");
  if (!number("dim", n.dim, 10, 100)) return fail("dim");
  if (!number("tile_scale", n.tile_scale, 0, 2)) return fail("tile_scale");
  if (!flag("rings", n.rings)) return fail("rings");
  if (!choice("tint", n.tint, TINTS)) return fail("tint");
  if (!flag("sweep", n.sweep)) return fail("sweep");
  if (!choice("feed", n.feed, FEEDS)) return fail("feed");
  if (!number("refresh_s", n.refresh_s, 3, 300)) return fail("refresh_s");
  if (!number("alt_min", n.alt_min, 0, ALTITUDE_CEILING)) return fail("alt_min");
  if (!number("alt_max", n.alt_max, 0, ALTITUDE_CEILING)) return fail("alt_max");
  if (n.alt_min > n.alt_max) return fail("alt_min");
  if (!flag("ground", n.ground)) return fail("ground");
  if (!number("most", n.most, 1, 60)) return fail("most");
  if (!choice("marker", n.marker, MARKERS)) return fail("marker");
  if (!choice("label", n.label, LABELS)) return fail("label");
  if (!flag("by_altitude", n.by_altitude)) return fail("by_altitude");
  if (!flag("glide", n.glide)) return fail("glide");
  if (!number("track_s", n.track_s, 0, 300)) return fail("track_s");
  if (!o["watch"].isNull()) {
    JsonArrayConst list = o["watch"].as<JsonArrayConst>();
    if (list.isNull() || list.size() > MOST_WATCHED) return fail("watch");
    n.watch.clear();
    for (JsonObjectConst item : list) {
      const std::string id = upper_id(item["id"].as<const char *>());
      const int colour = WATCH_COLOURS.find(item["colour"] | "red");
      if (id.size() < 2 || id.size() > 10 || colour < 0) return fail("watch");
      if (std::none_of(n.watch.begin(), n.watch.end(), [&](const Watch &w) { return w.id == id; }))
        n.watch.push_back({id, colour});
    }
  }
  if (!number("rotation", n.rotation, -1, 270) || (n.rotation >= 0 && n.rotation % 90)) return fail("rotation");
  if (!flag("metric", n.metric)) return fail("metric");
  if (!number("brightness", n.brightness, 0, 100)) return fail("brightness");
  s = std::move(n);
  return true;
}

}  // namespace sky
