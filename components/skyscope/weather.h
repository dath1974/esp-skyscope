#pragma once
// Aviation weather for the place watched, for the screen's weather page (wxpage.cpp): the latest METAR and the TAF of
// a station (the one set, or the nearest to the place that reports), the GFS MOS for it, and the SIGMETs, G-AIRMETs
// and pilot reports over the map. The worker asks (worker.cpp); here the addresses and what the answers say.
//
// METARs, TAFs, advisories and pilot reports come from the Aviation Weather Center's data API (aviationweather.gov, a
// US government service: at most 100 requests a minute, with a User-Agent); the MOS from the Iowa Environmental
// Mesonet's API (Iowa State University), which keeps the NWS's MOS bulletins. The page asks for them only while it is
// shown, and again only after REFRESH_MS. Not for flight planning: these are what the services publish, as they do.
#include <ArduinoJson.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "geo.h"

namespace sky::weather {

constexpr uint32_t REFRESH_MS = 10 * 60 * 1000;
constexpr size_t MOST_REPLY_BYTES = 128 * 1024;
inline const char *const AWC = "https://aviationweather.gov/api/data/";
inline const char *const IEM_MOS = "https://mesonet.agron.iastate.edu/api/1/mos.json";

// ---------------------------------------------------------------- addresses

// A station as the services take it: 3 to 4 letters and digits in capitals; "" for anything else.
inline std::string station_id(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c >= 'a' && c <= 'z') c = (char) (c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c != ' ') return "";
  }
  return out.size() >= 3 && out.size() <= 4 ? out : "";
}
inline std::string metar_url(const std::string &id) { return std::string(AWC) + "metar?format=json&ids=" + id; }
inline std::string taf_url(const std::string &id) { return std::string(AWC) + "taf?format=json&ids=" + id; }
inline std::string mos_url(const std::string &id) { return std::string(IEM_MOS) + "?model=GFS&station=" + id; }
inline std::string airsigmet_url() { return std::string(AWC) + "airsigmet?format=json"; }
inline std::string gairmet_url() { return std::string(AWC) + "gairmet?format=json"; }
// A box `half` degrees of latitude either side of a place (wider in longitude, by its latitude), as "s,w,n,e".
inline std::string bbox(double lat, double lon, double half) {
  const double across = half / std::max(0.2, std::cos(geo::radians(lat)));
  char text[96];
  std::snprintf(text, sizeof(text), "%.2f,%.2f,%.2f,%.2f", lat - half, lon - across, lat + half, lon + across);
  return text;
}
// The METARs near a place, to find the nearest station that reports: half a degree around it.
inline std::string nearby_url(double lat, double lon) {
  return std::string(AWC) + "metar?format=json&bbox=" + bbox(lat, lon, 0.5);
}
// Pilot reports over the map: as far as `range_nm` around the place, at least 50 NM.
inline std::string pirep_url(double lat, double lon, int range_nm) {
  return std::string(AWC) + "pirep?format=json&age=2&bbox=" + bbox(lat, lon, std::max(50, range_nm) / 60.0);
}

// ---------------------------------------------------------------- METAR

enum Category : uint8_t { UNKNOWN, VFR, MVFR, IFR, LIFR };
inline const char *const CATEGORY_NAMES[] = {"", "VFR", "MVFR", "IFR", "LIFR"};
inline Category category_of(const char *s) {
  const std::string c = s ? s : "";
  return c == "VFR" ? VFR : c == "MVFR" ? MVFR : c == "IFR" ? IFR : c == "LIFR" ? LIFR : UNKNOWN;
}

struct Cloud {
  std::string cover;  // FEW, SCT, BKN, OVC, SKC, CLR, OVX
  int base_ft = -1;
};
struct Metar {
  std::string id, name, raw, visibility;  // visibility as given: "10+", "1/2", "3"
  double lat = 0, lon = 0;
  int64_t observed = 0;  // Unix seconds
  Category category = UNKNOWN;
  bool has_temp = false, has_wind = false, variable = false;
  double temp_c = 0, dew_c = 0, altim_hpa = 0;
  int wind_dir = 0, wind_kt = 0, gust_kt = 0;
  std::vector<Cloud> clouds;
};

