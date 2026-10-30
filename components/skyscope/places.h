#pragma once
// Finding a place typed on the screen's own keyboard, as the web page finds one (page.h): an airport's code (KJFK, JFK)
// in the FAA's airport data first; an airport's name (Teterboro Airport) there too; then anything at all in
// OpenStreetMap's Nominatim. The worker asks (worker.cpp), here only the addresses and what the answers say. Nominatim
// asks for an application's own User-Agent and at most a request a second, and for nothing as you type: the screen asks
// once for each search a person starts, with its User-Agent, and shows OpenStreetMap's credit beside what it found.
#include <ArduinoJson.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "keyboard.h"

namespace sky::places {
using keyboard::trimmed;

// A place found: where, the name the screen shows for it (an airport's code, else the first part of its name, at most
// 40 bytes as the settings take it), the whole of what the source calls it, and the source.
struct Found {
  double lat = 0, lon = 0;
  std::string name, detail, source;
  bool airport = false;
};
constexpr size_t MOST_FOUND = 6;
constexpr size_t MOST_REPLY_BYTES = 64 * 1024;
constexpr size_t MOST_NAME = 40;

inline const char *const FAA_AIRPORTS =
    "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/US_Airport/FeatureServer/0/query";

inline std::string encoded(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char) c;
    } else {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}
inline std::string upper(std::string s) {
  for (char &c : s) c = (char) std::toupper((unsigned char) c);
  return s;
}

// The FAA query for an airport code: 2 to 5 letters and digits; "" for a search that is not one.
inline std::string code_url(const std::string &q) {
  const std::string id = upper(trimmed(q));
  if (id.size() < 2 || id.size() > 5) return "";
  for (char c : id)
    if (!std::isalnum((unsigned char) c)) return "";
  return std::string(FAA_AIRPORTS) + "?f=json&outSR=4326&outFields=IDENT,ICAO_ID,NAME,SERVCITY,STATE&where=" +
         encoded("ICAO_ID='" + id + "' OR IDENT='" + id + "'");
}

// The FAA query for a search that names an airport ("Teterboro Airport", "Boeing Field"): every other word in the
// airport's name (the FAA writes some words short) or its town; "" for a search that names none. Only letters, digits
// and spaces reach the query.
inline std::string named_url(const std::string &q) {
  static const char *const airport_words[] = {"AIRPORT", "AIRFIELD", "AERODROME", "AIRPARK", "AIRSTRIP", "FIELD",
                                              "INTERNATIONAL", "INTL", "REGIONAL", "MUNICIPAL"};
  static const char *const shorter[][2] = {{"FIELD", "FLD"}, {"INTERNATIONAL", "INTL"}, {"REGIONAL", "RGNL"},
                                           {"MUNICIPAL", "MUNI"}, {"MEMORIAL", "MEML"}, {"COUNTY", "CO"}};
  std::vector<std::string> words;
  std::string word;
  const std::string up = upper(q) + " ";
  for (char c : up) {
    if (std::isalnum((unsigned char) c)) {
      word += c;
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
    }
  }
  bool airport = false;
  for (const auto &w : words)
    for (const char *a : airport_words)
      if (w == a) airport = true;
  if (!airport) return "";
  std::string where = "TYPE_CODE='AD'";
  bool any = false;
  for (const auto &w : words) {
    if (w == "AIRPORT" || w == "AIRFIELD" || w == "AERODROME" || w == "AIRSTRIP") continue;
    any = true;
    where += " AND (UPPER(NAME) LIKE '%" + w + "%'";
    for (const auto &s : shorter)
      if (w == s[0]) where += std::string(" OR UPPER(NAME) LIKE '%") + s[1] + "%'";
    where += " OR UPPER(SERVCITY) LIKE '%" + w + "%')";
  }
  if (!any) return "";
  return std::string(FAA_AIRPORTS) + "?f=json&outSR=4326&resultRecordCount=6&orderByFields=PRIVATEUSE" +
         "&outFields=IDENT,ICAO_ID,NAME,SERVCITY,STATE&where=" + encoded(where);
}

