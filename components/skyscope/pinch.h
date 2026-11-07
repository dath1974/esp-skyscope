#pragma once
// Two fingers on the map (preview.cpp): spread or pinched they zoom, moved together they pan, as on a phone's map. The
// place under the fingers stays under them. Nothing is downloaded while they move: what the glass showed when they
// landed (the map and its planes) is drawn larger, smaller or shifted as the new view would show it (transform(),
// stretch()), and the new view is planned only once the fingers have been still for a while.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "geo.h"
#include "settings.h"

namespace sky::pinch {

struct Point {
  double x = 0, y = 0;
};

// The range a view shows, as the settings keep it: nautical miles from the middle to the nearer edge, whole, 1 to 250.
inline int range_nm(const geo::View &v) {
  const double nm = v.meters_per_px() * std::min(v.width, v.height) / 2.0 / geo::NM;
  return std::max(MIN_RANGE_NM, std::min(MAX_RANGE_NM, (int) std::lround(nm)));
}

// Two fingers, from where they first were both down on the view then shown.
struct Fingers {
  bool active = false;
  Point a0, b0;
  geo::View from;

  void start(Point a, Point b, const geo::View &v) {
    active = true;
    a0 = a;
    b0 = b;
    from = v;
  }
  // The view with the fingers at `a` and `b`: zoomed by how much farther apart they are (as far as the range goes:
  // 1 to 250 NM), and moved so the place under their middle at the start is under their middle now.
  geo::View moved(Point a, Point b) const {
    const double d0 = std::hypot(b0.x - a0.x, b0.y - a0.y), d = std::hypot(b.x - a.x, b.y - a.y);
    const int short_px = std::min(from.width, from.height);
    const double closest = geo::zoom_for_radius(from.lat, MIN_RANGE_NM * geo::NM, short_px);
    const double farthest = geo::zoom_for_radius(from.lat, MAX_RANGE_NM * geo::NM, short_px);
    double zoom = from.zoom + (d0 > 1 ? std::log2(std::max(d, 1.0) / d0) : 0);
    zoom = std::max({farthest, geo::MIN_ZOOM, std::min({closest, geo::MAX_ZOOM, zoom})});
    const double scale = std::pow(2.0, zoom - from.zoom);
    const Point m0{(a0.x + b0.x) / 2, (a0.y + b0.y) / 2}, m{(a.x + b.x) / 2, (a.y + b.y) / 2};
    // The world under the starting middle, at the new zoom, put under the middle now.
    geo::View v = from;
    v.zoom = zoom;
    v.left = (from.left + m0.x) * scale - m.x;
    v.top = (from.top + m0.y) * scale - m.y;
    geo::Place c = v.place(from.width / 2.0, from.height / 2.0);
    c.lat = std::max(-85.0, std::min(85.0, c.lat));  // as far as the settings go
    c.lon = std::remainder(c.lon, 360.0);
    return geo::View(c.lat, c.lon, zoom, from.width, from.height);
  }
};

// How a frame drawn for `drawn` is drawn again to show `target`: each of its pixels (x, y) at
// (x * scale + dx, y * scale + dy).
struct Transform {
  double scale = 1, dx = 0, dy = 0;
  bool identity() const { return std::fabs(scale - 1) < 1e-6 && std::fabs(dx) < 0.5 && std::fabs(dy) < 0.5; }
};
inline Transform transform(const geo::View &drawn, const geo::View &target) {
  Transform t;
  t.scale = std::pow(2.0, target.zoom - drawn.zoom);
  t.dx = drawn.left * t.scale - target.left;
  t.dy = drawn.top * t.scale - target.top;
  // The short way around the date line.
  const double size = geo::TILE_PX * std::pow(2.0, target.zoom);
  t.dx = std::remainder(t.dx, size);
  return t;
}

// The frame `from` drawn again into `to` as `t` shows it, each pixel the nearest of `from`'s, `ground` where `from` does
// not reach; both `w` by `h`. `columns` is kept between calls: each column's column of `from`.
inline void stretch(const uint16_t *from, uint16_t *to, int w, int h, const Transform &t, uint16_t ground,
                    std::vector<int> &columns) {
  columns.resize(w);
  for (int x = 0; x < w; ++x) {
    const double sx = std::floor((x + 0.5 - t.dx) / t.scale);
    columns[x] = sx >= 0 && sx < w ? (int) sx : -1;
  }
  for (int y = 0; y < h; ++y) {
    uint16_t *row = to + (size_t) y * w;
    const double sy = std::floor((y + 0.5 - t.dy) / t.scale);
    if (sy < 0 || sy >= h) {
      std::fill(row, row + w, ground);
      continue;
    }
    const uint16_t *source = from + (size_t) sy * w;
    for (int x = 0; x < w; ++x) row[x] = columns[x] >= 0 ? source[columns[x]] : ground;
  }
}
}  // namespace sky::pinch
