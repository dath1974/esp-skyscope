#pragma once
// What the screen has seen in its range: each plane's visit (from when it came within the range to when it was last
// heard there), the day's tally (how many planes, by the hour, the commonest types and operators, the nearest, highest
// and fastest), a log of the latest visits, and the alerts a visit raises: a watched plane coming into range, or a plane
// squawking an emergency. Only reports around the place count: while a plane is followed the planes come from around
// it, and they are no sighting of the screen's own.
//
// Nothing here fetches, keeps the time or writes a file (the worker hands in each report with the time and the local
// date, and writes the log to the map card), so the tests run on a PC. Everything that grows with the sky is in PSRAM.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "feed.h"
#include "geo.h"
#include "overlay.h"
#include "psram.h"
#include "settings.h"

namespace sky::sightings {

// A visit ends once the plane has not been heard in range for this long; heard again later, it is a new visit (and
// counted again in the log, but not in the day's planes).
constexpr int64_t VISIT_GAP_S = 10 * 60;
// The visits the log keeps, the newest last, and the planes the day's tally tells apart (past that they are still
// counted, but one heard again may be counted twice); the commonest types and operators kept with a day.
constexpr size_t MOST_LOG = 400, MOST_DAY_PLANES = 6000, MOST_TOP = 12;
// The days whose number of planes is kept, today's among them.
constexpr size_t MOST_DAYS = 14;

template<size_t N> inline void copy(char (&to)[N], const std::string &from) {
  const size_t n = std::min(from.size(), N - 1);
  std::memcpy(to, from.data(), n);
  to[n] = 0;
}

struct Visit {
  char hex[7] = "", callsign[11] = "", registration[13] = "", type[7] = "", operator_name[41] = "";
  int64_t first = 0, last = 0;  // Unix seconds
  float closest_m = 0;          // its nearest to the place, along the ground
  int32_t lowest_ft = INT32_MAX, highest_ft = INT32_MIN;  // its altitudes seen in range; none while INT32_MAX
  bool watched = false, emergency = false;
  char squawk[5] = "";  // the emergency's code, while `emergency`
};

// A name and how often it was seen: a type ("B738") or an operator.
struct Count {
  char name[41] = "";
  uint32_t n = 0;
};
// The day's nearest (meters), highest (feet) and fastest (knots), with the plane's name and when.
struct Record {
  bool set = false;
  float value = 0;
  char name[13] = "";
  int64_t at = 0;
};
struct Day {
  uint32_t date = 0;  // yyyymmdd, the screen's local date; 0 before the first report
  uint32_t planes = 0;  // told apart by their address
  uint32_t visits = 0, watched = 0, emergencies = 0;
  uint32_t busiest = 0;  // the most in range at once
  uint16_t hours[24] = {};  // planes first seen in each hour of the day
  Record closest, highest, fastest;
};
struct DayCount {
  uint32_t date = 0, planes = 0;
};

enum Kind : uint8_t { WATCHED, EMERGENCY };
inline constexpr const char *KINDS[] = {"watched", "emergency"};
struct Alert {
  Kind kind = WATCHED;
  std::string hex, name;  // name: overlay::name_of, its callsign or registration
  std::string text;       // in words, for the glass and Home Assistant: "N12345 watched, 8.4 NM NE"
  int color = 0;          // a watched plane's (overlay::watch_color), 0 for none
  double meters = 0, bearing = 0;
};

// The plane is one to count: anything but a vehicle or an obstacle on the ground (category C).
inline bool counts(const feed::Flight &f) { return !(f.category.size() == 2 && f.category[0] == 'C'); }

// The words of an alert: who, why, how far and which way.
inline std::string alert_text(Kind kind, const feed::Flight &f, double meters, double bearing, bool metric) {
  std::string out = overlay::name_of(f);
  if (kind == EMERGENCY) out += " squawks " + (f.squawk.empty() ? std::string("an emergency") : f.squawk);
  else out += " is in range";
  out += ", " + overlay::distance_text(meters, metric) + " " + geo::compass_point(bearing);
  return out;
}

class Book {
 public:
  Book() {
    log_.reserve(MOST_LOG);
    days_.reserve(MOST_DAYS);
  }