inline std::string nominatim_url(const std::string &q) {
  return "https://nominatim.openstreetmap.org/search?format=json&limit=6&extratags=1&q=" + encoded(trimmed(q));
}

// The FAA's airports in an answer, each named by its identifier (its ICAO code where it has one).
inline bool read_faa(const char *text, size_t length, JsonDocument &doc, std::vector<Found> &out, std::string &error) {
  const DeserializationError e = deserializeJson(doc, text, length);
  if (e) {
    error = std::string("the FAA's answer could not be read: ") + e.c_str();
    return false;
  }
  if (!doc["error"].isNull()) {
    error = "the FAA's server refused the search";
    return false;
  }
  for (JsonObjectConst f : doc["features"].as<JsonArrayConst>()) {
    JsonObjectConst a = f["attributes"], g = f["geometry"];
    if (g.isNull() || !g["x"].is<double>() || !g["y"].is<double>()) continue;
    const char *icao = a["ICAO_ID"] | "", *ident = a["IDENT"] | "";
    const std::string id = upper(*icao ? icao : ident);
    if (id.empty()) continue;
    Found p;
    p.lat = g["y"].as<double>();
    p.lon = g["x"].as<double>();
    p.name = keyboard::clipped(id, MOST_NAME);
    p.detail = id + " " + (a["NAME"] | "") + ", " + (a["SERVCITY"] | "") + ", " + (a["STATE"] | "");
    p.source = "FAA";
    p.airport = true;
    out.push_back(p);
    if (out.size() >= MOST_FOUND) break;
  }
  return true;
}

// Nominatim's places in an answer. An airport is named by its code: the one searched for when that was a code, else
// the code OpenStreetMap tags it with (ICAO first); any other place by the first part of its name.
inline bool read_nominatim(const char *text, size_t length, const std::string &q, JsonDocument &doc,
                           std::vector<Found> &out, std::string &error) {
  JsonDocument keep;
  for (const char *k : {"lat", "lon", "display_name", "type", "class"}) keep[0][k] = true;
  for (const char *k : {"icao", "faa", "iata"}) keep[0]["extratags"][k] = true;
  const DeserializationError e = deserializeJson(doc, text, length, DeserializationOption::Filter(keep));
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("OpenStreetMap's answer could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  const std::string query = trimmed(q);
  bool code = query.size() >= 3 && query.size() <= 4;
  for (char c : query)
    if (!std::isalnum((unsigned char) c)) code = false;
  for (JsonObjectConst p : doc.as<JsonArrayConst>()) {
    const char *lat = p["lat"] | "", *lon = p["lon"] | "";
    char *end_lat = nullptr, *end_lon = nullptr;
    Found f;
    f.lat = std::strtod(lat, &end_lat);
    f.lon = std::strtod(lon, &end_lon);
    if (end_lat == lat || end_lon == lon) continue;
    f.detail = p["display_name"] | "";
    f.source = "OpenStreetMap";
    const std::string type = p["type"] | "", kind = p["class"] | "";
    const bool airport = f.airport = type == "aerodrome" || kind == "aeroway";
    std::string tagged;
    for (const char *k : {"icao", "faa", "iata"})
      if (tagged.empty() && p["extratags"][k].is<const char *>()) tagged = upper(p["extratags"][k].as<const char *>());
    if (airport && code) f.name = upper(query);
    else if (airport && !tagged.empty()) f.name = tagged;
    else f.name = trimmed(f.detail.substr(0, f.detail.find(',')));
    f.name = keyboard::clipped(f.name, MOST_NAME);
    if (f.name.empty()) continue;
    out.push_back(f);
    if (out.size() >= MOST_FOUND) break;
  }
  return true;
}

// The FAA's airports by that name first, then OpenStreetMap's airports, then its other places, at most MOST_FOUND.
inline std::vector<Found> in_order(const std::vector<Found> &named, const std::vector<Found> &osm) {
  std::vector<Found> out = named;
  for (const bool airports : {true, false})
    for (const auto &p : osm)
      if (p.airport == airports && out.size() < MOST_FOUND) out.push_back(p);
  return out;
}
}  // namespace sky::places
