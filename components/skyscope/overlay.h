#pragma once
// From the feed's planes to what the glass shows: which planes a frame shows, where on it, how they move between
// reports, their labels and their tracks. Nothing here draws or fetches, so the tests run on a PC.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "feed.h"
#include "geo.h"
#include "plan.h"
#include "settings.h"

namespace sky::overlay {
// Altitude bands a marker is coloured by (tar1090's rainbow): on the ground, below 5,000 ft, below 15,000, below
// 30,000, and above.
inline constexpr int BANDS[] = {5000, 15000, 30000};
// A plane is still shown this far beyond the frame, so one that flies in is there at once.
constexpr int MARGIN = 24;
// A plane the feed stopped listing is forgotten after this long, and with it its track.
constexpr uint32_t FORGET_MS = 120000;
constexpr size_t TRACK_POINTS = 10;
constexpr int MOST_TRACK_S = 300;

inline int band(const feed::Flight &f) {
  if (f.on_ground) return 0;
  if (!f.has_altitude) return plan::NO_BAND;
  int b = 1;
  for (const int limit : BANDS) b += f.altitude >= limit;
  return b;
}

// Where each plane has been, with the moment, so a track is the last N seconds of it.
class Tracks {
 public:
  struct Place { uint32_t at; double lat, lon; };
  void update(const std::vector<feed::Flight> &flights, uint32_t now) {
    for (const auto &f : flights) {
      auto &places = seen_[f.hex];
      if (places.empty() || places.back().lat != f.lat || places.back().lon != f.lon) places.push_back({now, f.lat, f.lon});
    }
    for (auto it = seen_.begin(); it != seen_.end();) {
      auto &places = it->second;
      places.erase(std::remove_if(places.begin(), places.end(),
                                  [&](const Place &p) { return now - p.at > (uint32_t) MOST_TRACK_S * 1000; }),
                   places.end());
      if (places.empty() || now - places.back().at > FORGET_MS) it = seen_.erase(it);
      else ++it;
    }
  }
  std::vector<Place> of(const std::string &hex, int seconds, uint32_t now) const {
    std::vector<Place> out;
    const auto found = seen_.find(hex);
    if (found == seen_.end()) return out;
    for (const auto &p : found->second) if (now - p.at <= (uint32_t) seconds * 1000) out.push_back(p);
    return out;
  }
  void clear() { seen_.clear(); }
  size_t size() const { return seen_.size(); }

