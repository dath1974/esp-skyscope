#pragma once
// The aircraft around the screen, straight from a public ADS-B exchange: adsb.lol or adsb.fi, which both answer in
// readsb's JSON (an `ac` or `aircraft` list of planes with `hex`, `flight`, `lat`, `lon`, `alt_baro`, `gs`, `track`
// ...). Nothing here touches the network: http.cpp fetches the text, parse() reads it, so the tests run on a PC.
#include <ArduinoJson.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "settings.h"

namespace sky::feed {

struct Flight {
  std::string hex;           // the transponder's ICAO address, lower case: the plane's id
  std::string callsign;      // trimmed
  std::string registration;
  std::string type;          // ICAO type designator, B738
  std::string description;   // the type in words, where the feed has it
  std::string operator_name;
  std::string squawk;
  std::string category;      // A1 ... C3
  double lat = 0, lon = 0;
  bool on_ground = false;
  bool has_altitude = false;
  int altitude = 0;          // feet, barometric
  bool has_speed = false;
  float speed = 0;           // knots over the ground
  bool has_heading = false;
  float heading = 0;         // degrees from true north
  bool has_climb = false;
  int climb = 0;             // feet a minute
  bool emergency = false;
};

// The URL of `feed` for planes within `radius_nm` of a place. Every one of them caps the radius at 250 nm.
inline std::string url(Feed feed, double lat, double lon, int radius_nm, const std::string &receiver = "") {
  radius_nm = std::max(1, std::min(250, radius_nm));
  char out[160];
  switch (feed) {
    case RECEIVER: {
      // A receiver lists every plane it hears; one with an API of its own may take the place and range.
      std::string u = receiver;
      auto put = [&u](const char *key, const std::string &value) {
        for (size_t at; (at = u.find(key)) != std::string::npos;) u.replace(at, std::strlen(key), value);
      };
      std::snprintf(out, sizeof(out), "%.4f", lat);
      put("{lat}", out);
      std::snprintf(out, sizeof(out), "%.4f", lon);
      put("{lon}", out);
      put("{dist}", std::to_string(radius_nm));
      return u;
    }
    case ADSB_FI:
      std::snprintf(out, sizeof(out), "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", lat, lon, radius_nm);
      break;
    default:
      std::snprintf(out, sizeof(out), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat, lon, radius_nm);
  }
  return out;
}
inline const char *credit(Feed feed) {
  switch (feed) {
    case ADSB_FI: return "adsb.fi";
    case RECEIVER: return "receiver";
    default: return "adsb.lol";
  }
}

inline std::string trimmed(const char *s, size_t most = 24) {
  if (!s) return "";
  while (*s == ' ') ++s;
  size_t n = std::min(std::strlen(s), most);
  // Cut before a character, never inside one: the label and the API take UTF-8.
  while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) --n;
  std::string out(s, n);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// The fewest milliseconds between two requests that keep within every window a server announces in its
// x-ratelimit-limit header, as Envoy writes it ("500, 4;w=1, 60;w=60, 500;w=86400": N requests in W seconds; the
// first number alone is the default window's, which the list repeats); 0 when it names no window. 500 a day, as adsb.lol
// announced it once, is one request every 173 seconds.
inline uint32_t min_interval_ms(const std::string &limit) {
  uint32_t most = 0;
  size_t at = 0;
  while (at < limit.size()) {
    const size_t end = std::min(limit.find(',', at), limit.size());
    const std::string part = limit.substr(at, end - at);
    const size_t w = part.find("w=");
    if (w != std::string::npos) {
      const long n = std::atol(part.c_str()), seconds = std::atol(part.c_str() + w + 2);
      if (n > 0 && seconds > 0) most = std::max<uint32_t>(most, (uint32_t) ((seconds * 1000 + n - 1) / n));
    }
    at = end + 1;
  }
  return most;
}

// How long until `at` (millis), from `now`, for a countdown: "6 h 58 min" from an hour and a half, "12:05" (minutes and
// seconds) below, "now" once it is due.
inline std::string countdown(uint32_t at, uint32_t now) {
  const int32_t left = (int32_t) (at - now);
  if (left <= 0) return "now";
  const uint32_t s = ((uint32_t) left + 999) / 1000;
  char out[24];
  if (s >= 90 * 60) std::snprintf(out, sizeof(out), "%u h %02u min", (unsigned) (s / 3600), (unsigned) (s % 3600 / 60));
  else std::snprintf(out, sizeof(out), "%u:%02u", (unsigned) (s / 60), (unsigned) (s % 60));
  return out;
}

// A plane whose position is older than this is left out: the exchange still lists it for a while after it was heard.
constexpr double STALE_POSITION_S = 60;

// The fields read of a plane, so one with fifty fields costs the memory of the ones we draw.
constexpr const char *FIELDS[] = {"hex", "flight", "r", "t", "desc", "ownOp", "alt_baro", "gs", "track", "true_heading",
                                  "baro_rate", "geom_rate", "squawk", "category", "lat", "lon", "seen_pos", "emergency"};
inline JsonDocument plane_filter() {
  JsonDocument f;
  for (const char *key : FIELDS) f[key] = true;
  return f;
}
// Of a reply that has no list, its word on why.
inline JsonDocument message_filter() {
  JsonDocument f;
  f["msg"] = true;
  return f;
}

namespace scan {
// A JSON value's extent, without reading it: what the reply is cut into before each plane is read on its own.
inline const char *space(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  return p;
}
// Past the string that starts at `p` (a quote), or null when it does not end.
inline const char *string(const char *p, const char *end) {
  for (++p; p < end; ++p) {
    if (*p == '\\') ++p;
    else if (*p == '"') return p + 1;
  }
  return nullptr;
}
// Past the value that starts at `p`, or null when it does not end.
inline const char *value(const char *p, const char *end) {
  if (p >= end) return nullptr;
  if (*p == '"') return string(p, end);
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < end) {
      const char c = *p;
      if (c == '"') {
        p = string(p, end);
        if (!p) return nullptr;
        continue;
      }
      if (c == '{' || c == '[') ++depth;
      else if ((c == '}' || c == ']') && --depth == 0) return p + 1;
      ++p;
    }
    return nullptr;
  }
  while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') ++p;
  return p;
}
}  // namespace scan

