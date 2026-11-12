#pragma once
// Everything a person can set over HTTP, in one table: its defaults, its limits and its JSON. The web page, the API and
// the stored copy all go through to_json() and apply(), so a key exists in one place only.
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "schedule.h"

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

// The maps a person chooses from. `streets` is Esri's gray canvas, dark or light by the appearance; every other map is
// one map, made dark by tiles.h's night rules where it has no dark one of its own.
enum MapStyle : uint8_t { RADAR, OSM, STREETS, SATELLITE, TOPO, SECTIONAL, IFR_LOW, IFR_HIGH, CUSTOM };
inline constexpr Names<9> MAP_STYLES{
    {"radar", "osm", "streets", "satellite", "topo", "sectional", "ifr_low", "ifr_high", "custom"}};
enum Theme : uint8_t { DARK, LIGHT };
inline constexpr Names<2> THEMES{{"dark", "light"}};
// The public exchanges, a receiver of one's own (readsb, tar1090, dump1090: its aircraft.json at receiver_url), and the
// screen's own radio (radio.h), where it has one.
enum Feed : uint8_t { ADSB_LOL, ADSB_FI, RECEIVER, RADIO };
inline constexpr Names<4> FEEDS{{"adsb_lol", "adsb_fi", "receiver", "radio"}};
// Whether this screen has a radio of its own (the `radio:` of its YAML), set once as it starts. Without one the radio
// is never asked, even preferred, as a receiver without an address is not: the menu skips it, and the others stand in
// where falling back is allowed.
inline bool radio_fitted = false;
enum Tint : uint8_t { GREEN, AMBER, BLUE, WHITE };
inline constexpr Names<4> TINTS{{"green", "amber", "blue", "white"}};
// "plane" is one airliner for every aircraft; "type" each drawn as its kind (shapes.h): a helicopter, a heavy, a drone.
enum MarkerKind : uint8_t { MARKER_DOT, MARKER_ARROW, MARKER_PLANE, MARKER_TYPE };
inline constexpr Names<4> MARKERS{{"dot", "arrow", "plane", "type"}};
enum LabelKind : uint8_t { LABEL_AUTO, LABEL_NONE, LABEL_CALLSIGN, LABEL_ALTITUDE, LABEL_FULL, LABEL_SCOPE };
inline constexpr Names<6> LABELS{{"auto", "none", "callsign", "altitude", "full", "scope"}};
inline constexpr Names<6> WATCH_COLORS{{"red", "orange", "amber", "green", "blue", "purple"}};

inline constexpr int RANGES_NM[] = {2, 3, 5, 8, 10, 15, 20, 30, 40, 60, 80, 120, 160, 250};
// The range goes out to a good part of the world, for following a plane across an ocean (follow.h); the exchanges
// answer for 250 NM around a point at most, so planes come from that far around the place, or around the plane
// followed.
constexpr int MIN_RANGE_NM = 1, MAX_RANGE_NM = 5000, FEED_REACH_NM = 250;
constexpr int ALTITUDE_CEILING = 60000;
constexpr size_t MOST_WATCHED = 12;

struct Watch {
  std::string id;  // a registration, callsign or ICAO hex, upper case
  int color = 0;  // index into WATCH_COLORS
};

