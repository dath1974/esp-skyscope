#pragma once
// The settings the screen's own menu offers (a long press on the map opens it), one row each: what it is called, the
// values its arrows step through, and the maps it applies to. A row's change goes through settings.h apply(), as the
// web page's does, so the menu offers nothing the settings would refuse (tests/test_host.cpp walks every value of every
// row through it). The place and the custom tile server stay on the web page, as they take typing, and so does the
// rotation, which restarts the screen at every change.
#include <ArduinoJson.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "settings.h"

namespace sky::menu {

enum Kind : uint8_t { TEXT, NUMBER, FLAG };
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
};

inline const std::vector<Row> &rows() {
  static const std::vector<Row> table = {
      {"map", "Map", TEXT,
       {{"radar", "Radar"}, {"streets", "Streets"}, {"osm", "OSM"}, {"satellite", "Satellite"}, {"topo", "Topo"},
        {"sectional", "Sectional"}, {"ifr_low", "IFR low"}, {"ifr_high", "IFR high"}, {"custom", "Custom"}}},
      {"theme", "Appearance", TEXT, {{"dark", "Dark"}, {"light", "Light"}}, TILE_MAPS},
      {"range_nm", "Range", NUMBER,
       {{"2", "2 NM"}, {"3", "3 NM"}, {"5", "5 NM"}, {"7", "7 NM"}, {"10", "10 NM"}, {"15", "15 NM"}, {"20", "20 NM"},
        {"25", "25 NM"}, {"30", "30 NM"}, {"40", "40 NM"}, {"50", "50 NM"}, {"60", "60 NM"}, {"80", "80 NM"},
        {"100", "100 NM"}, {"150", "150 NM"}, {"200", "200 NM"}, {"250", "250 NM"}}},
      {"tint", "Phosphor", TEXT, {{"green", "Green"}, {"amber", "Amber"}, {"blue", "Blue"}, {"white", "White"}}, SCOPE},
      {"scope", "Scope", FLAG, {}, SCOPE},
      {"sweep", "Beam sweeps", FLAG, {}, SCOPE},
      {"map_brightness", "Map brightness", NUMBER,
       {{"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"}, {"80", "80%"}, {"90", "90%"}, {"100", "100%"}},
       TILE_MAPS},
      {"place_names", "Place names", FLAG, {}, NAMED},
      {"tile_scale", "Map text", NUMBER, {{"0", "Default"}, {"1", "Small"}, {"2", "Large"}}, DRAWN},
      {"rings", "Range rings", FLAG, {}},
      {"airspace", "Airspace", FLAG, {}},
      {"marker", "Marker", TEXT, {{"type", "By type"}, {"plane", "Plane"}, {"arrow", "Arrow"}, {"dot", "Dot"}}},
      {"label", "Labels", TEXT,
       {{"auto", "Auto"}, {"none", "None"}, {"callsign", "Callsign"}, {"altitude", "Altitude"}, {"full", "Full"}}},
      {"track_s", "Tracks", NUMBER, {{"0", "Off"}, {"30", "30 s"}, {"60", "1 min"}, {"120", "2 min"}, {"300", "5 min"}}},
      {"most", "Most planes", NUMBER,
       {{"5", "5"}, {"10", "10"}, {"15", "15"}, {"20", "20"}, {"30", "30"}, {"40", "40"}, {"60", "60"}}},
      {"by_altitude", "Colour by altitude", FLAG, {}},
      {"glide", "Glide", FLAG, {}},
      {"ground", "On the ground", FLAG, {}},
      {"feed", "Source", TEXT, {{"adsb_fi", "adsb.fi"}, {"adsb_lol", "adsb.lol"}, {"receiver", "Receiver"}}},
      {"fallback", "Use the other", FLAG, {}},
      {"refresh_s", "Refresh", NUMBER, {{"5", "5 s"}, {"10", "10 s"}, {"15", "15 s"}, {"30", "30 s"}, {"60", "1 min"}}},
      {"metric", "Units", FLAG, {}},
      {"brightness", "Brightness", NUMBER,
       {{"5", "5%"}, {"10", "10%"}, {"20", "20%"}, {"30", "30%"}, {"40", "40%"}, {"50", "50%"}, {"60", "60%"}, {"70", "70%"},
        {"80", "80%"}, {"90", "90%"}, {"100", "100%"}}},
      {"bedtime", "Bedtime", FLAG, {}},
      {"sleep_min", "Screen off", NUMBER,
       {{"0", "Never"}, {"1", "1 min"}, {"5", "5 min"}, {"10", "10 min"}, {"15", "15 min"}, {"30", "30 min"},
        {"60", "1 hour"}, {"120", "2 hours"}}},
  };
  return table;
}

// The menu's groups, each from the row of its first key to the next group's: a heading over its rows.
struct Group {
  const char *first_key, *name;
};
inline const std::vector<Group> &groups() {
  static const std::vector<Group> table = {
      {"map", "Map"}, {"marker", "Aircraft"}, {"feed", "Source"}, {"metric", "Screen"}};
  return table;
}
// The group a row is the first of, or nullptr.
inline const Group *group_starting(const Row &row) {
  for (const auto &g : groups())
    if (std::string(g.first_key) == row.key) return &g;
  return nullptr;
}

inline bool applies(const Row &row, const Settings &s) { return row.maps & bit(s.map); }

// What a row shows for `s`: its choice's label, "On" or "Off", or for a number not among its choices the number.
inline std::string shown(const Row &row, const Settings &s) {
  JsonDocument doc;
  to_json(s, doc.to<JsonObject>());
  JsonVariantConst v = doc[row.key];
  if (row.kind == FLAG) {
    // Units is the one flag that is not on or off.
    if (std::string(row.key) == "metric") return v.as<bool>() ? "Metric" : "ft, kt";
    return v.as<bool>() ? "On" : "Off";
  }
  const std::string now = row.kind == NUMBER ? std::to_string(v.as<int>()) : std::string(v.as<const char *>());
  for (const auto &c : row.choices)
    if (now == c.value) return c.label;
  return now;
}

// `s` with `row` one step on (`step` 1) or back (-1). Words go round; numbers stop at their ends, and a number between
// two choices steps to the next one its way. The map skips the custom server, and the source a receiver, where none is set. False when nothing
// changes or the settings refuse it.
inline bool stepped(const Row &row, Settings &s, int step) {
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
      int next = at < 0 ? 0 : at;
      for (int tries = 0; tries < count; ++tries) {
        next = (next + step + count) % count;
        // Choices that need an address set on the web page are skipped while there is none.
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
}  // namespace sky::menu