// One plane of a reply, or false when it is not one to show (no address, no place, a place too old or off the earth).
inline bool flight_of(JsonObjectConst p, Flight &f) {
  if (!p["lat"].template is<double>() || !p["lon"].template is<double>()) return false;
  if ((p["seen_pos"] | 0.0) > STALE_POSITION_S) return false;
  f.hex = trimmed(p["hex"] | "", 8);
  if (f.hex.empty()) return false;
  for (auto &c : f.hex) if (c >= 'A' && c <= 'Z') c += 32;
  f.lat = p["lat"].template as<double>();
  f.lon = p["lon"].template as<double>();
  if (!std::isfinite(f.lat) || !std::isfinite(f.lon) || std::fabs(f.lat) > 90 || std::fabs(f.lon) > 180) return false;
  f.callsign = trimmed(p["flight"] | "", 10);
  f.registration = trimmed(p["r"] | "", 12);
  f.type = trimmed(p["t"] | "", 6);
  f.description = trimmed(p["desc"] | "", 40);
  f.operator_name = trimmed(p["ownOp"] | "", 40);
  f.squawk = trimmed(p["squawk"] | "", 4);
  f.category = trimmed(p["category"] | "", 2);
  JsonVariantConst alt = p["alt_baro"];
  if (alt.template is<const char *>() && std::strcmp(alt.template as<const char *>(), "ground") == 0) {
    f.on_ground = true;
  } else if (alt.template is<double>()) {
    f.has_altitude = true;
    f.altitude = (int) std::lround(alt.template as<double>());
  }
  if (p["gs"].template is<double>()) { f.has_speed = true; f.speed = p["gs"].template as<float>(); }
  JsonVariantConst heading = p["track"].template is<double>() ? p["track"] : p["true_heading"];
  if (heading.template is<double>()) {
    f.has_heading = true;
    f.heading = std::fmod(heading.template as<float>() + 360.0f, 360.0f);
  }
  JsonVariantConst climb = p["baro_rate"].template is<double>() ? p["baro_rate"] : p["geom_rate"];
  if (climb.template is<double>()) { f.has_climb = true; f.climb = (int) std::lround(climb.template as<double>()); }
  const char *emergency = p["emergency"] | "none";
  f.emergency = std::strcmp(emergency, "none") != 0 || f.squawk == "7500" || f.squawk == "7600" || f.squawk == "7700";
  return true;
}