struct Settings {
  // Where and how far.
  bool located = false;
  double lat = 0, lon = 0;
  int range_nm = 20;
  // The map under the planes.
  std::string place;       // what the place was found as, an airport's code in capitals ("KJFK"), else a short name
  MapStyle map = STREETS;  // a plain light map to start with: Esri's light gray canvas, by `theme`
  std::string custom_url;  // {z} {x} {y} template, PNG or JPEG
  Theme theme = LIGHT;     // the map's variant or night rendering, and the screen's own colors
  int map_brightness = 100;  // percent of a map's light kept, in either appearance
  int tile_scale = 0;      // 1 or 2 screen pixels per tile pixel; 0 the board's default
  bool rings = true;       // range rings and a compass rose over any map, the scope on `radar`
  bool place_names = true; // a map's layer of place names over it, where it has one (Esri's gray maps and imagery)
  bool airspace = false;   // the FAA's Class B, C and D airspace and runways over any map (airspace.h), United States
  Tint tint = GREEN;
  // The radar's scope: its glow, bearing lines, range rings and rose. Off, the radar is the planes and the airspace
  // alone on the dark ground, in its phosphor, as a controller's bare map.
  bool scope = true;
  bool sweep = true;       // the radar's beam turns, around the scope's rose
  // The aircraft.
  // adsb.fi first: adsb.lol's limits change with its load and went as low as 500 requests a day per address (one every
  // three minutes); providers.h keeps to whatever each one announces.
  Feed feed = ADSB_FI;  // the preferred exchange
  bool fallback = true;  // while it waits (it failed, or asked for fewer requests), the next one in FEEDS
  // A receiver's aircraft.json (http or https), {lat} {lon} {dist} put in where it has them; "" for none, and then the
  // receiver is never asked.
  std::string receiver_url;
  int refresh_s = 10;
  int alt_min = 0, alt_max = ALTITUDE_CEILING;
  bool ground = false;
  int most = 30;
  MarkerKind marker = MARKER_TYPE;
  LabelKind label = LABEL_AUTO;
  bool by_altitude = true;
  bool glide = true;
  int track_s = 60;
  // A photo of the plane picked on its card, from Planespotters.net (photos.h), with its photographer and a QR code
  // to its page.
  bool photos = true;
  std::vector<Watch> watch;
  // The plane followed (follow.h), as it was asked for: a registration (N12345, G-EUPT), a callsign (BAW286), a flight
  // number (BA286) or an ICAO hex; "" for none. follow_hex is its transponder's address once found, kept so a restart
  // over an ocean still knows which plane it is.
  std::string follow, follow_hex;
  // The followed plane's own label, whatever the others' are: a controller's data block unless chosen otherwise.
  LabelKind follow_label = LABEL_SCOPE;
  // Minutes of the followed plane's track, 0 for its whole flight since it was found.
  int follow_track_min = 0;
  // The range the screen had before following a plane first changed it (to the plane's own, follow.h plane_range_nm),
  // 0 while it has not: what the Own range key goes back to, and the range again once following stops.
  int own_range_nm = 0;
  // The screen.
  int rotation = 0;  // degrees: 0, 90, 180 or 270; 0 is the panel's own way up and the cheapest, as nothing is turned
  bool metric = false;
  int brightness = 80;
  int sleep_min = 0;  // the screen goes dark after this many minutes without a touch; 0: never
  // Bedtime: dark every day at bed_off, until a tap or, with bed_wake, bed_on; the times in `timezone` (POSIX TZ).
  bool bedtime = false;
  std::string bed_off = "22:00", bed_on = "07:00";
  bool bed_wake = true;
  std::string timezone = "UTC0";
  // Aviation weather (weather.h): the station whose METAR, TAF and MOS the weather page shows, "" for the nearest to
  // the place that reports; the NEXRAD radar over the map, and whether it plays its last hour as a loop.
  std::string weather_station;
  bool radar_overlay = false;
  bool radar_loop = false;
  // The web page's and the API's password (HTTP Basic, the user "admin"), "" for none. Never written out but to the
  // stored copy: the API says only whether there is one (`password_set`).
  std::string password;
  // The tour of what a finger does here (tour.h) is still to be shown: on a new screen, until it is taken or ended.
  // Set again, the screen shows it again.
  bool tour = true;
};
constexpr size_t MOST_PASSWORD = 32;
constexpr const char *WEB_USER = "admin";

// Whether the radar's scope is drawn, and whether its beam turns: the beam goes around the scope's rose, so not without
// it.
inline bool scope_shown(const Settings &s) { return s.map == RADAR && s.scope; }
inline bool sweeps(const Settings &s) { return scope_shown(s) && s.sweep; }

inline std::string upper_id(const char *s) {
  std::string out;
  for (; s && *s; ++s) {
    const char c = *s;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c >= 'a' && c <= 'z') out += (char) (c - 32);
  }
  return out;
}

