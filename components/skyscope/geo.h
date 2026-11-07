#pragma once
// Where things are on the glass: Web Mercator pixels, 256 to a tile at zoom 0, as every web map counts them. A View is
// a middle, a zoom that need not be whole and the size of the frame; everything drawn (map tiles, planes, the scope)
// goes through the same View, so it all lines up.
#include <algorithm>
#include <cmath>

namespace sky::geo {
constexpr double TILE_PX = 256.0;
constexpr double MAX_LATITUDE = 85.05112878;
constexpr double EARTH_METERS = 6371000.0;
constexpr double NM = 1852.0;
constexpr double PI = 3.14159265358979323846;
// Zoom 1 is the world twice as wide as a tile: far enough out for MAX_RANGE_NM on any glass, wherever it is.
constexpr double MIN_ZOOM = 1.0, MAX_ZOOM = 18.0;

inline double radians(double d) { return d * PI / 180.0; }
inline double degrees(double r) { return r * 180.0 / PI; }

struct Pixel { double x, y; };
struct Place { double lat, lon; };

// World pixels of a place at `zoom`.
inline Pixel world(double lat, double lon, double zoom) {
  lat = std::max(-MAX_LATITUDE, std::min(MAX_LATITUDE, lat));
  const double size = TILE_PX * std::pow(2.0, zoom);
  const double s = std::sin(radians(lat));
  return {(lon + 180.0) / 360.0 * size, (0.5 - std::log((1 + s) / (1 - s)) / (4 * PI)) * size};
}

inline double meters_between(double lat1, double lon1, double lat2, double lon2) {
  const double p1 = radians(lat1), p2 = radians(lat2), dp = p2 - p1, dl = radians(lon2 - lon1);
  const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
  return 2 * EARTH_METERS * std::asin(std::min(1.0, std::sqrt(a)));
}

// The place `meters` from (lat, lon) setting off on `bearing` (degrees from true north) along a great circle, as a long
// flight goes; its longitude kept within -180 to 180.
inline Place destination(double lat, double lon, double bearing, double meters) {
  const double d = meters / EARTH_METERS, b = radians(bearing), p1 = radians(lat);
  const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
  const double l2 = radians(lon) + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1),
                                              std::cos(d) - std::sin(p1) * std::sin(p2));
  return {degrees(p2), std::remainder(degrees(l2), 360.0)};
}

// The place `f` of the way (0 to 1) along the great circle from one place to another.
inline Place between(double lat1, double lon1, double lat2, double lon2, double f) {
  const double p1 = radians(lat1), l1 = radians(lon1), p2 = radians(lat2), l2 = radians(lon2);
  const double d = meters_between(lat1, lon1, lat2, lon2) / EARTH_METERS;
  if (d < 1e-9) return {lat1, lon1};
  const double a = std::sin((1 - f) * d) / std::sin(d), b = std::sin(f * d) / std::sin(d);
  const double x = a * std::cos(p1) * std::cos(l1) + b * std::cos(p2) * std::cos(l2);
  const double y = a * std::cos(p1) * std::sin(l1) + b * std::cos(p2) * std::sin(l2);
  const double z = a * std::sin(p1) + b * std::sin(p2);
  return {degrees(std::atan2(z, std::hypot(x, y))), degrees(std::atan2(y, x))};
}

inline double meters_per_pixel(double lat, double zoom) {
  return 2 * PI * EARTH_METERS * std::cos(radians(lat)) / (TILE_PX * std::pow(2.0, zoom));
}

// The zoom at which `meters` either side of the middle fill the frame's shorter side, `short_px` pixels.
inline double zoom_for_radius(double lat, double meters, int short_px) {
  const double wanted = 2 * meters / std::max(1, short_px);  // meters per pixel
  return std::log2(2 * PI * EARTH_METERS * std::cos(radians(lat)) / (TILE_PX * wanted));
}

struct View {
  double lat = 0, lon = 0, zoom = 10;
  int width = 0, height = 0;
  double left = 0, top = 0;  // world pixels of the frame's top-left corner
  bool round = false;        // the frame is a circle as wide as it (glass.h): its corners are not on the glass

  View() = default;
  View(double lat_, double lon_, double zoom_, int w, int h) : lat(lat_), lon(lon_), width(w), height(h) {
    zoom = std::max(MIN_ZOOM, std::min(MAX_ZOOM, zoom_));
    const Pixel c = world(lat, lon, zoom);
    left = c.x - width / 2.0;
    top = c.y - height / 2.0;
  }
  Pixel point(double plat, double plon) const {
    const Pixel p = world(plat, plon, zoom);
    double x = p.x - left;
    // The short way around the date line.
    const double size = TILE_PX * std::pow(2.0, zoom);
    if (x < -size / 2) x += size;
    if (x > size / 2) x -= size;
    return {x, p.y - top};
  }
  // The place at (x, y) on the frame: point() the other way.
  Place place(double x, double y) const {
    const double size = TILE_PX * std::pow(2.0, zoom);
    const double wx = left + x, wy = top + y;
    return {degrees(std::atan(std::sinh(PI * (1 - 2 * wy / size)))), wx / size * 360.0 - 180.0};
  }
  double meters_per_px() const { return meters_per_pixel(lat, zoom); }
  // The farthest corner from the middle, in meters: how far a feed must be asked to cover the frame.
  double reach_meters() const { return std::hypot(width / 2.0, height / 2.0) * meters_per_px(); }
  bool operator==(const View &o) const {
    return lat == o.lat && lon == o.lon && zoom == o.zoom && width == o.width && height == o.height;
  }
  bool operator!=(const View &o) const { return !(*this == o); }
};

// Where tile (tz, tx, ty) starts on the frame, and how many frame pixels one tile pixel is.
struct TileOrigin { double x, y, scale; };
inline TileOrigin tile_origin(const View &v, int tz, int tx, int ty) {
  const double scale = std::pow(2.0, v.zoom - tz), span = TILE_PX * scale;
  const int count = 1 << tz;
  double column = tx;
  // A tile that wraps around the date line is drawn where the frame needs it.
  while (column * span + span < v.left) column += count;
  while (column * span > v.left + v.width) column -= count;
  return {column * span - v.left, ty * span - v.top, scale};
}
}  // namespace sky::geo