// The planes of a reply, or false with `error` set when it is not one. Each plane is read on its own into `doc`: read
// whole, ArduinoJson looks through every string it holds for each new one, and a list of a thousand planes took
// seconds (4500 took over a minute, and then ran out of the slots a document has on a 32-bit chip).
template<typename Doc> bool parse(const char *text, size_t length, Doc &doc, std::vector<Flight> &out, std::string &error) {
  static const JsonDocument keep = plane_filter();
  const char *end = text + length, *p = scan::space(text, end);
  const char *list = nullptr, *list_end = nullptr;
  auto broken = [&](const char *at) {
    error = at ? "JSON: InvalidInput" : "JSON: IncompleteInput";
    return false;
  };
  if (p >= end) return broken(nullptr);
  if (*p != '{') return broken(p);
  // The reply's members: the list (`ac`, or `aircraft` as readsb names it) found, the rest stepped over.
  p = scan::space(p + 1, end);
  while (p < end && *p != '}') {
    if (*p != '"') return broken(p);
    const char *key = p + 1, *after = scan::string(p, end);
    if (!after) return broken(nullptr);
    const std::string name(key, after - 1 - key);
    p = scan::space(after, end);
    if (p >= end) return broken(nullptr);
    if (*p != ':') return broken(p);
    p = scan::space(p + 1, end);
    const char *value_end = scan::value(p, end);
    if (!value_end) return broken(nullptr);
    if ((name == "ac" || (name == "aircraft" && !list)) && p < end && *p == '[') {
      list = p;
      list_end = value_end;
    }
    p = scan::space(value_end, end);
    if (p < end && *p == ',') p = scan::space(p + 1, end);
    else if (p < end && *p != '}') return broken(p);
  }
  if (p >= end) return broken(nullptr);
  if (!list) {
    static const JsonDocument only_msg = message_filter();
    const DeserializationError e = deserializeJson(doc, text, length, DeserializationOption::Filter(only_msg));
    error = e ? std::string("JSON: ") + e.c_str() : std::string(doc["msg"] | "no aircraft list");
    return false;
  }
  out.clear();
  for (p = scan::space(list + 1, list_end); p < list_end && *p != ']';) {
    const char *item_end = scan::value(p, list_end);
    if (!item_end) return broken(nullptr);
    if (*p == '{') {  // anything else in the list is no plane
      const DeserializationError e = deserializeJson(doc, p, item_end - p, DeserializationOption::Filter(keep));
      if (e) {
        error = std::string("JSON: ") + e.c_str();
        return false;
      }
      Flight f;
      if (flight_of(doc.template as<JsonObjectConst>(), f)) out.push_back(std::move(f));
    }
    p = scan::space(item_end, list_end);
    if (p < list_end && *p == ',') p = scan::space(p + 1, list_end);
    else if (p < list_end && *p != ']') return broken(p);
  }
  doc.clear();
  return true;
}
}  // namespace sky::feed
