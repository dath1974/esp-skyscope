#pragma once
// What the messages modes.h finds say, and the planes they add up to: each plane's identity, altitude, speed, track and
// squawk, and its place from the compact position reports (CPR) its transponder sends. The Tracker keeps every plane
// heard in the last minute and hands the ones with a place to the rest of the screen as feed::Flight, as an exchange's
// report would. Nothing here touches the radio or keeps the time itself, so the tests run on a PC.
//
// Field layouts and the CPR arithmetic are those of ICAO Doc 9871 (the extended squitter) and Annex 10, vol. IV, as
// set out in "The 1090 Megahertz Riddle" (Junzi Sun, TU Delft), whose example messages the tests read.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "feed.h"
#include "geo.h"
#include "modes.h"

namespace sky::adsb {

constexpr uint32_t FORGET_MS = 60000;        // a plane not heard for this long is forgotten
constexpr uint32_t POSITION_MS = 60000;      // a place older than this is not shown
constexpr uint32_t PAIR_MS = 10000;          // an even and an odd report this close together make a place
constexpr double MOST_RANGE_NM = 400;        // farther from the receiver than any radio hears: a bad decode
constexpr double FASTEST_KT = 1500;          // a plane that seems to move faster than this has been decoded wrong

// `count` bits from message bit `first` (numbered from 1, as the standards do).
inline uint32_t bits(const uint8_t *m, int first, int count) {
  uint32_t v = 0;
  for (int i = 0; i < count; ++i) {
    const int b = first - 1 + i;
    v = v << 1 | ((m[b / 8] >> (7 - b % 8)) & 1);
  }
  return v;
}
// Bits of the 56-bit ME field of an extended squitter (numbered from 1), which starts at message bit 33.
inline uint32_t me(const uint8_t *m, int first, int count) { return bits(m, 32 + first, count); }

// ---------------------------------------------------------------- fields

// A callsign: eight characters of six bits from ME bits 9 to 56, trimmed.
inline std::string callsign(const uint8_t *m) {
  static const char CHARS[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
  std::string out;
  for (int i = 0; i < 8; ++i) out += CHARS[me(m, 9 + 6 * i, 6)];
  while (!out.empty() && (out.back() == ' ' || out.back() == '#')) out.pop_back();
  return out;
}

// The 12-bit altitude of an airborne position (ME bits 9 to 20), in feet: with its Q bit, 25-foot steps from -1000.
// The 100-foot Gillham code (no Q bit) is rare and left unread.
inline bool altitude12(uint32_t a, int &feet) {
  if (a == 0 || !(a & 0x010)) return false;
  feet = (int) (((a & 0xFE0) >> 1) | (a & 0x00F)) * 25 - 1000;
  return true;
}
// The 13-bit altitude code of a surveillance reply (DF0, 4, 16, 20, bits 20 to 32): the same, with an M bit for meters
// (left unread) besides the Q bit.
inline bool altitude13(uint32_t a, int &feet) {
  if (a == 0 || (a & 0x0040) || !(a & 0x0010)) return false;
  feet = (int) (((a & 0x1F80) >> 2) | ((a & 0x0020) >> 1) | (a & 0x000F)) * 25 - 1000;
  return true;
}
// A 13-bit identity code (DF5, 21 bits 20 to 32; an emergency report's ME bits 12 to 24) as the four digits of a
// squawk. Its bits, first to last: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4.
inline std::string squawk13(uint32_t id) {
  auto b = [id](int k) { return (int) ((id >> k) & 1); };
  const int a = b(7) * 4 + b(9) * 2 + b(11), bb = b(1) * 4 + b(3) * 2 + b(5);
  const int c = b(8) * 4 + b(10) * 2 + b(12), d = b(0) * 4 + b(2) * 2 + b(4);
  char out[8];
  std::snprintf(out, sizeof(out), "%d%d%d%d", a, bb, c, d);
  return out;
}
// A surface position's movement (ME bits 6 to 12) in knots, or -1 for none given.
inline float movement(uint32_t m) {
  if (m == 0 || m > 124) return -1;
  if (m == 1) return 0;
  if (m <= 8) return 0.125f * (float) (m - 1);
  if (m <= 12) return 1 + 0.25f * (float) (m - 8);
  if (m <= 38) return 2 + 0.5f * (float) (m - 13);
  if (m <= 93) return 15 + (float) (m - 39);
  if (m <= 108) return 70 + 2.0f * (float) (m - 94);
  if (m <= 123) return 100 + 5.0f * (float) (m - 109);
  return 175;
}

// ---------------------------------------------------------------- positions (CPR)

// One compact position report: 17 bits each of latitude and longitude within a zone, even or odd.
struct Cpr {
  uint32_t lat = 0, lon = 0;
  bool odd = false;
  bool surface = false;
  uint32_t at = 0;  // millis() when heard
};

inline double modulo(double x, double y) { return x - y * std::floor(x / y); }

// The number of longitude zones at a latitude (NL), for 15 latitude zones a hemisphere.
inline int zones(double lat) {
  lat = std::fabs(lat);
  if (lat < 1e-9) return 59;
  if (lat > 87) return 1;
  if (lat == 87) return 2;
  const double a = 1 - std::cos(geo::PI / 30), c = std::cos(geo::PI / 180 * lat);
  return (int) std::floor(2 * geo::PI / std::acos(1 - a / (c * c)));
}

// The place from an even and an odd airborne report, with no other place to go by: the latitude zone both agree on,
// then the newer report's longitude. False where the two straddle a zone boundary (they were heard either side of it).
inline bool global_airborne(const Cpr &even, const Cpr &odd, double &lat, double &lon) {
  const double ye = even.lat / 131072.0, yo = odd.lat / 131072.0, xe = even.lon / 131072.0, xo = odd.lon / 131072.0;
  const double j = std::floor(59 * ye - 60 * yo + 0.5);
  double late = 360.0 / 60 * (modulo(j, 60) + ye), lato = 360.0 / 59 * (modulo(j, 59) + yo);
  if (late >= 270) late -= 360;
  if (lato >= 270) lato -= 360;
  if (std::fabs(late) > 90 || std::fabs(lato) > 90 || zones(late) != zones(lato)) return false;
  const bool odd_newer = (int32_t) (odd.at - even.at) > 0;
  lat = odd_newer ? lato : late;
  const int nl = zones(lat), ni = std::max(odd_newer ? nl - 1 : nl, 1);
  const double m = std::floor(xe * (nl - 1) - xo * nl + 0.5);
  lon = 360.0 / ni * (modulo(m, ni) + (odd_newer ? xo : xe));
  if (lon >= 180) lon -= 360;
  return true;
}

// The place from one report and a place known to be near it (within half a zone: 180 nm airborne, 45 nm on the
// surface, whose zones are a quarter the size).
inline bool local(const Cpr &c, double ref_lat, double ref_lon, double &lat, double &lon) {
  const double span = c.surface ? 90.0 : 360.0;
  const int i = c.odd ? 1 : 0;
  const double y = c.lat / 131072.0, x = c.lon / 131072.0;
  const double dlat = span / (60 - i);
  const double j = std::floor(ref_lat / dlat) + std::floor(modulo(ref_lat, dlat) / dlat - y + 0.5);
  lat = dlat * (j + y);
  if (std::fabs(lat) > 90) return false;
  const double dlon = span / std::max(zones(lat) - i, 1);
  const double m = std::floor(ref_lon / dlon) + std::floor(modulo(ref_lon, dlon) / dlon - x + 0.5);
  lon = dlon * (m + x);
  if (lon >= 180) lon -= 360;
  if (lon < -180) lon += 360;
  return true;
}

inline double nm_between(double lat1, double lon1, double lat2, double lon2) {
  return geo::meters_between(lat1, lon1, lat2, lon2) / geo::NM;
}

// ---------------------------------------------------------------- the planes

struct Aircraft {
  uint32_t address = 0;
  uint32_t seen = 0;           // millis() of the last message
  uint32_t trusted = 0;        // of the last message that carried its address in the clear (DF11, 17, 18)
  uint32_t messages = 0;
  uint16_t signal = 0;
  std::string callsign, squawk, category;
  bool has_altitude = false;
  int altitude = 0;
  bool has_speed = false, has_heading = false, has_climb = false;
  float speed = 0, heading = 0;
  int climb = 0;
  bool on_ground = false;
  bool emergency = false;
  Cpr even, odd;
  bool has_even = false, has_odd = false;
  bool has_position = false;
  double lat = 0, lon = 0;
  uint32_t position_at = 0;
};

// What the tracker made of the messages, since it started.
struct TrackCounts {
  uint32_t positions = 0;       // places worked out
  uint32_t bad_positions = 0;   // places thrown away: too far, or too fast
  uint32_t unknown = 0;         // overlaid messages from no plane heard lately
};

class Tracker {
 public:
  // Where the receiver is (the screen's place): the surface reports' zones are found from it, and a place farther from
  // it than any radio hears is thrown away. None (`known` false) until it is set.
  void set_reference(double lat, double lon) {
    ref_lat_ = lat;
    ref_lon_ = lon;
    has_ref_ = true;
  }
  // Whether `address` is a plane heard by its own address lately: an overlaid message counts only for one.
  bool known(uint32_t address, uint32_t now) const {
    const auto it = planes_.find(address);
    return it != planes_.end() && it->second.trusted && now - it->second.trusted < FORGET_MS;
  }
  // A message the demodulator found, heard at `now`. False for an overlaid one from no plane heard lately.
  bool add(const modes::Message &m, uint32_t now) {
    if (m.overlaid && !known(m.address, now)) {
      ++counts_.unknown;
      return false;
    }
    Aircraft &a = planes_[m.address];
    a.address = m.address;
    a.seen = now;
    if (!m.overlaid) a.trusted = now;
    ++a.messages;
    a.signal = m.signal;
    const uint8_t *d = m.data;
    switch (m.df) {
      case 0:
      case 4:
      case 16:
      case 20: {
        int feet;
        if (altitude13(bits(d, 20, 13), feet)) {
          a.has_altitude = true;
          a.altitude = feet;
        }
        break;
      }
      case 5:
      case 21:
        a.squawk = squawk13(bits(d, 20, 13));
        break;
      case 11:
        if (bits(d, 6, 3) == 4) a.on_ground = true;
        break;
      case 17:
        if (bits(d, 6, 3) == 4) a.on_ground = true;
        extended(a, d, now);
        break;
      case 18:
        // Only ADS-B sent by a device with an ICAO address (CF 0); TIS-B and rebroadcasts carry other kinds.
        if (bits(d, 6, 3) == 0) extended(a, d, now);
        break;
      default:
        break;
    }
    a.emergency = a.emergency || a.squawk == "7500" || a.squawk == "7600" || a.squawk == "7700";
    return true;
  }
  // Planes not heard for a minute are forgotten.
  void expire(uint32_t now) {
    for (auto it = planes_.begin(); it != planes_.end();) {
      if (now - it->second.seen >= FORGET_MS) it = planes_.erase(it);
      else ++it;
    }
  }
  // The planes with a place heard in the last minute, as an exchange would report them.
  std::vector<feed::Flight> flights(uint32_t now) const {
    std::vector<feed::Flight> out;
    for (const auto &[address, a] : planes_) {
      if (!a.has_position || now - a.position_at >= POSITION_MS) continue;
      feed::Flight f;
      char hex[8];
      std::snprintf(hex, sizeof(hex), "%06x", (unsigned) address);
      f.hex = hex;
      f.callsign = a.callsign;
      f.squawk = a.squawk;
      f.category = a.category;
      f.lat = a.lat;
      f.lon = a.lon;
      f.on_ground = a.on_ground;
      f.has_altitude = a.has_altitude && !a.on_ground;
      f.altitude = a.altitude;
      f.has_speed = a.has_speed;
      f.speed = a.speed;
      f.has_heading = a.has_heading;
      f.heading = a.heading;
      f.has_climb = a.has_climb && !a.on_ground;
      f.climb = a.climb;
      f.emergency = a.emergency;
      out.push_back(std::move(f));
    }
    return out;
  }
  size_t size() const { return planes_.size(); }
  const std::map<uint32_t, Aircraft> &planes() const { return planes_; }
  const TrackCounts &counts() const { return counts_; }

 private:
  // An extended squitter's ME field, by its type code (ME bits 1 to 5).
  void extended(Aircraft &a, const uint8_t *d, uint32_t now) {
    const int tc = (int) me(d, 1, 5);
    if (tc >= 1 && tc <= 4) {
      a.callsign = callsign(d);
      const uint32_t cat = me(d, 6, 3);
      if (cat) {
        const char set = (char) ('A' + (4 - tc));
        a.category = std::string(1, set) + (char) ('0' + cat);
      }
    } else if (tc >= 5 && tc <= 8) {
      a.on_ground = true;
      const float kt = movement(me(d, 6, 7));
      if (kt >= 0) {
        a.has_speed = true;
        a.speed = kt;
      }
      if (me(d, 13, 1)) {
        a.has_heading = true;
        a.heading = me(d, 14, 7) * 360.0f / 128;
      }
      position(a, d, true, now);
    } else if (tc >= 9 && tc <= 18) {
      a.on_ground = false;
      int feet;
      if (altitude12(me(d, 9, 12), feet)) {
        a.has_altitude = true;
        a.altitude = feet;
      }
      position(a, d, false, now);
    } else if (tc >= 20 && tc <= 22) {
      // GNSS height: the place only; the altitude shown stays the barometric one.
      a.on_ground = false;
      position(a, d, false, now);
    } else if (tc == 19) {
      velocity(a, d);
    } else if (tc == 28 && me(d, 6, 3) == 1) {
      a.emergency = me(d, 9, 3) != 0;
      const uint32_t id = me(d, 12, 13);
      if (id) a.squawk = squawk13(id);
    }
  }
  // Airborne velocity: over the ground (subtypes 1 and 2, the second four times as coarse, for supersonic planes) or
  // through the air with a heading (3 and 4); and the rate of climb.
  void velocity(Aircraft &a, const uint8_t *d) {
    const int st = (int) me(d, 6, 3);
    if (st == 1 || st == 2) {
      const uint32_t ew = me(d, 15, 10), ns = me(d, 26, 10);
      if (ew && ns) {
        const int scale = st == 2 ? 4 : 1;
        const double vx = (me(d, 14, 1) ? -1.0 : 1.0) * (ew - 1) * scale;
        const double vy = (me(d, 25, 1) ? -1.0 : 1.0) * (ns - 1) * scale;
        a.has_speed = true;
        a.speed = (float) std::hypot(vx, vy);
        a.has_heading = true;
        a.heading = (float) modulo(std::atan2(vx, vy) * 180 / geo::PI, 360);
      }
    } else if (st == 3 || st == 4) {
      if (me(d, 14, 1)) {
        a.has_heading = true;
        a.heading = me(d, 15, 10) * 360.0f / 1024;
      }
      const uint32_t as = me(d, 26, 10);
      if (as) {
        a.has_speed = true;
        a.speed = (float) ((as - 1) * (st == 4 ? 4 : 1));
      }
    } else {
      return;
    }
    const uint32_t vr = me(d, 38, 9);
    if (vr) {
      a.has_climb = true;
      a.climb = (me(d, 37, 1) ? -1 : 1) * (int) (vr - 1) * 64;
    }
  }
  // A position report: from the plane's own last place where it has a recent one, else from an even and an odd
  // report heard close together (airborne) or the receiver's place (surface); kept only if it is within reach of the
  // receiver and the plane need not have flown impossibly fast to get there.
  void position(Aircraft &a, const uint8_t *d, bool surface, uint32_t now) {
    Cpr c;
    c.surface = surface;
    c.odd = me(d, 22, 1);
    c.lat = me(d, 23, 17);
    c.lon = me(d, 40, 17);
    c.at = now;
    // A surface report and an airborne one do not pair.
    if (c.odd) {
      a.odd = c;
      a.has_odd = true;
      if (a.has_even && a.even.surface != surface) a.has_even = false;
    } else {
      a.even = c;
      a.has_even = true;
      if (a.has_odd && a.odd.surface != surface) a.has_odd = false;
    }
    double lat = 0, lon = 0;
    bool found = false, from_own = false;
    if (a.has_position && now - a.position_at < POSITION_MS) {
      found = local(c, a.lat, a.lon, lat, lon);
      from_own = true;
    } else if (!surface && a.has_even && a.has_odd && now - a.even.at < PAIR_MS && now - a.odd.at < PAIR_MS) {
      found = global_airborne(a.even, a.odd, lat, lon);
    } else if (surface && has_ref_) {
      found = local(c, ref_lat_, ref_lon_, lat, lon);
    }
    if (!found) return;
    if (has_ref_ && nm_between(lat, lon, ref_lat_, ref_lon_) > MOST_RANGE_NM) {
      ++counts_.bad_positions;
      return;
    }
    if (from_own) {
      const double hours = std::max<uint32_t>(now - a.position_at, 1000) / 3600000.0;
      if (nm_between(lat, lon, a.lat, a.lon) > FASTEST_KT * hours + 1) {
        // Out of step with the last place: neither is trusted; the next even and odd pair starts afresh.
        ++counts_.bad_positions;
        a.has_position = false;
        return;
      }
    }
    a.has_position = true;
    a.lat = lat;
    a.lon = lon;
    a.position_at = now;
    ++counts_.positions;
  }

  std::map<uint32_t, Aircraft> planes_;
  TrackCounts counts_;
  double ref_lat_ = 0, ref_lon_ = 0;
  bool has_ref_ = false;
};

}  // namespace sky::adsb