// What is typed for a plane to follow, as the settings keep it: upper case letters, digits and dashes (a registration's
// own: G-EUPT), nothing else.
inline std::string follow_id(const char *s) {
  std::string out;
  for (; s && *s; ++s) {
    const char c = *s;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') out += c;
    else if (c >= 'a' && c <= 'z') out += (char) (c - 32);
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  while (!out.empty() && out.front() == '-') out.erase(0, 1);
  return out;
}

// The settings as JSON. `secrets` only for the stored copy: with it the password, else whether there is one.
inline void to_json(const Settings &s, JsonObject o, bool secrets = false) {
  o["located"] = s.located;
  o["lat"] = s.lat;
  o["lon"] = s.lon;
  o["place"] = s.place;
  o["range_nm"] = s.range_nm;
  o["map"] = MAP_STYLES[s.map];
  o["custom_url"] = s.custom_url;
  o["theme"] = THEMES[s.theme];
  o["map_brightness"] = s.map_brightness;
  o["tile_scale"] = s.tile_scale;
  o["rings"] = s.rings;
  o["place_names"] = s.place_names;
  o["airspace"] = s.airspace;
  o["tint"] = TINTS[s.tint];
  o["scope"] = s.scope;
  o["sweep"] = s.sweep;
  o["feed"] = FEEDS[s.feed];
  o["fallback"] = s.fallback;
  o["receiver_url"] = s.receiver_url;
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
  o["photos"] = s.photos;
  JsonArray w = o["watch"].to<JsonArray>();
  for (const auto &item : s.watch) {
    JsonObject e = w.add<JsonObject>();
    e["id"] = item.id;
    e["color"] = WATCH_COLORS[item.color];
  }
  o["follow"] = s.follow;
  o["follow_hex"] = s.follow_hex;
  o["follow_label"] = LABELS[s.follow_label];
  o["follow_track_min"] = s.follow_track_min;
  o["own_range_nm"] = s.own_range_nm;
  o["rotation"] = s.rotation;
  o["metric"] = s.metric;
  o["brightness"] = s.brightness;
  o["sleep_min"] = s.sleep_min;
  o["bedtime"] = s.bedtime;
  o["bed_off"] = s.bed_off;
  o["bed_on"] = s.bed_on;
  o["bed_wake"] = s.bed_wake;
  o["timezone"] = s.timezone;
  o["weather_station"] = s.weather_station;
  o["radar_overlay"] = s.radar_overlay;
  o["radar_loop"] = s.radar_loop;
  o["tour"] = s.tour;
  if (secrets) o["password"] = s.password;
  else o["password_set"] = !s.password.empty();
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
  // Older names: CARTO's basemaps (behind an API key now) and the two gray canvas maps are `streets`, in their look.
  if (!o["map"].isNull()) {
    const char *m = o["map"].as<const char *>();
    const bool dark = m && (!std::strcmp(m, "carto_dark") || !std::strcmp(m, "dark_gray"));
    const bool light = m && (!std::strcmp(m, "carto_light") || !std::strcmp(m, "light_gray"));
    if (dark || light) {
      n.map = STREETS;
      if (o["theme"].isNull()) n.theme = light ? LIGHT : DARK;
    } else if (!choice("map", n.map, MAP_STYLES)) {
      return fail("map");
    }
  }
  if (!o["place"].isNull()) {
    const char *p = o["place"].as<const char *>();
    if (!p || std::strlen(p) > 40) return fail("place");
    n.place = p;
  }
  if (!o["custom_url"].isNull()) {
    const char *url = o["custom_url"].as<const char *>();
    if (!url || std::strlen(url) > 200) return fail("custom_url");
    n.custom_url = url;
    if (!n.custom_url.empty() && (n.custom_url.rfind("http", 0) != 0 || n.custom_url.find("{z}") == std::string::npos))
      return fail("custom_url");
  }
  if (!choice("theme", n.theme, THEMES)) return fail("theme");
  if (!number("map_brightness", n.map_brightness, 10, 100)) return fail("map_brightness");
  if (!number("tile_scale", n.tile_scale, 0, 2)) return fail("tile_scale");
  if (!flag("rings", n.rings)) return fail("rings");
  if (!flag("place_names", n.place_names)) return fail("place_names");
  if (!flag("airspace", n.airspace)) return fail("airspace");
  if (!choice("tint", n.tint, TINTS)) return fail("tint");
  if (!flag("scope", n.scope)) return fail("scope");
  if (!flag("sweep", n.sweep)) return fail("sweep");
  if (!choice("feed", n.feed, FEEDS)) return fail("feed");
  if (!flag("fallback", n.fallback)) return fail("fallback");
  if (!o["receiver_url"].isNull()) {
    const char *url = o["receiver_url"].as<const char *>();
    if (!url || std::strlen(url) > 160) return fail("receiver_url");
    n.receiver_url = url;
    if (!n.receiver_url.empty() && n.receiver_url.rfind("http://", 0) != 0 && n.receiver_url.rfind("https://", 0) != 0)
      return fail("receiver_url");
  }
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
  if (!flag("photos", n.photos)) return fail("photos");
  if (!o["watch"].isNull()) {
    JsonArrayConst list = o["watch"].as<JsonArrayConst>();
    if (list.isNull() || list.size() > MOST_WATCHED) return fail("watch");
    n.watch.clear();
    for (JsonObjectConst item : list) {
      const std::string id = upper_id(item["id"].as<const char *>());
      // Settings stored by older firmware, and older scripts, spell the key "colour".
      const int color = WATCH_COLORS.find(item["color"] | (item["colour"] | "red"));
      if (id.size() < 2 || id.size() > 10 || color < 0) return fail("watch");
      if (std::none_of(n.watch.begin(), n.watch.end(), [&](const Watch &w) { return w.id == id; }))
        n.watch.push_back({id, color});
    }
  }
  // A new plane to follow is found again; one given with its address (a plane's card) is that plane.
  if (!o["follow"].isNull()) {
    const char *t = o["follow"].as<const char *>();
    if (!t) return fail("follow");
    const std::string id = follow_id(t);
    if (id.size() == 1 || id.size() > 10) return fail("follow");
    if (id != n.follow) n.follow_hex.clear();
    n.follow = id;
  }
  if (!o["follow_hex"].isNull()) {
    const char *t = o["follow_hex"].as<const char *>();
    std::string hex;
    for (const char *c = t ? t : ""; *c; ++c) {
      if (!std::isxdigit((unsigned char) *c)) return fail("follow_hex");
      hex += (char) std::tolower((unsigned char) *c);
    }
    if (!t || (!hex.empty() && hex.size() != 6)) return fail("follow_hex");
    n.follow_hex = hex;
  }
  if (n.follow.empty()) n.follow_hex.clear();
  if (!choice("follow_label", n.follow_label, LABELS)) return fail("follow_label");
  if (!number("follow_track_min", n.follow_track_min, 0, 1440)) return fail("follow_track_min");
  if (!number("own_range_nm", n.own_range_nm, 0, MAX_RANGE_NM)) return fail("own_range_nm");
  if (!number("rotation", n.rotation, 0, 270) || n.rotation % 90) return fail("rotation");
  if (!flag("metric", n.metric)) return fail("metric");
  if (!number("brightness", n.brightness, 0, 100)) return fail("brightness");
  if (!number("sleep_min", n.sleep_min, 0, 1440)) return fail("sleep_min");
  if (!flag("bedtime", n.bedtime)) return fail("bedtime");
  if (!flag("bed_wake", n.bed_wake)) return fail("bed_wake");
  using Time = std::pair<const char *, std::string *>;
  for (const auto &[key, field] : {Time{"bed_off", &n.bed_off}, Time{"bed_on", &n.bed_on}}) {
    if (o[key].isNull()) continue;
    const char *t = o[key].as<const char *>();
    if (!t || schedule::minutes(t) < 0) return fail(key);
    *field = t;
  }
  if (!o["timezone"].isNull()) {
    const char *t = o["timezone"].as<const char *>();
    schedule::Zone zone;
    if (!t || std::strlen(t) > 60 || !schedule::parse(t, zone)) return fail("timezone");
    n.timezone = t;
  }
  if (!o["weather_station"].isNull()) {
    const char *w = o["weather_station"].as<const char *>();
    if (!w || std::strlen(w) > 4) return fail("weather_station");
    std::string id;
    for (const char *c = w; *c; ++c) {
      if (!std::isalnum((unsigned char) *c)) return fail("weather_station");
      id += (char) std::toupper((unsigned char) *c);
    }
    if (!id.empty() && id.size() < 3) return fail("weather_station");
    n.weather_station = id;
  }
  if (!flag("radar_overlay", n.radar_overlay)) return fail("radar_overlay");
  if (!flag("radar_loop", n.radar_loop)) return fail("radar_loop");
  if (!flag("tour", n.tour)) return fail("tour");
  if (!o["password"].isNull()) {
    const char *p = o["password"].as<const char *>();
    if (!p || std::strlen(p) > MOST_PASSWORD) return fail("password");
    for (const char *c = p; *c; ++c)
      if ((unsigned char) *c < 0x20 || (unsigned char) *c > 0x7E) return fail("password");
    n.password = p;
  }
  s = std::move(n);
  return true;
}

}  // namespace sky