 private:
  std::map<std::string, std::vector<Place>> seen_;
};

// The colour of a plane someone watches (1 to plan::WATCH_COLOURS), by its registration, callsign or address; 0 for none.
inline int watch_colour(const feed::Flight &f, const std::vector<Watch> &watch) {
  if (watch.empty()) return 0;
  const std::string keys[] = {upper_id(f.registration.c_str()), upper_id(f.callsign.c_str()), upper_id(f.hex.c_str())};
  for (const auto &w : watch)
    for (const auto &k : keys)
      if (!k.empty() && k == w.id) return w.colour + 1;
  return 0;
}

// The planes the frame shows: within the altitudes, on the ground only when asked, inside the frame, the closest to its
// middle first, at most `most`; then every watched plane inside the frame, whatever the rest says.
inline std::vector<const feed::Flight *> shown(const std::vector<feed::Flight> &flights, const Settings &s, const geo::View &view) {
  std::vector<std::pair<double, const feed::Flight *>> near;
  std::vector<const feed::Flight *> watched;
  auto inside = [&](const geo::Pixel &p) {
    return p.x >= -MARGIN && p.x <= view.width + MARGIN && p.y >= -MARGIN && p.y <= view.height + MARGIN;
  };
  for (const auto &f : flights) {
    const geo::Pixel p = view.point(f.lat, f.lon);
    if (watch_colour(f, s.watch)) {
      if (inside(p)) watched.push_back(&f);
      continue;
    }
    if (f.on_ground && !s.ground) continue;
    if (!f.on_ground && f.has_altitude && (f.altitude < s.alt_min || f.altitude > s.alt_max)) continue;
    // A plane without an altitude is shown only when every altitude is.
    if (!f.on_ground && !f.has_altitude && (s.alt_min > 0 || s.alt_max < ALTITUDE_CEILING)) continue;
    if (!inside(p)) continue;
    near.push_back({std::hypot(p.x - view.width / 2.0, p.y - view.height / 2.0), &f});
  }
  std::sort(near.begin(), near.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<const feed::Flight *> out;
  for (size_t i = 0; i < near.size() && (int) i < s.most; ++i) out.push_back(near[i].second);
  out.insert(out.end(), watched.begin(), watched.end());
  return out;
}

// What a label says: the choice, or for `auto` what the frame has room for, `px_per_mm` pixels to the millimetre.
inline LabelKind label_kind(const Settings &s, const geo::View &view, float px_per_mm) {
  if (s.label != LABEL_AUTO) return s.label;
  const float small_mm = std::min(view.width, view.height) / std::max(1.0f, px_per_mm);
  return small_mm < 25 ? LABEL_CALLSIGN : LABEL_ALTITUDE;
}

inline std::string altitude_text(const feed::Flight &f, bool metric) {
  if (f.on_ground || !f.has_altitude) return "";
  char out[24];
  if (metric) std::snprintf(out, sizeof(out), "%ld m", std::lround(f.altitude * 0.3048 / 10) * 10);
  else if (f.altitude >= 18000) std::snprintf(out, sizeof(out), "FL%03ld", std::lround(f.altitude / 100.0));
  else std::snprintf(out, sizeof(out), "%ld ft", std::lround(f.altitude / 100.0) * 100);
  return out;
}
inline std::string speed_text(const feed::Flight &f, bool metric) {
  if (!f.has_speed) return "";
  char out[24];
  if (metric) std::snprintf(out, sizeof(out), "%ld km/h", std::lround(f.speed * 1.852));
  else std::snprintf(out, sizeof(out), "%ld kt", std::lround(f.speed));
  return out;
}
inline std::string name_of(const feed::Flight &f) {
  if (!f.callsign.empty()) return f.callsign;
  if (!f.registration.empty()) return f.registration;
  std::string hex = f.hex;
  for (auto &c : hex) if (c >= 'a' && c <= 'z') c -= 32;
  return hex;
}

// A label, its lines joined by '\n': the name, then its altitude, then its speed and type.
inline std::string label_of(const feed::Flight &f, LabelKind kind, bool metric) {
  if (kind == LABEL_NONE) return "";
  const std::string name = name_of(f);
  if (kind == LABEL_CALLSIGN) return name;
  const std::string altitude = f.on_ground ? std::string("GND") : altitude_text(f, metric);
  if (kind == LABEL_ALTITUDE) return altitude.empty() ? name : name + "\n" + altitude;
  std::string first = name, second = altitude;
  if (!f.type.empty()) first += " " + f.type;
  const std::string speed = speed_text(f, metric);
  if (!speed.empty()) second += (second.empty() ? "" : " ") + speed;
  return second.empty() ? first : first + "\n" + second;
}

// Where a plane goes in a minute on the frame, in pixels: its heading and ground speed at the view's zoom.
inline std::pair<int, int> velocity(const feed::Flight &f, const geo::View &view) {
  if (f.on_ground || !f.has_speed || !f.has_heading) return {0, 0};
  const double per_minute = f.speed * 0.514444 * 60 / geo::metres_per_pixel(f.lat, view.zoom);
  const double a = geo::radians(f.heading);
  return {(int) std::lround(std::sin(a) * per_minute), (int) std::lround(-std::cos(a) * per_minute)};
}

template<typename T> std::vector<T> thin(const std::vector<T> &points, size_t most) {
  if (points.size() <= most || most < 2) return points;
  std::vector<T> out;
  const double step = (points.size() - 1) / (double) (most - 1);
  for (size_t n = 0; n < most; ++n) out.push_back(points[(size_t) std::lround(n * step)]);
  return out;
}

// The overlay of the frame: every plane shown, watched ones last so they lie on top.
inline plan::Overlay build(const std::vector<feed::Flight> &flights, const Settings &s, const geo::View &view,
                           const Tracks &tracks, uint32_t now, float px_per_mm, bool radar) {
  plan::Overlay o;
  o.width = view.width;
  o.height = view.height;
  o.marker = static_cast<plan::Marker>(s.marker);
  o.by_altitude = s.by_altitude;
  o.glide = s.glide;
  o.at = now;
  const LabelKind kind = label_kind(s, view, px_per_mm);
  for (const feed::Flight *f : shown(flights, s, view)) {
    plan::Plane p;
    const geo::Pixel at = view.point(f->lat, f->lon);
    p.x = (int) std::lround(at.x);
    p.y = (int) std::lround(at.y);
    p.heading = f->has_heading ? (int) std::lround(f->heading) % 360 : plan::NO_HEADING;
    p.band = band(*f);
    // A scope's planes carry their motion whether they glide or not: its vector is where a plane is a minute on.
    if (s.glide || radar) std::tie(p.vx, p.vy) = velocity(*f, view);
    p.watch = watch_colour(*f, s.watch);
    p.label = label_of(*f, p.watch && kind == LABEL_NONE ? LABEL_CALLSIGN : kind, s.metric);
    p.id = f->hex;
    if (s.track_s > 0) {
      for (const auto &place : thin(tracks.of(f->hex, s.track_s, now), TRACK_POINTS)) {
        const geo::Pixel q = view.point(place.lat, place.lon);
        p.track.push_back((int16_t) std::lround(q.x));
        p.track.push_back((int16_t) std::lround(q.y));
      }
    }
    o.planes.push_back(std::move(p));
  }
  std::stable_partition(o.planes.begin(), o.planes.end(), [](const plan::Plane &p) { return p.watch == 0; });
  return o;
}
}  // namespace sky::overlay