  // A report around the place at `unix_s` (Unix seconds), on local date `date` (yyyymmdd) and hour: the planes within the
  // range counted, visits begun and carried on, alerts for the ones that call for one, and visits over (heard last more
  // than VISIT_GAP_S ago) moved to the log and handed back in `ended`, for the card's log.
  void see(const std::vector<feed::Flight> &flights, const Settings &s, int64_t unix_s, uint32_t date, int hour,
           std::vector<Alert> &alerts, std::vector<Visit> &ended) {
    if (!s.located || !date) return;
    // A new day: the visits under way end with the old one, so each plane is counted in the new day too.
    if (date != day_.date) {
      close_all(ended);
      new_day_(date);
    }
    const double range_m = std::min(s.range_nm, FEED_REACH_NM) * geo::NM;
    uint32_t in_range = 0;
    for (const feed::Flight &f : flights) {
      if (f.estimated || !counts(f)) continue;
      const double meters = geo::meters_between(s.lat, s.lon, f.lat, f.lon);
      if (meters > range_m) continue;
      ++in_range;
      Visit *v = open_visit_(f.hex);
      const int color = overlay::watch_color(f, s.watch);
      if (!v) {
        open_.push_back(Visit{});
        v = &open_.back();
        copy(v->hex, f.hex);
        v->first = unix_s;
        v->closest_m = (float) meters;
        ++day_.visits;
        if (!seen_today_(f.hex)) {
          ++day_.planes;
          if (hour >= 0 && hour < 24) ++day_.hours[hour];
          tally_(types_, f.type);
          tally_(operators_, f.operator_name);
          if (!days_.empty()) days_.back().planes = day_.planes;
        }
        if (color) {
          v->watched = true;
          ++day_.watched;
          alerts.push_back(alert_(WATCHED, f, s, meters, color));
        }
      }
      v->last = unix_s;
      if (!f.callsign.empty()) copy(v->callsign, f.callsign);
      if (!f.registration.empty()) copy(v->registration, f.registration);
      if (!f.type.empty()) copy(v->type, f.type);
      if (!f.operator_name.empty()) copy(v->operator_name, f.operator_name);
      v->closest_m = std::min(v->closest_m, (float) meters);
      if (f.has_altitude && !f.on_ground) {
        v->lowest_ft = std::min(v->lowest_ft, (int32_t) f.altitude);
        v->highest_ft = std::max(v->highest_ft, (int32_t) f.altitude);
      }
      if (f.emergency && !v->emergency) {
        v->emergency = true;
        copy(v->squawk, f.squawk);
        ++day_.emergencies;
        alerts.push_back(alert_(EMERGENCY, f, s, meters, color));
      }
      const std::string name = overlay::name_of(f);
      record_(day_.closest, (float) meters, name, unix_s, true);
      if (f.has_altitude && !f.on_ground) record_(day_.highest, (float) f.altitude, name, unix_s, false);
      if (f.has_speed && !f.on_ground) record_(day_.fastest, f.speed, name, unix_s, false);
    }
    day_.busiest = std::max(day_.busiest, in_range);
    now_in_range_ = in_range;
    for (auto it = open_.begin(); it != open_.end();) {
      if (unix_s - it->last <= VISIT_GAP_S) {
        ++it;
        continue;
      }
      ended.push_back(*it);
      keep_(*it);
      it = open_.erase(it);
    }
  }
  // Every visit still open, ended: before the log is written away for good (a restart), and for a new place, whose
  // planes are not the old one's.
  void close_all(std::vector<Visit> &ended) {
    for (const Visit &v : open_) {
      ended.push_back(v);
      keep_(v);
    }
    open_.clear();
  }

  const Day &today() const { return day_; }
  uint32_t in_range() const { return now_in_range_; }
  // The log, oldest first: the visits over, then the ones still open.
  std::vector<Visit> log() const {
    std::vector<Visit> out;
    out.reserve(log_.size() + open_.size());
    for (size_t i = 0; i < log_.size(); ++i) out.push_back(log_[(log_start_ + i) % log_.size()]);
    out.insert(out.end(), open_.begin(), open_.end());
    return out;
  }
  size_t open_visits() const { return open_.size(); }
  // The day's commonest, most first, at most MOST_TOP.
  std::vector<Count> top_types() const { return top_(types_); }
  std::vector<Count> top_operators() const { return top_(operators_); }
  // The planes of each day kept, oldest first, today's last.
  const std::vector<DayCount, InPsram<DayCount>> &days() const { return days_; }