inline bool read_metars(const char *text, size_t length, JsonDocument &doc, std::vector<Metar> &out,
                        std::string &error) {
  out.clear();
  if (length == 0) return true;  // none: the service answers an empty body
  const DeserializationError e = deserializeJson(doc, text, length);
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("the METARs could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  for (JsonObjectConst m : doc.as<JsonArrayConst>()) {
    Metar r;
    r.id = m["icaoId"] | "";
    if (r.id.empty()) continue;
    r.name = m["name"] | "";
    r.raw = m["rawOb"] | "";
    r.lat = m["lat"] | 0.0;
    r.lon = m["lon"] | 0.0;
    r.observed = m["obsTime"] | (int64_t) 0;
    r.category = category_of(m["fltCat"] | "");
    if (m["temp"].is<double>()) {
      r.has_temp = true;
      r.temp_c = m["temp"].as<double>();
      r.dew_c = m["dewp"] | r.temp_c;
    }
    // Wind: degrees, or "VRB" for a variable one.
    if (m["wspd"].is<int>()) {
      r.has_wind = true;
      r.wind_kt = m["wspd"].as<int>();
      r.gust_kt = m["wgst"] | 0;
      if (m["wdir"].is<int>()) r.wind_dir = m["wdir"].as<int>();
      else r.variable = true;
    }
    if (m["visib"].is<const char *>()) r.visibility = m["visib"].as<const char *>();
    else if (m["visib"].is<double>()) {
      char v[16];
      std::snprintf(v, sizeof(v), "%g", m["visib"].as<double>());
      r.visibility = v;
    }
    r.altim_hpa = m["altim"] | 0.0;
    for (JsonObjectConst c : m["clouds"].as<JsonArrayConst>())
      r.clouds.push_back({c["cover"] | "", c["base"].is<int>() ? c["base"].as<int>() : -1});
    out.push_back(r);
  }
  return true;
}

// The station nearest a place among `metars`, -1 for none.
inline int nearest(const std::vector<Metar> &metars, double lat, double lon) {
  int best = -1;
  double closest = 1e18;
  for (size_t i = 0; i < metars.size(); ++i) {
    const double d = geo::meters_between(lat, lon, metars[i].lat, metars[i].lon);
    if (d < closest) {
      closest = d;
      best = (int) i;
    }
  }
  return best;
}

// The ceiling: the lowest broken or overcast layer (or a sky obscured), in feet; -1 for none.
inline int ceiling_ft(const Metar &m) {
  int lowest = -1;
  for (const Cloud &c : m.clouds)
    if ((c.cover == "BKN" || c.cover == "OVC" || c.cover == "OVX" || c.cover == "VV") && c.base_ft >= 0 &&
        (lowest < 0 || c.base_ft < lowest))
      lowest = c.base_ft;
  return lowest;
}

// A METAR in words, a line each: wind, visibility, sky, temperature and dew point, altimeter.
inline std::vector<std::string> decoded(const Metar &m, bool metric) {
  std::vector<std::string> lines;
  char text[96];
  if (m.has_wind) {
    if (m.wind_kt == 0) std::snprintf(text, sizeof(text), "Wind calm");
    else if (m.variable) std::snprintf(text, sizeof(text), "Wind variable, %d kt", m.wind_kt);
    else std::snprintf(text, sizeof(text), "Wind %03d\xC2\xB0 at %d kt", m.wind_dir, m.wind_kt);
    std::string w = text;
    if (m.gust_kt > 0) w += ", gusting " + std::to_string(m.gust_kt) + " kt";
    lines.push_back(w);
  }
  if (!m.visibility.empty()) {
    const std::string v = m.visibility;
    lines.push_back("Visibility " + (v.back() == '+' ? v.substr(0, v.size() - 1) + " SM or more" : v + " SM"));
  }
  std::string sky;
  for (const Cloud &c : m.clouds) {
    if (!sky.empty()) sky += ", ";
    sky += c.cover;
    if (c.base_ft >= 0) sky += " " + std::to_string(c.base_ft) + " ft";
  }
  if (!sky.empty()) lines.push_back("Sky " + sky);
  const int ceiling = ceiling_ft(m);
  if (ceiling >= 0) lines.push_back("Ceiling " + std::to_string(ceiling) + " ft");
  if (m.has_temp) {
    if (metric)
      std::snprintf(text, sizeof(text), "Temperature %.0f\xC2\xB0" "C, dew point %.0f\xC2\xB0" "C", m.temp_c,
                    m.dew_c);
    else
      std::snprintf(text, sizeof(text), "Temperature %.0f\xC2\xB0" "F, dew point %.0f\xC2\xB0" "F",
                    m.temp_c * 9 / 5 + 32, m.dew_c * 9 / 5 + 32);
    lines.push_back(text);
  }
  if (m.altim_hpa > 0) {
    std::snprintf(text, sizeof(text), "Altimeter %.2f inHg (%.0f hPa)", m.altim_hpa * 0.0295299830714, m.altim_hpa);
    lines.push_back(text);
  }
  return lines;
}

