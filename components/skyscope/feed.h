#pragma once
// The aircraft around the screen, straight from a public ADS-B exchange: adsb.lol or adsb.fi, which both answer in
// readsb's JSON (an `ac` or `aircraft` list of planes with `hex`, `flight`, `lat`, `lon`, `alt_baro`, `gs`, `track`
// ...). Nothing here touches the network: http.cpp fetches the text, parse() reads it, so the tests run on a PC.
#include <ArduinoJson.h>

#include <cmath>
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
inline std::string url(Feed feed, double lat, double lon, int radius_nm) {
  radius_nm = std::max(1, std::min(250, radius_nm));
  char out[160];
  switch (feed) {
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
    default: return "adsb.lol";
  }
}

inline std::string trimmed(const char *s, size_t most = 24) {
  if (!s) return "";
  while (*s == ' ') ++s;
  std::string out(s, std::min(std::strlen(s), most));
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// A plane whose position is older than this is left out: the exchange still lists it for a while after it was heard.
constexpr double STALE_POSITION_S = 60;

// The fields read, so a reply of two hundred planes with fifty fields each costs the memory of the ones we draw.
inline JsonDocument filter() {
  JsonDocument f;
  for (const char *list : {"ac", "aircraft"}) {
    JsonObject p = f[list].add<JsonObject>();
    for (const char *key : {"hex", "flight", "r", "t", "desc", "ownOp", "alt_baro", "gs", "track", "true_heading",
                            "baro_rate", "geom_rate", "squawk", "category", "lat", "lon", "seen_pos", "emergency"})
      p[key] = true;
  }
  f["msg"] = true;
  return f;
}

// The planes of a reply, or false with `error` set when it is not one.
template<typename Doc> bool parse(const char *text, size_t length, Doc &doc, std::vector<Flight> &out, std::string &error) {
  static const JsonDocument keep = filter();
  const DeserializationError e = deserializeJson(doc, text, length, DeserializationOption::Filter(keep));
  if (e) {
    error = std::string("JSON: ") + e.c_str();
    return false;
  }
  JsonArrayConst list = doc["ac"].template as<JsonArrayConst>();
  if (list.isNull()) list = doc["aircraft"].template as<JsonArrayConst>();
  if (list.isNull()) {
    const char *msg = doc["msg"] | "no aircraft list";
    error = msg;
    return false;
  }
  out.clear();
  out.reserve(list.size());
  for (JsonObjectConst p : list) {
    if (!p["lat"].template is<double>() || !p["lon"].template is<double>()) continue;
    if ((p["seen_pos"] | 0.0) > STALE_POSITION_S) continue;
    Flight f;
    f.hex = trimmed(p["hex"] | "", 8);
    if (f.hex.empty()) continue;
    for (auto &c : f.hex) if (c >= 'A' && c <= 'Z') c += 32;
    f.lat = p["lat"].template as<double>();
    f.lon = p["lon"].template as<double>();
    if (!std::isfinite(f.lat) || !std::isfinite(f.lon) || std::fabs(f.lat) > 90 || std::fabs(f.lon) > 180) continue;
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
    out.push_back(std::move(f));
  }
  return true;
}
}  // namespace sky::feed
