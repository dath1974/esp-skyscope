#pragma once
// Where the screen is, worked out from the planes its own radio hears (radio.h), for a place to pick when none is
// known or the screen has moved. A plane's transponder says where the plane is, not where the receiver is, but the
// planes heard lie around the receiver and the loudest of them are the nearest: a plane's signal falls with the square
// of its distance. So each plane is kept as it was when it was loudest (near its closest pass), and the place is the
// middle of those, the loud ones counting for far more than the faint. A plane can only be heard above the horizon, and
// the lower it flies the nearer that is: one heard farther from the middle than its height lets a radio hear it is a
// bad position or a far-off reflection, and is left out before the middle is found again.
//
// It is rough: a few miles with planes passing near and on every side, tens of miles with only distant ones or with a
// hill or a wall shading one side. The screen offers it as a place to pick, never sets it by itself.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "geo.h"

namespace sky::locate {

constexpr size_t MOST_PLANES = 96;            // planes kept; the faintest goes for a louder one
constexpr uint32_t KEEP_MS = 20 * 60 * 1000;  // a plane not heard with a position for this long is let go
constexpr size_t FEWEST_PLANES = 6;           // fewer than this: no place is offered
constexpr double NM = 1852.0;

// How far a radio hears a plane at `feet` above it: the two radio horizons, the plane's and the antenna's (taken as a
// rooftop's, 30 feet), each 1.23 NM times the square root of the height in feet; a fifth more for the air bending the
// signal over the horizon, and a margin for a position a little out.
inline double horizon_nm(int feet) {
  const double plane = 1.23 * std::sqrt((double) std::max(feet, 300));
  const double antenna = 1.23 * std::sqrt(30.0);
  return 1.2 * (plane + antenna) + 10;
}

// A plane as it was when it was loudest.
struct Heard {
  uint32_t address = 0;
  float lat = 0, lon = 0;
  int altitude = -1;     // feet, -1 when not known
  uint32_t signal = 0;   // the demodulator's power (modes.h): falls with the square of the distance
  uint32_t at = 0;       // millis() of the loudest position
  uint32_t last = 0;     // millis() of the last position
};

// A place worked out: how many planes it came from and about how far out it may be.
struct Fix {
  bool found = false;
  double lat = 0, lon = 0;
  size_t planes = 0;
  double within_nm = 0;
};

class Locator {
 public:
  // A plane's position, heard at `now` at that signal; `altitude` in feet, -1 for none.
  void add(uint32_t address, double lat, double lon, int altitude, uint32_t signal, uint32_t now) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90) return;
    Heard *h = nullptr;
    for (Heard &k : heard_)
      if (k.address == address) h = &k;
    if (!h) {
      if (heard_.size() >= MOST_PLANES) {
        // The faintest goes; a fainter plane than every one kept is not taken.
        auto faintest = std::min_element(heard_.begin(), heard_.end(),
                                         [](const Heard &a, const Heard &b) { return a.signal < b.signal; });
        if (faintest->signal >= signal) return;
        *faintest = Heard{};
        h = &*faintest;
      } else {
        heard_.emplace_back();
        h = &heard_.back();
      }
      h->address = address;
      h->signal = 0;
    }
    h->last = now;
    // Louder than the loudest kept, or that one heard so long ago that the plane may since have turned or landed.
    if (signal >= h->signal || now - h->at >= KEEP_MS) {
      h->lat = (float) lat;
      h->lon = (float) lon;
      h->altitude = altitude;
      h->signal = signal;
      h->at = now;
    }
  }

  // Planes not heard for KEEP_MS let go: called often (the tracker's expire), so a time is never kept long enough for
  // millis() to turn over under it (ticks.h).
  void expire(uint32_t now) {
    heard_.erase(std::remove_if(heard_.begin(), heard_.end(), [&](const Heard &h) { return now - h.last >= KEEP_MS; }),
                 heard_.end());
  }

  void clear() { heard_.clear(); }
  size_t size() const { return heard_.size(); }

  Fix fix() const { return fix_of(heard_); }

  // The middle of the planes heard, each weighted by its loudness (so by one over its distance squared), found again
  // without those too far from it to have been heard at their height.
  static Fix fix_of(const std::vector<Heard> &heard) {
    Fix out;
    if (heard.size() < FEWEST_PLANES) return out;
    uint32_t loudest = 1;
    for (const Heard &h : heard) loudest = std::max(loudest, h.signal);
    std::vector<bool> kept(heard.size(), true);
    double lat = 0, lon = 0;
    for (int pass = 0; pass < 4; ++pass) {
      // Unit vectors added up, so a place across the date line or near a pole comes out right.
      double x = 0, y = 0, z = 0, total = 0;
      size_t used = 0;
      for (size_t i = 0; i < heard.size(); ++i) {
        if (!kept[i]) continue;
        const double w = std::max<uint32_t>(heard[i].signal, 1) / (double) loudest;
        const double phi = geo::radians(heard[i].lat), lambda = geo::radians(heard[i].lon);
        x += w * std::cos(phi) * std::cos(lambda);
        y += w * std::cos(phi) * std::sin(lambda);
        z += w * std::sin(phi);
        total += w;
        ++used;
      }
      if (used < FEWEST_PLANES || total <= 0 || std::hypot(x, y, z) < 1e-9) return out;
      lat = geo::degrees(std::atan2(z, std::hypot(x, y)));
      lon = geo::degrees(std::atan2(y, x));
      bool dropped = false;
      for (size_t i = 0; i < heard.size(); ++i) {
        if (!kept[i]) continue;
        const int feet = heard[i].altitude < 0 ? 40000 : heard[i].altitude;
        if (geo::meters_between(lat, lon, heard[i].lat, heard[i].lon) / NM > horizon_nm(feet)) {
          kept[i] = false;
          dropped = true;
        }
      }
      if (!dropped) break;
    }
    // How far out it may be: the planes' weighted spread around the middle, over the square root of how many count
    // (the loud ones count as more than the faint). Against simulated traffic (tests/test_host.cpp) all but a few places
    // in a hundred came within it.
    double spread = 0, total = 0, squares = 0;
    size_t used = 0;
    for (size_t i = 0; i < heard.size(); ++i) {
      if (!kept[i]) continue;
      const double w = std::max<uint32_t>(heard[i].signal, 1) / (double) loudest;
      const double d = geo::meters_between(lat, lon, heard[i].lat, heard[i].lon) / NM;
      spread += w * d * d;
      total += w;
      squares += w * w;
      ++used;
    }
    if (used < FEWEST_PLANES) return out;
    const double effective = total * total / squares;
    out.found = true;
    out.lat = lat;
    out.lon = lon;
    out.planes = used;
    out.within_nm = std::max(1.2 * std::sqrt(spread / total) / std::sqrt(effective), 1.0);
    return out;
  }

 private:
  std::vector<Heard> heard_;
};

}  // namespace sky::locate
