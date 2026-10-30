#pragma once
// The settings the screen's own menu offers (a long press on the map opens it), one row each: what it is called, the
// values its arrows step through, how it is typed on the menu's keyboard (keyboard.h), and the maps it applies to. A
// row with choices steps through them; a row with a pad is typed as well, or only, where it has no choices. A row's
// change goes through settings.h apply(), as the web page's does, so the menu offers nothing the settings would refuse
// (tests/test_host.cpp walks every value of every row through it). The place is found by a search (places.h), as on
// the web page.
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "keyboard.h"
#include "settings.h"

namespace sky::menu {
using keyboard::Pad;

// PLACE is the place to watch: its name, or its coordinates, and a search to find another.
enum Kind : uint8_t { TEXT, NUMBER, FLAG, PLACE };
struct Choice {
  const char *value;  // as JSON takes it: a word, or a whole number written out
  const char *label;  // as the screen shows it
};
// The maps a row applies to, a bit a MapStyle.
constexpr uint16_t bit(MapStyle m) { return (uint16_t) (1u << m); }
constexpr uint16_t ALL = 0xFFFF;
constexpr uint16_t TILE_MAPS = ALL & ~bit(RADAR);
constexpr uint16_t DRAWN = bit(STREETS) | bit(OSM) | bit(SATELLITE) | bit(TOPO) | bit(CUSTOM);
constexpr uint16_t NAMED = bit(STREETS) | bit(SATELLITE);
constexpr uint16_t SCOPE = bit(RADAR);

struct Row {
  const char *key, *name;
  Kind kind;
  std::vector<Choice> choices;  // TEXT and NUMBER; a FLAG is on or off
  uint16_t maps = ALL;
  Pad pad = keyboard::NONE;  // typed on the keyboard, and how
  const char *unit = "";     // after a number typed that is none of the choices: "12 NM"
  const char *hint = "";     // what the keyboard says under the text: what the value may be
  std::vector<const char *> needs = {};  // flags that must be on for the row to show
  size_t most = 0;  // bytes the keyboard takes; 0 for its pad's own
};

inline const std::vector<Row> &rows() {
  using namespace keyboard;
  static const std::vector<Row> table = {
      {.key = "place", .name = "Place", .kind = PLACE, .pad = WORDS,
       .hint = "An airport code (KJFK), an airport's name, a town or an address", .most = 60},
      {.key = "lat", .name = "Latitude", .kind = NUMBER, .pad = DECIMAL,
       .hint = "Degrees, -85 to 85: north positive, south negative"},
      {.key = "lon", .name = "Longitude", .kind = NUMBER, .pad = DECIMAL,
       .hint = "Degrees, -180 to 180: east positive, west negative"},
      {"range_nm", "Range", NUMBER,
       {{"2", "2 NM"}, {"3", "3 NM"}, {"5", "5 NM"}, {"7", "7 NM"}, {"10", "10 NM"}, {"15", "15 NM"}, {"20", "20 NM"},
        {"25", "25 NM"}, {"30", "30 NM"}, {"40", "40 NM"}, {"50", "50 NM"}, {"60", "60 NM"}, {"80", "80 NM"},
        {"100", "100 NM"}, {"150", "150 NM"}, {"200", "200 NM"}, {"250", "250 NM"}},
       ALL, WHOLE, " NM", "Nautical miles to the nearer edge, 1 to 250"},
      {"map", "Map", TEXT,
       {{"radar", "Radar"}, {"streets", "Streets"}, {"osm", "OSM"}, {"satellite", "Satellite"}, {"topo", "Topo"},
        {"sectional", "Sectional"}, {"ifr_low", "IFR low"}, {"ifr_high", "IFR high"}, {"custom", "Custom"}}},
      {.key = "custom_url", .name = "Custom tiles", .kind = TEXT, .pad = WORDS,
       .hint = "A server's 256-pixel PNG or JPEG tiles, with {z}, {x} and {y}: https://example.com/{z}/{x}/{y}.png",
       .most = 200},
      {"theme", "Appearance", TEXT, {{"dark", "Dark"}, {"light", "Light"}}, TILE_MAPS},
      {"tint", "Phosphor", TEXT, {{"green", "Green"}, {"amber", "Amber"}, {"blue", "Blue"}, {"white", "White"}}, SCOPE},
      {"scope", "Scope", FLAG, {}, SCOPE},
      {"sweep", "Beam sweeps", FLAG, {}, SCOPE},
      {"map_brightness", "Map brightness", NUMBER,
       {{"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"}, {"80", "80%"}, {"90", "90%"}, {"100", "100%"}},
       TILE_MAPS, WHOLE, "%", "Percent of the map's light, 10 to 100"},
      {"place_names", "Place names", FLAG, {}, NAMED},
      {"tile_scale", "Map text", NUMBER, {{"0", "Default"}, {"1", "Small"}, {"2", "Large"}}, DRAWN},
      {"rings", "Range rings", FLAG, {}},
      {"airspace", "Airspace", FLAG, {}},
      {"marker", "Marker", TEXT, {{"type", "By type"}, {"plane", "Plane"}, {"arrow", "Arrow"}, {"dot", "Dot"}}},
      {"label", "Labels", TEXT,
       {{"auto", "Auto"}, {"none", "None"}, {"callsign", "Callsign"}, {"altitude", "Altitude"}, {"full", "Full"}}},
      {"track_s", "Tracks", NUMBER, {{"0", "Off"}, {"30", "30 s"}, {"60", "1 min"}, {"120", "2 min"}, {"300", "5 min"}},
       ALL, WHOLE, " s", "Seconds of track behind each plane, 0 to 300"},
      {"most", "Most planes", NUMBER,
       {{"5", "5"}, {"10", "10"}, {"15", "15"}, {"20", "20"}, {"30", "30"}, {"40", "40"}, {"60", "60"}},
       ALL, WHOLE, "", "1 to 60"},
      {.key = "alt_min", .name = "Lowest", .kind = NUMBER, .pad = WHOLE, .unit = " ft",
       .hint = "Feet, 0 to 60000, and no higher than the highest"},
      {.key = "alt_max", .name = "Highest", .kind = NUMBER, .pad = WHOLE, .unit = " ft",
       .hint = "Feet, 0 to 60000, and no lower than the lowest"},
      {"by_altitude", "Colour by altitude", FLAG, {}},
      {"glide", "Glide", FLAG, {}},
      {"ground", "On the ground", FLAG, {}},
      {.key = "watch", .name = "Watched", .kind = TEXT, .pad = WORDS,
       .hint = "A registration, callsign or ICAO hex and a colour (red, orange, amber, green, blue, purple), with a "
               "comma between: N12345 red, BAW123 blue",
       .most = 240},
      {"feed", "Source", TEXT, {{"adsb_fi", "adsb.fi"}, {"adsb_lol", "adsb.lol"}, {"receiver", "Receiver"}}},
      {"fallback", "Use the other", FLAG, {}},
      {.key = "receiver_url", .name = "Receiver", .kind = TEXT, .pad = WORDS,
       .hint = "A receiver's aircraft.json: http://receiver.local/tar1090/data/aircraft.json", .most = 160},
      {"refresh_s", "Refresh", NUMBER, {{"5", "5 s"}, {"10", "10 s"}, {"15", "15 s"}, {"30", "30 s"}, {"60", "1 min"}},
       ALL, WHOLE, " s", "Seconds between reports, 3 to 300"},
      {"metric", "Units", FLAG, {}},
      {"brightness", "Brightness", NUMBER,
       {{"5", "5%"}, {"10", "10%"}, {"20", "20%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"},
        {"80", "80%"}, {"90", "90%"}, {"100", "100%"}},
       ALL, WHOLE, "%", "Percent, 0 to 100"},
      {"sleep_min", "Screen off", NUMBER,
       {{"0", "Never"}, {"1", "1 min"}, {"5", "5 min"}, {"10", "10 min"}, {"15", "15 min"}, {"30", "30 min"},
        {"60", "1 hour"}, {"120", "2 hours"}},
       ALL, WHOLE, " min", "Minutes without a touch, 0 for never, up to 1440"},
      {"bedtime", "Bedtime", FLAG, {}},
      {.key = "bed_off", .name = "Off at", .kind = TEXT, .pad = CLOCK, .hint = "The time of day, 22:00",
       .needs = {"bedtime"}},
      {.key = "bed_wake", .name = "On again", .kind = FLAG, .needs = {"bedtime"}},
      {.key = "bed_on", .name = "On at", .kind = TEXT, .pad = CLOCK, .hint = "The time of day, 07:00",
       .needs = {"bedtime", "bed_wake"}},
      {"timezone", "Time zone", TEXT,
       {{"UTC0", "UTC"}, {"EST5EDT,M3.2.0,M11.1.0", "US Eastern"}, {"CST6CDT,M3.2.0,M11.1.0", "US Central"},
        {"MST7MDT,M3.2.0,M11.1.0", "US Mountain"}, {"MST7", "US Arizona"}, {"PST8PDT,M3.2.0,M11.1.0", "US Pacific"},
        {"AKST9AKDT,M3.2.0,M11.1.0", "US Alaska"}, {"HST10", "US Hawaii"}, {"<-03>3", "Brazil"},
        {"GMT0BST,M3.5.0/1,M10.5.0", "UK, Ireland"}, {"CET-1CEST,M3.5.0,M10.5.0/3", "Central Europe"},
        {"EET-2EEST,M3.5.0/3,M10.5.0/4", "Eastern Europe"}, {"IST-5:30", "India"}, {"CST-8", "China"},
        {"JST-9", "Japan, Korea"}, {"AEST-10AEDT,M10.1.0,M4.1.0/3", "Sydney"},
        {"NZST-12NZDT,M9.5.0,M4.1.0/3", "New Zealand"}},
       ALL, WORDS, "", "Another zone as a POSIX TZ string: CET-1CEST,M3.5.0,M10.5.0/3", {"bedtime"}},
      // Another way up restarts the screen when the menu is saved.
      {"rotation", "Rotation", NUMBER,
       {{"-1", "Default"}, {"0", "0\xC2\xB0"}, {"90", "90\xC2\xB0"}, {"180", "180\xC2\xB0"}, {"270", "270\xC2\xB0"}}},
  };
  return table;
}

// The menu's groups, each from the row of its first key to the next group's: a heading over its rows.
struct Group {
  const char *first_key, *name;
};
inline const std::vector<Group> &groups() {
  static const std::vector<Group> table = {
      {"place", "Where"}, {"map", "Map"}, {"marker", "Aircraft"}, {"feed", "Source"}, {"metric", "Screen"}};
  return table;
}
// The group a row is the first of, or nullptr.
inline const Group *group_starting(const Row &row) {
  for (const auto &g : groups())
    if (std::string(g.first_key) == row.key) return &g;
  return nullptr;
}

inline bool steps(const Row &row) { return row.kind == FLAG || !row.choices.empty(); }
inline bool typed(const Row &row) { return row.pad != keyboard::NONE; }
inline size_t most_typed(const Row &row) {
  if (row.most) return row.most;
  return row.pad == keyboard::CLOCK ? 5 : row.pad == keyboard::WORDS ? 60 : 12;
}

// Whether a row shows for `s`: it applies to the map, and the flags it needs are on.
inline bool applies(const Row &row, const Settings &s) {
  if (!(row.maps & bit(s.map))) return false;
  if (row.needs.empty()) return true;
  JsonDocument doc;
  to_json(s, doc.to<JsonObject>());
  for (const char *flag : row.needs)
    if (!doc[flag].as<bool>()) return false;
  return true;
}

// A row's value as the keyboard starts from it: the number or the words, the place's name, coordinates to five places
// (none before there is a place), the watched aircraft each with its colour.
inline std::string text_of(const Row &row, const Settings &s) {
  const std::string key = row.key;
  if (row.kind == PLACE) return s.place;
  if (key == "lat" || key == "lon") {
    if (!s.located) return "";
    char text[24];
    std::snprintf(text, sizeof(text), "%.5f", key == "lat" ? s.lat : s.lon);
    return text;
  }
  if (key == "watch") {
    std::string out;
    for (const auto &w : s.watch) out += (out.empty() ? "" : ", ") + w.id + " " + WATCH_COLOURS[w.colour];
    return out;
  }
  JsonDocument doc;
  to_json(s, doc.to<JsonObject>());
  JsonVariantConst v = doc[row.key];
  if (row.kind == NUMBER) return std::to_string(v.as<int>());
  if (row.kind == FLAG) return v.as<bool>() ? "true" : "false";
  return v.as<const char *>() ? v.as<const char *>() : "";
}

// What a row shows for `s`: its choice's label, "On" or "Off", or what was typed (a number with its unit).
inline std::string shown(const Row &row, const Settings &s) {
  const std::string key = row.key;
  if (row.kind == PLACE) {
    if (!s.place.empty()) return s.place;
    if (!s.located) return "Not set";
    char text[40];
    std::snprintf(text, sizeof(text), "%.3f, %.3f", s.lat, s.lon);
    return text;
  }
  if (key == "watch") return s.watch.empty() ? "None" : std::to_string(s.watch.size()) + " aircraft";
  const std::string now = text_of(row, s);
  if (row.kind == FLAG) {
    // Units is the one flag that is not on or off.
    if (key == "metric") return now == "true" ? "Metric" : "ft, kt";
    return now == "true" ? "On" : "Off";
  }
  for (const auto &c : row.choices)
    if (now == c.value) return c.label;
  if (now.empty()) return key == "lat" || key == "lon" ? "Not set" : "None";
  return row.kind == NUMBER ? now + row.unit : now;
}

// `s` with `row` one step on (`step` 1) or back (-1). Words go round; numbers stop at their ends, and a number between
// two choices steps to the next one its way. The map skips the custom server, and the source a receiver, where none is
// set. False when nothing changes or the settings refuse it.
inline bool stepped(const Row &row, Settings &s, int step) {
  if (!steps(row)) return false;
  JsonDocument now;
  to_json(s, now.to<JsonObject>());
  JsonDocument change;
  if (row.kind == FLAG) {
    change[row.key] = !now[row.key].as<bool>();
  } else {
    const int count = (int) row.choices.size();
    int at = -1;
    if (row.kind == NUMBER) {
      const int value = now[row.key].as<int>();
      for (int i = 0; i < count; ++i)
        if (std::atoi(row.choices[i].value) == value) at = i;
      if (at < 0) {
        // Between choices: the first above it going on, the last below it going back.
        at = step > 0 ? count : -1;
        for (int i = 0; i < count; ++i) {
          const int c = std::atoi(row.choices[i].value);
          if (step > 0 && c > value) { at = i - 1; break; }
          if (step < 0 && c < value) at = i + 1;
        }
      }
      const int next = at + step;
      if (next < 0 || next >= count) return false;
      change[row.key] = std::atoi(row.choices[next].value);
    } else {
      const std::string value = now[row.key].as<const char *>();
      for (int i = 0; i < count; ++i)
        if (value == row.choices[i].value) at = i;
      // One typed that is none of the choices: the first going on, the last going back.
      int next = at < 0 ? (step > 0 ? count - 1 : 0) : at;
      for (int tries = 0; tries < count; ++tries) {
        next = (next + step + count) % count;
        // Choices that need an address are skipped while there is none.
        if (std::string(row.key) == "map" && std::string(row.choices[next].value) == "custom" && s.custom_url.empty())
          continue;
        if (std::string(row.key) == "feed" && std::string(row.choices[next].value) == "receiver" &&
            s.receiver_url.empty())
          continue;
        break;
      }
      if (next == at) return false;
      change[row.key] = row.choices[next].value;
    }
  }
  std::string error;
  return apply(change.as<JsonObjectConst>(), s, error);
}

// `s` with `row` set to what was typed. False, with `s` as it was, when the settings refuse it. Coordinates typed by
// hand are no longer the place found, so the place's name goes with them.
inline bool typed_in(const Row &row, Settings &s, const std::string &typed) {
  const std::string text = keyboard::trimmed(typed), key = row.key;
  JsonDocument change;
  if (row.kind == NUMBER) {
    if (text.empty()) return false;
    char *end = nullptr;
    if (row.pad == keyboard::DECIMAL) {
      change[row.key] = std::strtod(text.c_str(), &end);
    } else {
      const long n = std::strtol(text.c_str(), &end, 10);
      change[row.key] = (int) std::max<long>(-100000, std::min<long>(100000, n));
    }
    if (*end) return false;
    if (key == "lat" || key == "lon") change["place"] = "";
  } else if (key == "watch") {
    // "N12345 red, BAW123": each an aircraft and its colour, red where none is given.
    JsonArray list = change["watch"].to<JsonArray>();
    size_t from = 0;
    while (from <= text.size()) {
      size_t to = text.find_first_of(",\n", from);
      if (to == std::string::npos) to = text.size();
      const std::string item = keyboard::trimmed(text.substr(from, to - from));
      from = to + 1;
      if (item.empty()) continue;
      const size_t gap = item.find(' ');
      JsonObject w = list.add<JsonObject>();
      w["id"] = item.substr(0, gap);
      std::string colour = gap == std::string::npos ? "red" : keyboard::trimmed(item.substr(gap));
      for (char &c : colour) c = (char) std::tolower((unsigned char) c);
      w["colour"] = colour;
    }
  } else {
    change[row.key] = text;
  }
  std::string error;
  return apply(change.as<JsonObjectConst>(), s, error);
}

// Why a Wi-Fi network's name and password cannot be joined, or nullptr when they can: a name is 1 to 32 bytes, a
// password none (an open network) or 8 to 63 characters, or 64 hexadecimal digits.
inline const char *network_refused(const std::string &name, const std::string &password) {
  if (name.empty() || name.size() > 32) return "A network's name is 1 to 32 characters";
  if (password.empty() || (password.size() >= 8 && password.size() <= 63)) return nullptr;
  if (password.size() == 64 && password.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) return nullptr;
  return "A password is 8 to 63 characters, or none for an open network";
}
}  // namespace sky::menu