  // The day's tally as bytes, to keep over a restart (the loop stores them), and back. The log is not kept: the card
  // has it. A day read back that is over by the next report starts a new one then, as any day does.
  std::vector<uint8_t> pack() const {
    std::vector<uint8_t> out;
    put_(out, MAGIC);
    put_(out, day_.date);
    put_(out, day_.planes);
    put_(out, day_.visits);
    put_(out, day_.watched);
    put_(out, day_.emergencies);
    put_(out, day_.busiest);
    for (uint16_t h : day_.hours) put_(out, h);
    for (const Record *r : {&day_.closest, &day_.highest, &day_.fastest}) {
      put_(out, (uint8_t) r->set);
      put_(out, r->value);
      put_(out, r->at);
      put_text_(out, r->name);
    }
    for (const auto *list : {&types_, &operators_}) {
      const std::vector<Count> top = top_(*list, MOST_TOP * 4);
      put_(out, (uint32_t) top.size());
      for (const Count &c : top) {
        put_text_(out, c.name);
        put_(out, c.n);
      }
    }
    put_(out, (uint32_t) days_.size());
    for (const DayCount &d : days_) {
      put_(out, d.date);
      put_(out, d.planes);
    }
    put_(out, (uint32_t) hexes_.size());
    for (uint32_t h : hexes_) put_(out, h);
    return out;
  }
  bool unpack(const uint8_t *data, size_t size) {
    Reader r{data, data + size};
    Book b;
    uint32_t magic = 0, n = 0;
    if (!r.get(magic) || magic != MAGIC) return false;
    bool ok = r.get(b.day_.date) && r.get(b.day_.planes) && r.get(b.day_.visits) && r.get(b.day_.watched) &&
              r.get(b.day_.emergencies) && r.get(b.day_.busiest);
    for (uint16_t &h : b.day_.hours) ok = ok && r.get(h);
    for (Record *rec : {&b.day_.closest, &b.day_.highest, &b.day_.fastest}) {
      uint8_t set = 0;
      ok = ok && r.get(set) && r.get(rec->value) && r.get(rec->at) && r.text(rec->name);
      rec->set = set;
    }
    for (auto *list : {&b.types_, &b.operators_}) {
      ok = ok && r.get(n) && n <= MOST_TOP * 4;
      for (uint32_t i = 0; ok && i < n; ++i) {
        Count c;
        ok = r.text(c.name) && r.get(c.n);
        list->push_back(c);
      }
    }
    ok = ok && r.get(n) && n <= MOST_DAYS;
    for (uint32_t i = 0; ok && i < n; ++i) {
      DayCount d;
      ok = r.get(d.date) && r.get(d.planes);
      b.days_.push_back(d);
    }
    ok = ok && r.get(n) && n <= MOST_DAY_PLANES;
    for (uint32_t i = 0; ok && i < n; ++i) {
      uint32_t h = 0;
      ok = r.get(h);
      b.hexes_.push_back(h);
    }
    if (!ok || r.at != r.end) return false;
    std::sort(b.hexes_.begin(), b.hexes_.end());
    day_ = b.day_;
    types_ = std::move(b.types_);
    operators_ = std::move(b.operators_);
    days_ = std::move(b.days_);
    hexes_ = std::move(b.hexes_);
    return true;
  }

 private:
  static constexpr uint32_t MAGIC = 0x31444B53;  // "SKD1"
  using Counts = std::vector<Count, InPsram<Count>>;

