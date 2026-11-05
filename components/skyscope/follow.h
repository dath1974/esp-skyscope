#pragma once
// Following one plane wherever it flies. A person names it by its registration (N12345, G-EUPT), its callsign
// (BAW286), the flight number on a ticket (BA286, turned into its callsign by the airline's ICAO code below) or its
// transponder's ICAO hex; a tap on a plane's card names it by its own. The worker asks an exchange for it by each of
// these in turn (adsb.lol and adsb.fi both look a plane up worldwide by callsign, registration and hex), and once it is
// found, follows its hex: the planes are asked for round it instead of round the place, the map moves with it, and the
// saved place is left as it was.
//
// The exchanges hear only what their volunteers' receivers on the ground hear, so a plane crossing an ocean is out of
// reach for hours. Meanwhile the screen carries it on along the great circle it was flying, at its last speed, and shows
// it dimmed as an estimate until it is heard again (asked for by its hex every so often). A plane that has been seen in
// the air and then sits on the ground, or is lost low and slow near the ground, has landed: following stops by itself,
// and so it does when a plane has been lost for half a day.
//
// Nothing here fetches or keeps the time itself (millis() is handed in), so the tests run on a PC.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "feed.h"
#include "geo.h"
#include "psram.h"
#include "settings.h"

namespace sky::follow {

// The airlines a flight number is understood for: IATA's two-character code and ICAO's three letters, which is what
// a plane's transponder sends as the start of its callsign. Kept by hand, the larger airlines of every continent; any
// other is followed by its callsign or registration typed in full.
struct Airline {
  const char *iata, *icao;
};
inline constexpr Airline AIRLINES[] = {
    // North America
    {"AA", "AAL"}, {"AS", "ASA"}, {"B6", "JBU"}, {"DL", "DAL"}, {"F9", "FFT"}, {"G4", "AAY"}, {"HA", "HAL"},
    {"NK", "NKS"}, {"UA", "UAL"}, {"WN", "SWA"}, {"SY", "SCX"}, {"QX", "QXE"}, {"OO", "SKW"}, {"YX", "RPA"},
    {"9E", "EDV"}, {"MQ", "ENY"}, {"OH", "JIA"}, {"YV", "ASH"}, {"G7", "GJS"}, {"PT", "PDT"}, {"ZW", "AWI"},
    {"C5", "UCA"}, {"MX", "MXY"}, {"XP", "VXP"}, {"AC", "ACA"}, {"WS", "WJA"}, {"TS", "TSC"}, {"PD", "POE"},
    {"QK", "JZA"}, {"F8", "FLE"}, {"AM", "AMX"}, {"Y4", "VOI"}, {"VB", "VIV"}, {"FX", "FDX"}, {"5X", "UPS"},
    {"5Y", "GTI"}, {"K4", "CKS"}, {"PO", "PAC"},
    // Central and South America
    {"CM", "CMP"}, {"AV", "AVA"}, {"LA", "LAN"}, {"JJ", "TAM"}, {"G3", "GLO"}, {"AD", "AZU"}, {"AR", "ARG"},
    // Europe
    {"BA", "BAW"}, {"VS", "VIR"}, {"U2", "EZY"}, {"FR", "RYR"}, {"EI", "EIN"}, {"LH", "DLH"}, {"LX", "SWR"},
    {"OS", "AUA"}, {"SN", "BEL"}, {"AF", "AFR"}, {"KL", "KLM"}, {"IB", "IBE"}, {"VY", "VLG"}, {"UX", "AEA"},
    {"TP", "TAP"}, {"AZ", "ITY"}, {"SK", "SAS"}, {"AY", "FIN"}, {"DY", "NAX"}, {"LO", "LOT"}, {"TK", "THY"},
    {"PC", "PGT"}, {"A3", "AEE"}, {"W6", "WZZ"}, {"EW", "EWG"}, {"DE", "CFG"}, {"LS", "EXS"}, {"LM", "LOG"},
    {"BT", "BTI"}, {"CV", "CLX"}, {"SU", "AFL"}, {"S7", "SBI"},
    // The Middle East and Africa
    {"EK", "UAE"}, {"EY", "ETD"}, {"QR", "QTR"}, {"SV", "SVA"}, {"GF", "GFA"}, {"WY", "OMA"}, {"FZ", "FDB"},
    {"KU", "KAC"}, {"RJ", "RJA"}, {"LY", "ELY"}, {"MS", "MSR"}, {"ET", "ETH"}, {"KQ", "KQA"}, {"SA", "SAA"},
    {"AT", "RAM"},
    // Asia and the Pacific
    {"AI", "AIC"}, {"6E", "IGO"}, {"UK", "VTI"}, {"SG", "SEJ"}, {"IX", "AXB"}, {"QP", "AKJ"}, {"PK", "PIA"},
    {"UL", "ALK"}, {"BG", "BBC"}, {"HY", "UZB"}, {"KC", "KZR"}, {"J2", "AHY"}, {"SQ", "SIA"}, {"TR", "TGW"},
    {"MH", "MAS"}, {"AK", "AXM"}, {"D7", "XAX"}, {"TG", "THA"}, {"VN", "HVN"}, {"PR", "PAL"}, {"5J", "CEB"},
    {"GA", "GIA"}, {"CX", "CPA"}, {"UO", "HKE"}, {"CI", "CAL"}, {"BR", "EVA"}, {"JL", "JAL"}, {"NH", "ANA"},
    {"KE", "KAL"}, {"OZ", "AAR"}, {"7C", "JJA"}, {"CA", "CCA"}, {"MU", "CES"}, {"CZ", "CSN"}, {"HU", "CHH"},
    {"3U", "CSC"}, {"MF", "CXA"}, {"ZH", "CSZ"}, {"QF", "QFA"}, {"VA", "VOZ"}, {"JQ", "JST"}, {"NZ", "ANZ"},
    {"FJ", "FJI"},
};

inline const char *icao_airline(const std::string &iata) {
  for (const auto &a : AIRLINES)
    if (iata == a.iata) return a.icao;
  return nullptr;
}

// The callsign a flight number flies under: "BA286" or "BA0286" is BAW286, "U21234" EZY1234; "" for something that is
// no flight number of an airline known above. A flight number is the airline's two characters (letters, or a letter
// and a digit), one to four digits and at most one letter after them.
inline std::string callsign_of(const std::string &id) {
  if (id.size() < 3 || id.size() > 7) return "";
  const std::string airline = id.substr(0, 2);
  if (!std::isalnum((unsigned char) airline[0]) || !std::isalnum((unsigned char) airline[1]) ||
      (std::isdigit((unsigned char) airline[0]) && std::isdigit((unsigned char) airline[1])))
    return "";
  std::string number = id.substr(2), suffix;
  if (!number.empty() && std::isalpha((unsigned char) number.back())) {
    suffix = number.back();
    number.pop_back();
  }
  if (number.empty() || number.size() > 4) return "";
  for (char c : number)
    if (!std::isdigit((unsigned char) c)) return "";
  const char *icao = icao_airline(airline);
  if (!icao) return "";
  const size_t first = std::min(number.find_first_not_of('0'), number.size() - 1);
  return icao + number.substr(first) + suffix;
}

// The ways a plane is asked for, in the order they are tried.
enum By : uint8_t { BY_CALLSIGN, BY_REGISTRATION, BY_HEX };
struct Ask {
  By by;
  std::string id;
  bool operator==(const Ask &o) const { return by == o.by && id == o.id; }
};

// What to ask for `id` (as settings.h follow_id keeps it): only its hex, where it is known; else its callsign (a flight
// number's first), its registration (the exchanges keep it with its dash, so "GEUPT" is asked as G-EUPT and GE-UPT too,
// all but an American N-number, which has none), and its hex where it could be one.
inline std::vector<Ask> asks(const std::string &id, const std::string &hex) {
  std::vector<Ask> out;
  auto add = [&](By by, const std::string &what) {
    if (what.empty()) return;
    const Ask a{by, what};
    if (std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
  };
  if (!hex.empty()) {
    add(BY_HEX, hex);
    return out;
  }
  std::string bare;
  for (char c : id)
    if (c != '-') bare += c;
  if (bare.size() < 2) return out;
  add(BY_CALLSIGN, callsign_of(bare));
  if (bare.size() <= 8) add(BY_CALLSIGN, bare);
  if (id.find('-') != std::string::npos) {
    add(BY_REGISTRATION, id);
  } else {
    add(BY_REGISTRATION, bare);
    if (bare[0] != 'N' && bare.size() >= 4) {
      add(BY_REGISTRATION, bare.substr(0, 1) + "-" + bare.substr(1));
      add(BY_REGISTRATION, bare.substr(0, 2) + "-" + bare.substr(2));
    }
  }
  bool hexish = bare.size() == 6;
  for (char c : bare)
    if (!std::isxdigit((unsigned char) c)) hexish = false;
  if (hexish) {
    std::string lower = bare;
    for (char &c : lower) c = (char) std::tolower((unsigned char) c);
    add(BY_HEX, lower);
  }
  return out;
}

// The exchange's address for one ask. Only the public exchanges look a plane up worldwide: a receiver of one's own
// or the screen's radio hears only what is near, so a plane is looked up at adsb.lol or adsb.fi whatever the source.
inline std::string url(Feed feed, const Ask &a) {
  std::string id;
  for (char c : a.id) {
    if (std::isalnum((unsigned char) c) || c == '-') id += c;
  }
  const bool fi = feed == ADSB_FI;
  switch (a.by) {
    case BY_CALLSIGN: return (fi ? "https://opendata.adsb.fi/api/v2/callsign/" : "https://api.adsb.lol/v2/callsign/") + id;
    case BY_REGISTRATION:
      return (fi ? "https://opendata.adsb.fi/api/v2/registration/" : "https://api.adsb.lol/v2/reg/") + id;
    default: return (fi ? "https://opendata.adsb.fi/api/v2/hex/" : "https://api.adsb.lol/v2/hex/") + id;
  }
}
// The exchanges to look a plane up at, the one preferred first.
inline std::vector<Feed> lookup_feeds(Feed preferred) {
  if (preferred == ADSB_FI) return {ADSB_FI, ADSB_LOL};
  return {ADSB_LOL, ADSB_FI};
}

// The plane an answer found: one in the air before one on the ground (a callsign flown again tomorrow is not the plane
// at the gate today), or nullptr for none.
inline const feed::Flight *pick(const std::vector<feed::Flight> &flights) {
  const feed::Flight *found = nullptr;
  for (const auto &f : flights) {
    if (!found || (found->on_ground && !f.on_ground)) found = &f;
  }
  return found;
}

constexpr uint32_t LOST_MS = 60 * 1000;                // no report of it this long: out of reach
constexpr uint32_t LANDED_MS = 10 * 60 * 1000;         // on the ground (or lost near it) this long after flying: landed
constexpr uint32_t GIVE_UP_MS = 12u * 3600 * 1000;     // lost this long: following stops
constexpr uint32_t MOST_ESTIMATE_MS = 10u * 3600 * 1000;  // carried on at most this long, then held where that was
constexpr int LOW_FT = 3000;     // lost below this, coming down or slow: landing, not flying on
constexpr float SLOW_KT = 80;    // too slow to be carried on
// How often the plane is asked for by itself: while it is still to be found (each ask in turn, then a pause), and by
// its hex while it is out of reach of the reports round it.
constexpr uint32_t LOOK_EVERY_MS = 2 * 60 * 1000;
constexpr uint32_t HEX_EVERY_MS = 30 * 1000;
constexpr uint32_t ASK_GAP_MS = 1100;  // between two requests to an exchange: adsb.fi asks for one a second at most

enum Phase : uint8_t { LOOKING, LIVE, LOST, LANDED, GAVE_UP };
inline constexpr const char *PHASES[] = {"looking", "live", "lost", "landed", "gave_up"};

// The plane followed, as the reports have seen it: the last of them, and whether it has flown since following began.
class Follower {
 public:
  void reset() { *this = Follower{}; }
  void seen(const feed::Flight &f, uint32_t now) {
    last_ = f;
    at_ = now;
    known_ = true;
    if (!f.on_ground) {
      flown_ = true;
      ground_since_ = 0;
    } else if (!ground_since_) {
      ground_since_ = now | 1;
    }
  }
  bool known() const { return known_; }
  const feed::Flight &last() const { return last_; }
  const std::string &hex() const { return last_.hex; }
  uint32_t age_ms(uint32_t now) const { return known_ ? now - at_ : 0; }

  Phase phase(uint32_t now) const {
    if (!known_) return LOOKING;
    const uint32_t age = now - at_;
    if (flown_ && ground_since_ && now - ground_since_ >= LANDED_MS) return LANDED;
    if (age < LOST_MS) return LIVE;
    if (flown_ && landing() && age >= LANDED_MS) return LANDED;
    if (age >= GIVE_UP_MS) return GAVE_UP;
    return LOST;
  }
  // Whether it is shown where it is likely to be rather than where it was last seen: lost while flying on.
  bool estimating(uint32_t now) const {
    return phase(now) == LOST && !landing() && last_.has_heading && last_.has_speed && last_.speed >= SLOW_KT;
  }
  // Where it is: last seen, or carried on from there along the great circle it was flying.
  geo::Place where(uint32_t now) const {
    if (!estimating(now)) return {last_.lat, last_.lon};
    const double hours = std::min(now - at_, MOST_ESTIMATE_MS) / 3600000.0;
    return geo::destination(last_.lat, last_.lon, last_.heading, last_.speed * hours * geo::NM);
  }
  // The plane as drawn: the last report of it, moved to where it is likely to be while lost.
  feed::Flight shown(uint32_t now) const {
    feed::Flight f = last_;
    const geo::Place p = where(now);
    f.lat = p.lat;
    f.lon = p.lon;
    return f;
  }

 private:
  // Its last report was of a plane landing: on the ground, or low and coming down or slow.
  bool landing() const {
    if (last_.on_ground) return true;
    if (!last_.has_altitude || last_.altitude >= LOW_FT) return false;
    return (last_.has_climb && last_.climb < 0) || (last_.has_speed && last_.speed < SLOW_KT * 2);
  }
  feed::Flight last_;
  uint32_t at_ = 0, ground_since_ = 0;
  bool known_ = false, flown_ = false;
};

// The view follows the plane in steps rather than every report, as each step draws the map again: it moves once the
// plane is past KEEP of the way from the middle to an edge, and then puts it LEAD of the way back from the middle,
// behind it, so it has the most of the glass ahead of it before the next step.
constexpr double KEEP = 0.6, LEAD = 0.4;

// Whether the plane at (lat, lon) is out past `keep` of the way to the edge of `v` (on round glass, of its radius).
inline bool off_centre(const geo::View &v, double lat, double lon, double keep = KEEP) {
  const geo::Pixel p = v.point(lat, lon);
  const double nx = (p.x - v.width / 2.0) / (v.width / 2.0), ny = (p.y - v.height / 2.0) / (v.height / 2.0);
  return v.round ? std::hypot(nx, ny) > keep : std::max(std::fabs(nx), std::fabs(ny)) > keep;
}
// The middle of the next view: the plane LEAD of the way back from it, towards where it came from. `v` gives the scale.
inline geo::Place centre_for(const geo::View &v, double lat, double lon, bool has_heading, float heading) {
  geo::Place c{lat, lon};
  if (has_heading) {
    const double half = std::min(v.width, v.height) / 2.0 * geo::metres_per_pixel(lat, v.zoom);
    c = geo::destination(lat, lon, heading, LEAD * half);
  }
  c.lat = std::max(-80.0, std::min(80.0, c.lat));
  return c;
}

// The FAA's charts and the USGS map cover the United States alone: the lower 48, Alaska, Hawaii and Puerto Rico.
inline bool in_united_states(double lat, double lon) {
  struct Box { double south, north, west, east; };
  static constexpr Box boxes[] = {{24, 50, -125, -66}, {51, 72, -170, -129}, {18, 23, -161, -154}, {17.5, 18.6, -67.5, -65}};
  for (const Box &b : boxes)
    if (lat >= b.south && lat <= b.north && lon >= b.west && lon <= b.east) return true;
  return false;
}
inline bool united_states_only(MapStyle m) { return m == TOPO || m == SECTIONAL || m == IFR_LOW || m == IFR_HIGH; }

// The middle of the view while a plane is followed, and that plane's hex.
struct Centre {
  bool on = false;
  double lat = 0, lon = 0;
  std::string hex;
  bool operator==(const Centre &o) const { return on == o.on && lat == o.lat && lon == o.lon && hex == o.hex; }
  bool operator!=(const Centre &o) const { return !(*this == o); }
};

// The settings as the glass shows them while a plane is followed: its view's middle for the place, a map of the whole
// world where a chart of the United States would show nothing, no range rings (they measure from the place, and the
// middle now moves), and the plane watched, so it always shows, ringed and labelled.
inline Settings shown(const Settings &s, const Centre &c) {
  if (!c.on) return s;
  Settings out = s;
  out.located = true;
  out.lat = c.lat;
  out.lon = c.lon;
  out.rings = false;
  out.follow_hex = c.hex;
  if (united_states_only(out.map) && !in_united_states(c.lat, c.lon)) out.map = STREETS;
  const std::string hex = upper_id(c.hex.c_str());
  if (!hex.empty() && std::none_of(out.watch.begin(), out.watch.end(), [&](const Watch &w) { return w.id == hex; })) {
    out.watch.push_back({hex, 4});  // blue
  }
  return out;
}

// Where the planes round a followed plane are asked for: round it, out to the view's farthest corner from it, at most
// the exchanges' reach. (lat, lon) is the plane; the view's middle and reach come from `v`.
inline int reach_nm(const geo::View &v, double lat, double lon) {
  const double off = geo::metres_between(lat, lon, v.lat, v.lon);
  return std::max(1, std::min(FEED_REACH_NM, (int) std::ceil((v.reach_metres() + off) / geo::NM * 1.05)));
}

// The followed plane's way so far, for its track: the whole flight by default (settings.h follow_track_min), far longer
// than the other planes' few minutes. A place is kept once the plane has moved MIN_STEP_M from the last one; past
// MOST_POINTS every other one of the older half goes, so a long flight keeps its shape in a few tens of kilobytes of
// PSRAM, finer where it was lately. Only what the reports saw: a stretch out of reach is a straight line across.
class Trail {
 public:
  static constexpr double MIN_STEP_M = 500;
  static constexpr size_t MOST_POINTS = 2000;
  struct Point {
    uint32_t at;
    float lat, lon;
  };
  void clear(const std::string &hex = "") {
    points_.clear();
    hex_ = hex;
  }
  const std::string &hex() const { return hex_; }
  size_t size() const { return points_.size(); }
  void add(double lat, double lon, uint32_t at) {
    if (!points_.empty() &&
        geo::metres_between(points_.back().lat, points_.back().lon, lat, lon) < MIN_STEP_M) return;
    if (points_.size() >= MOST_POINTS) {
      // The older half thinned to every other place: the start of the flight stays, in less detail.
      const size_t half = points_.size() / 2;
      size_t kept = 0;
      for (size_t i = 0; i < half; i += 2) points_[kept++] = points_[i];
      for (size_t i = half; i < points_.size(); ++i) points_[kept++] = points_[i];
      points_.resize(kept);
    }
    points_.push_back({at, (float) lat, (float) lon});
  }
  // Its places in the last `minutes` (0: all of them), oldest first.
  std::vector<Point> since(uint32_t now, int minutes) const {
    std::vector<Point> out;
    for (const auto &p : points_)
      if (minutes <= 0 || now - p.at <= (uint32_t) minutes * 60000) out.push_back(p);
    return out;
  }

 private:
  std::vector<Point, InPsram<Point>> points_;
  std::string hex_;
};

// The trail as a plane's track on `v` (plan.h Plane track: x, y, x, y ... oldest first), at most `most` places evenly
// thinned. A part out beyond FAR_PX of the frame is cut off at its oldest end, as pixels that far would not fit the
// track's 16 bits; the plane is on the frame, so what is cut is long off the glass.
constexpr double FAR_PX = 8000;
inline std::vector<int16_t> track_on(const std::vector<Trail::Point> &points, const geo::View &v, size_t most) {
  std::vector<int16_t> out;
  size_t from = 0;
  for (size_t i = points.size(); i-- > 0;) {
    const geo::Pixel p = v.point(points[i].lat, points[i].lon);
    if (std::fabs(p.x) > FAR_PX || std::fabs(p.y) > FAR_PX) {
      from = i + 1;
      break;
    }
  }
  const size_t count = points.size() - from;
  if (count == 0) return out;
  const size_t take = std::min(count, std::max<size_t>(most, 2));
  const double step = take > 1 ? (count - 1) / (double) (take - 1) : 0;
  for (size_t n = 0; n < take; ++n) {
    const auto &q = points[from + (size_t) std::lround(n * step)];
    const geo::Pixel p = v.point(q.lat, q.lon);
    out.push_back((int16_t) std::lround(p.x));
    out.push_back((int16_t) std::lround(p.y));
  }
  return out;
}

// ---- The route: where a flight is going, from adsbdb (a build with `routes: true` only, __init__.py). Its terms: the
// route data is David Taylor's and Jim Mason's and may not be copied into other databases, so it is asked for each
// flight followed, shown, and kept nowhere. Only the airports it flies from and to: no filed flight plan (those are
// not free), so the screen draws the great circle between them, as a long flight roughly flies.
struct Route {
  bool known = false;
  std::string callsign, from, to;  // the callsign it was asked for, the airports' ICAO codes
  double from_lat = 0, from_lon = 0, to_lat = 0, to_lon = 0;
};
inline std::string route_url(const std::string &callsign) {
  std::string id;
  for (char c : callsign)
    if (std::isalnum((unsigned char) c)) id += (char) std::toupper((unsigned char) c);
  return "https://api.adsbdb.com/v0/callsign/" + id;
}
// adsbdb's answer: {"response": {"flightroute": {"origin": {"icao_code", "latitude", "longitude"}, "destination":
// {...}}}}, or {"response": "unknown callsign"} (no route, and no error).
inline bool read_route(const char *text, size_t length, JsonDocument &doc, Route &out, std::string &error) {
  out.known = false;
  if (const DeserializationError e = deserializeJson(doc, text, length)) {
    error = std::string("adsbdb's answer could not be read: ") + e.c_str();
    return false;
  }
  JsonObjectConst r = doc["response"]["flightroute"];
  if (r.isNull()) return true;
  JsonObjectConst a = r["origin"], b = r["destination"];
  if (!a["latitude"].is<double>() || !a["longitude"].is<double>() || !b["latitude"].is<double>() ||
      !b["longitude"].is<double>())
    return true;
  out.from = a["icao_code"] | "";
  out.to = b["icao_code"] | "";
  out.from_lat = a["latitude"];
  out.from_lon = a["longitude"];
  out.to_lat = b["latitude"];
  out.to_lon = b["longitude"];
  out.known = true;
  return true;
}
// The route's great circle as places, `count` of them from end to end.
inline std::vector<geo::Place> route_places(const Route &r, int count = 64) {
  std::vector<geo::Place> out;
  if (!r.known) return out;
  for (int i = 0; i < count; ++i)
    out.push_back(geo::between(r.from_lat, r.from_lon, r.to_lat, r.to_lon, i / (double) (count - 1)));
  return out;
}
// The route on `v` as lines: x, y, x, y ..., with ROUTE_BREAK, ROUTE_BREAK between two pieces where it leaves the
// frame by far or wraps round the date line.
constexpr int16_t ROUTE_BREAK = INT16_MIN;
inline std::vector<int16_t> route_on(const Route &r, const geo::View &v) {
  std::vector<int16_t> out;
  const double world = geo::TILE_PX * std::pow(2.0, v.zoom);
  bool have = false;
  geo::Pixel last{0, 0};
  for (const geo::Place &p : route_places(r)) {
    const geo::Pixel q = v.point(p.lat, p.lon);
    const bool far = std::fabs(q.x) > FAR_PX || std::fabs(q.y) > FAR_PX;
    if (far || (have && std::fabs(q.x - last.x) > world / 2)) {
      if (!out.empty() && out[out.size() - 1] != ROUTE_BREAK) {
        out.push_back(ROUTE_BREAK);
        out.push_back(ROUTE_BREAK);
      }
      have = false;
      if (far) continue;
    }
    out.push_back((int16_t) std::lround(q.x));
    out.push_back((int16_t) std::lround(q.y));
    last = q;
    have = true;
  }
  while (out.size() >= 2 && out.back() == ROUTE_BREAK) out.resize(out.size() - 2);
  return out;
}

// A view's middle and range that hold every one of `places` on a glass `w` x `h` (round or not), with room to spare:
// the whole route, and the plane on it. The range is what the settings keep, the map's zoom a whole one near it.
struct Fit {
  geo::Place centre;
  int range_nm;
};
inline Fit fit(const std::vector<geo::Place> &places, int w, int h, bool round) {
  Fit out{{0, 0}, MAX_RANGE_NM};
  if (places.empty()) return out;
  // Web Mercator at zoom 0, each longitude taken the short way round from the first, so a route over the date line
  // is one piece.
  double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  for (const auto &p : places) {
    const double lon = places[0].lon + std::remainder(p.lon - places[0].lon, 360.0);
    const geo::Pixel q = geo::world(std::max(-80.0, std::min(80.0, p.lat)), 0, 0);
    const double x = (lon + 180.0) / 360.0 * geo::TILE_PX;
    x0 = std::min(x0, x);
    x1 = std::max(x1, x);
    y0 = std::min(y0, q.y);
    y1 = std::max(y1, q.y);
  }
  const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
  out.centre.lon = std::remainder(cx / geo::TILE_PX * 360.0 - 180.0, 360.0);
  out.centre.lat = geo::degrees(std::atan(std::sinh(geo::PI * (1 - 2 * cy / geo::TILE_PX))));
  // A map's zoom is rounded to a whole one (up as much as half a level) and a round glass has no corners: room for both.
  const double fill = round ? 0.45 : 0.65;
  const double zoom = std::log2(std::min(w * fill / std::max(x1 - x0, 1e-6), h * fill / std::max(y1 - y0, 1e-6)));
  const double nm = geo::metres_per_pixel(out.centre.lat, zoom) * std::min(w, h) / 2 / geo::NM;
  out.range_nm = std::max(MIN_RANGE_NM, std::min(MAX_RANGE_NM, (int) std::ceil(nm)));
  return out;
}

// The view a range round a place takes on a glass `w` x `h`, at the whole zoom a map would have (tiles.h plan).
inline geo::View view_for(double lat, double lon, int range_nm, int w, int h) {
  const double z = geo::zoom_for_radius(lat, range_nm * geo::NM, std::min(w, h));
  return geo::View(lat, lon, std::floor(z + 0.5), w, h);
}
constexpr int PLANE_RANGE_NM = 100;  // the Plane key's view round it

// How long, in words: "40 s", "12 min", "3 h 05 min".
inline std::string age_text(uint32_t ms) {
  const uint32_t s = ms / 1000;
  char text[24];
  if (s < 60) std::snprintf(text, sizeof(text), "%u s", (unsigned) s);
  else if (s < 3600) std::snprintf(text, sizeof(text), "%u min", (unsigned) (s / 60));
  else std::snprintf(text, sizeof(text), "%u h %02u min", (unsigned) (s / 3600), (unsigned) (s / 60 % 60));
  return text;
}

// What the status line says of the plane followed: "Following BAW286", "BAW286 lost 42 min, estimated", "Looking for
// BA286". `name` is what the screen calls it.
inline std::string status(const Follower &f, const std::string &asked, uint32_t now) {
  std::string name = asked;
  if (f.known()) {
    if (!f.last().callsign.empty()) name = f.last().callsign;
    else if (!f.last().registration.empty()) name = f.last().registration;
  }
  switch (f.phase(now)) {
    case LOOKING: return "Looking for " + asked;
    case LIVE: return "Following " + name;
    case LOST:
      return name + " out of reach " + age_text(f.age_ms(now)) + (f.estimating(now) ? ", estimated" : ", last seen");
    case LANDED: return name + " landed";
    default: return name + " lost";
  }
}
}  // namespace sky::follow