// "0751Z", and how long ago from `now` (Unix seconds; 0 when the clock is not set): "0751Z, 23 min ago".
inline std::string when(int64_t t, int64_t now) {
  if (t <= 0) return "";
  const int64_t day = t % 86400;
  char text[48];
  std::snprintf(text, sizeof(text), "%02d%02dZ", (int) (day / 3600), (int) (day / 60 % 60));
  std::string out = text;
  if (now > t) {
    const int64_t ago = (now - t) / 60;
    out += ago < 120 ? ", " + std::to_string(ago) + " min ago" : ", " + std::to_string(ago / 60) + " h ago";
  }
  return out;
}

// ---------------------------------------------------------------- TAF

// "2026-10-30 06:00" or "2026-10-30T05:22:00.000Z" as Unix seconds (UTC).
inline int64_t utc_seconds(const char *s) {
  int y, mo, d, h, mi;
  if (!s || std::sscanf(s, "%d-%d-%d%*c%d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  // Days since 1970 by the civil calendar (Howard Hinnant's days_from_civil).
  y -= mo <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = (int64_t) era * 146097 + (int64_t) doe - 719468;
  return days * 86400 + h * 3600 + mi * 60;
}

struct Taf {
  std::string id, raw;
  int64_t issued = 0;
};
inline bool read_taf(const char *text, size_t length, JsonDocument &doc, Taf &out, std::string &error) {
  out = Taf{};
  if (length == 0) return true;
  const DeserializationError e = deserializeJson(doc, text, length);
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("the TAF could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  JsonObjectConst t = doc[0];
  if (t.isNull()) return true;
  out.id = t["icaoId"] | "";
  out.raw = t["rawTAF"] | "";
  // Unix seconds, or (as the service writes it now) an ISO time.
  out.issued = t["issueTime"].is<int64_t>() ? t["issueTime"].as<int64_t>() : utc_seconds(t["issueTime"] | "");
  return true;
}
// A TAF as it is read, a line for each change: from, temporarily, becoming, with a probability.
inline std::vector<std::string> taf_lines(const std::string &raw) {
  std::vector<std::string> lines;
  std::string line, word;
  auto starts_change = [](const std::string &w) {
    return w.rfind("FM", 0) == 0 && w.size() >= 8 && std::isdigit((unsigned char) w[2]) ? true
           : w == "TEMPO" || w == "BECMG" || w.rfind("PROB", 0) == 0;
  };
  bool after_prob = false;
  for (size_t i = 0; i <= raw.size(); ++i) {
    const char c = i < raw.size() ? raw[i] : ' ';
    if (c != ' ' && c != '\n') {
      word += c;
      continue;
    }
    if (word.empty()) continue;
    // PROB30 TEMPO is one change.
    if (starts_change(word) && !line.empty() && !(after_prob && word == "TEMPO")) {
      lines.push_back(line);
      line.clear();
    }
    after_prob = word.rfind("PROB", 0) == 0;
    line += (line.empty() ? "" : " ") + word;
    word.clear();
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

// ---------------------------------------------------------------- MOS

// One forecast hour of the GFS MOS (the NWS's MAV): its time, temperature and dew point (°F), wind, sky, the ceiling
// and visibility categories (1 the lowest), the 6-hour chance of precipitation (-1 for none given).
struct MosRow {
  int64_t at = 0;  // Unix seconds
  int temp_f = 0, dew_f = 0, wind_dir = 0, wind_kt = 0, cig = 0, vis = 0, pop6 = -1;
  std::string sky;  // CL, FW, SC, BK, OV
};
inline bool read_mos(const char *text, size_t length, JsonDocument &doc, std::vector<MosRow> &out, std::string &error) {
  out.clear();
  JsonDocument keep;
  for (const char *k : {"ftime", "tmp", "dpt", "wdr", "wsp", "cld", "cig", "vis", "p06"}) keep["data"][0][k] = true;
  const DeserializationError e = deserializeJson(doc, text, length, DeserializationOption::Filter(keep));
  if (e || !doc["data"].is<JsonArrayConst>()) {
    error = std::string("the MOS could not be read: ") + (e ? e.c_str() : "no data");
    return false;
  }
  for (JsonObjectConst r : doc["data"].as<JsonArrayConst>()) {
    MosRow m;
    m.at = utc_seconds(r["ftime"] | "");
    if (!m.at || !r["tmp"].is<int>()) continue;
    m.temp_f = r["tmp"].as<int>();
    m.dew_f = r["dpt"] | m.temp_f;
    m.wind_dir = r["wdr"] | 0;
    m.wind_kt = r["wsp"] | 0;
    m.sky = r["cld"] | "";
    m.cig = r["cig"] | 0;
    m.vis = r["vis"] | 0;
    if (r["p06"].is<double>()) m.pop6 = (int) std::lround(r["p06"].as<double>());
    out.push_back(m);
  }
  return true;
}
// The flight category a MOS hour forecasts, from its ceiling and visibility categories: below 500 ft or 1 mile LIFR,
// below 1000 ft or 3 miles IFR, 3000 ft or 5 miles or less MVFR.
inline Category category_of(const MosRow &m) {
  if (!m.cig || !m.vis) return UNKNOWN;
  if (m.cig <= 2 || m.vis <= 2) return LIFR;
  if (m.cig <= 3 || m.vis <= 4) return IFR;
  if (m.cig <= 5 || m.vis <= 5) return MVFR;
  return VFR;
}
// "06Z  58/55°F  230/9 kt  FW  VFR", with its chance of precipitation where it has one.
inline std::string mos_line(const MosRow &m) {
  const int64_t day = m.at % 86400;
  char text[96];
  std::snprintf(text, sizeof(text), "%02dZ  %d/%d\xC2\xB0" "F  %03d/%d kt  %s", (int) (day / 3600), m.temp_f, m.dew_f,
                m.wind_dir, m.wind_kt, m.sky.c_str());
  std::string out = text;
  if (category_of(m) != UNKNOWN) out += std::string("  ") + CATEGORY_NAMES[category_of(m)];
  if (m.pop6 >= 0) out += "  " + std::to_string(m.pop6) + "% precip";
  return out;
}

// ---------------------------------------------------------------- advisories and pilot reports

struct Advisory {
  std::string kind, hazard, words, raw;  // kind: SIGMET, G-AIRMET; words: what more there is (due to, severity)
  int low_ft = -1, high_ft = -1;
  int64_t until = 0;
  std::vector<geo::Place> area;
};
struct Pirep {
  std::string raw, aircraft;
  int level_ft = -1;
  double lat = 0, lon = 0;
  int64_t observed = 0;
};

// Whether an area comes within `nm` of a place: the place inside it, or a corner within reach.
inline bool near(const std::vector<geo::Place> &area, double lat, double lon, double nm) {
  if (area.empty()) return false;
  bool inside = false;
  for (size_t i = 0, j = area.size() - 1; i < area.size(); j = i++) {
    const geo::Place &a = area[i], &b = area[j];
    if ((a.lat > lat) != (b.lat > lat) && lon < (b.lon - a.lon) * (lat - a.lat) / (b.lat - a.lat) + a.lon)
      inside = !inside;
  }
  if (inside) return true;
  for (const geo::Place &p : area)
    if (geo::meters_between(lat, lon, p.lat, p.lon) <= nm * geo::NM) return true;
  return false;
}

inline double number_of(JsonVariantConst v) {
  if (v.is<double>()) return v.as<double>();
  if (v.is<const char *>()) return std::atof(v.as<const char *>());
  return 0;
}
inline std::vector<geo::Place> area_of(JsonArrayConst coords) {
  std::vector<geo::Place> out;
  for (JsonObjectConst c : coords) out.push_back({number_of(c["lat"]), number_of(c["lon"])});
  return out;
}

// SIGMETs (convective ones too) within `nm` of a place.
inline bool read_sigmets(const char *text, size_t length, JsonDocument &doc, double lat, double lon, double nm,
                         std::vector<Advisory> &out, std::string &error) {
  if (length == 0) return true;
  const DeserializationError e = deserializeJson(doc, text, length);
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("the SIGMETs could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  for (JsonObjectConst s : doc.as<JsonArrayConst>()) {
    Advisory a;
    a.area = area_of(s["coords"]);
    if (!near(a.area, lat, lon, nm)) continue;
    a.kind = s["airSigmetType"] | "SIGMET";
    a.hazard = s["hazard"] | "";
    a.raw = s["rawAirSigmet"] | "";
    a.low_ft = s["altitudeLow1"].is<int>() ? s["altitudeLow1"].as<int>() : -1;
    a.high_ft = s["altitudeHi1"].is<int>() ? s["altitudeHi1"].as<int>() : -1;
    a.until = s["validTimeTo"] | (int64_t) 0;
    out.push_back(a);
  }
  return true;
}

// G-AIRMETs (the AIRMETs of the United States) within `nm` of a place, for the earliest forecast hour given (the one
// valid now); each hazard once, with its altitudes.
inline bool read_gairmets(const char *text, size_t length, JsonDocument &doc, double lat, double lon, double nm,
                          std::vector<Advisory> &out, std::string &error) {
  if (length == 0) return true;
  JsonDocument keep;
  for (const char *k : {"forecastHour", "hazard", "due_to", "severity", "top", "base", "expireTime", "coords"})
    keep[0][k] = true;
  const DeserializationError e = deserializeJson(doc, text, length, DeserializationOption::Filter(keep));
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("the G-AIRMETs could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  int first = 1000;
  for (JsonObjectConst g : doc.as<JsonArrayConst>()) first = std::min(first, g["forecastHour"] | 1000);
  for (JsonObjectConst g : doc.as<JsonArrayConst>()) {
    if ((g["forecastHour"] | 1000) != first) continue;
    Advisory a;
    a.area = area_of(g["coords"]);
    if (!near(a.area, lat, lon, nm)) continue;
    a.kind = "G-AIRMET";
    a.hazard = g["hazard"] | "";
    const bool seen = std::any_of(out.begin(), out.end(), [&](const Advisory &o) {
      return o.kind == a.kind && o.hazard == a.hazard;
    });
    if (seen) continue;
    a.words = g["due_to"] | "";
    if (g["severity"].is<const char *>())
      a.words += std::string(a.words.empty() ? "" : ", ") + g["severity"].as<const char *>();
    // Altitudes in hundreds of feet, as strings ("SFC", "FZL", "180").
    auto feet = [](JsonVariantConst v) {
      const char *hundreds = v.is<const char *>() ? v.as<const char *>() : "";
      return std::isdigit((unsigned char) hundreds[0]) ? std::atoi(hundreds) * 100 : -1;
    };
    a.low_ft = feet(g["base"]);
    a.high_ft = feet(g["top"]);
    a.until = g["expireTime"] | (int64_t) 0;
    out.push_back(a);
  }
  return true;
}

inline bool read_pireps(const char *text, size_t length, JsonDocument &doc, std::vector<Pirep> &out,
                        std::string &error) {
  out.clear();
  if (length == 0) return true;  // none: the service answers 204, an empty body
  const DeserializationError e = deserializeJson(doc, text, length);
  if (e || !doc.is<JsonArrayConst>()) {
    error = std::string("the pilot reports could not be read: ") + (e ? e.c_str() : "not a list");
    return false;
  }
  for (JsonObjectConst p : doc.as<JsonArrayConst>()) {
    Pirep r;
    r.raw = p["rawOb"] | "";
    if (r.raw.empty()) continue;
    r.aircraft = p["acType"] | "";
    r.level_ft = p["fltLvl"].is<int>() ? p["fltLvl"].as<int>() * 100 : -1;
    r.lat = p["lat"] | 0.0;
    r.lon = p["lon"] | 0.0;
    r.observed = p["obsTime"] | (int64_t) 0;
    out.push_back(r);
  }
  // The latest first.
  std::sort(out.begin(), out.end(), [](const Pirep &a, const Pirep &b) { return a.observed > b.observed; });
  return true;
}

// "SIGMET CONVECTIVE, to FL400, until 0955Z", "G-AIRMET ICE, 120 to 240 (hundreds of feet)...": an advisory's line.
inline std::string advisory_line(const Advisory &a) {
  std::string out = a.kind + " " + a.hazard;
  auto alt = [](int ft) {
    if (ft == 0) return std::string("the surface");
    if (ft >= 18000) return "FL" + std::to_string(ft / 100);
    return std::to_string(ft) + " ft";
  };
  if (a.low_ft >= 0 && a.high_ft >= 0) out += ", " + alt(a.low_ft) + " to " + alt(a.high_ft);
  else if (a.high_ft >= 0) out += ", to " + alt(a.high_ft);
  if (!a.words.empty()) out += ", " + a.words;
  if (a.until > 0) out += ", until " + when(a.until, 0);
  return out;
}

// All the page shows, as the worker last found it.
struct Report {
  std::string station, error;  // error: the first that went wrong; the rest is what came
  bool has_metar = false;
  Metar metar;
  Taf taf;
  std::vector<MosRow> mos;
  std::vector<Advisory> advisories;
  std::vector<Pirep> pireps;
};
}  // namespace sky::weather