  Visit *open_visit_(const std::string &hex) {
    for (Visit &v : open_)
      if (hex == v.hex) return &v;
    return nullptr;
  }
  // Whether the plane was counted today already; counted now if not.
  bool seen_today_(const std::string &hex) {
    // A made-up address (feed.h made_up_address, "~2ba403") apart from the transponders' 24 bits, by a bit above them.
    const bool made_up = !hex.empty() && hex[0] == '~';
    const uint32_t h = (uint32_t) std::strtoul(hex.c_str() + made_up, nullptr, 16) | (made_up ? 1u << 24 : 0);
    const auto at = std::lower_bound(hexes_.begin(), hexes_.end(), h);
    if (at != hexes_.end() && *at == h) return true;
    if (hexes_.size() < MOST_DAY_PLANES) hexes_.insert(at, h);
    return false;
  }
  void new_day_(uint32_t date) {
    day_ = Day{};
    day_.date = date;
    types_.clear();
    operators_.clear();
    hexes_.clear();
    if (days_.size() >= MOST_DAYS) days_.erase(days_.begin());
    days_.push_back({date, 0});
  }
  static void tally_(Counts &list, const std::string &name) {
    if (name.empty()) return;
    const std::string key = name.substr(0, sizeof(Count::name) - 1);
    for (Count &c : list)
      if (key == c.name) {
        ++c.n;
        return;
      }
    Count c;
    copy(c.name, key);
    c.n = 1;
    list.push_back(c);
  }
  static std::vector<Count> top_(const Counts &list, size_t most = MOST_TOP) {
    std::vector<Count> out(list.begin(), list.end());
    std::stable_sort(out.begin(), out.end(), [](const Count &a, const Count &b) { return a.n > b.n; });
    if (out.size() > most) out.resize(most);
    return out;
  }
  static void record_(Record &r, float value, const std::string &name, int64_t at, bool least) {
    if (r.set && (least ? value >= r.value : value <= r.value)) return;
    r.set = true;
    r.value = value;
    r.at = at;
    copy(r.name, name);
  }
  Alert alert_(Kind kind, const feed::Flight &f, const Settings &s, double meters, int color) const {
    Alert a;
    a.kind = kind;
    a.hex = f.hex;
    a.name = overlay::name_of(f);
    a.color = color;
    a.meters = meters;
    a.bearing = geo::bearing(s.lat, s.lon, f.lat, f.lon);
    a.text = alert_text(kind, f, meters, a.bearing, s.metric);
    return a;
  }
  void keep_(const Visit &v) {
    if (log_.size() < MOST_LOG) {
      log_.push_back(v);
      return;
    }
    log_[log_start_] = v;
    log_start_ = (log_start_ + 1) % MOST_LOG;
  }
  template<typename T> static void put_(std::vector<uint8_t> &out, T v) {
    const auto *p = reinterpret_cast<const uint8_t *>(&v);
    out.insert(out.end(), p, p + sizeof(T));
  }
  static void put_text_(std::vector<uint8_t> &out, const char *s) {
    const size_t n = std::min<size_t>(std::strlen(s), 255);
    out.push_back((uint8_t) n);
    out.insert(out.end(), s, s + n);
  }
  struct Reader {
    const uint8_t *at, *end;
    template<typename T> bool get(T &v) {
      if (end - at < (ptrdiff_t) sizeof(T)) return false;
      std::memcpy(&v, at, sizeof(T));
      at += sizeof(T);
      return true;
    }
    template<size_t N> bool text(char (&to)[N]) {
      uint8_t n = 0;
      if (!get(n) || end - at < n) return false;
      const size_t kept = std::min<size_t>(n, N - 1);
      std::memcpy(to, at, kept);
      to[kept] = 0;
      at += n;
      return true;
    }
  };

  Day day_;
  uint32_t now_in_range_ = 0;
  std::vector<Visit, InPsram<Visit>> open_, log_;
  size_t log_start_ = 0;
  Counts types_, operators_;
  std::vector<uint32_t, InPsram<uint32_t>> hexes_;
  std::vector<DayCount, InPsram<DayCount>> days_;
};

// The log as CSV, a line a visit: what the page downloads and the card keeps (a file a day). Times are the screen's
// local ones, from `local` (Unix seconds to "2026-11-10 14:05:09").
template<typename Local> inline std::string csv_line(const Visit &v, bool metric, Local local) {
  char line[256];
  const double closest = metric ? v.closest_m / 1000.0 : v.closest_m / geo::NM;
  std::string op = v.operator_name;
  for (char &c : op)
    if (c == ',' || c == '"') c = ' ';
  char lowest[16] = "", highest[16] = "";
  if (v.lowest_ft != INT32_MAX) {
    std::snprintf(lowest, sizeof(lowest), "%ld", (long) v.lowest_ft);
    std::snprintf(highest, sizeof(highest), "%ld", (long) v.highest_ft);
  }
  std::snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s,%s,%.1f,%s,%s,%s,%s\n", local(v.first).c_str(),
                local(v.last).c_str(), v.hex, v.callsign, v.registration, v.type, op.c_str(), closest, lowest, highest,
                v.watched ? "watched" : "", v.emergency ? v.squawk : "");
  return line;
}
inline std::string csv_header(bool metric) {
  return std::string("first,last,hex,callsign,registration,type,operator,closest_") + (metric ? "km" : "nm") +
         ",lowest_ft,highest_ft,watched,emergency\n";
}

}  // namespace sky::sightings
