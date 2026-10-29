#pragma once
// Airspace and runways from the FAA's ADDS (its Aeronautical Data Delivery Service, the same public ArcGIS servers as
// its charts): the outlines of the Class B, C and D airspace around the screen, and each runway as a line, those of
// the airports with a tower lengthened into dashed centerlines, as a controller's scope draws them. The screen asks
// once for each place and range and reads the reply here, with no network, so the tests run on a PC, and draws it
// here too, straight into the frame's pixels; worker.cpp fetches and scope.cpp chooses its colours. United States
// only, like the FAA's charts.
#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "feed.h"
#include "geo.h"
#include "pixels.h"

namespace sky::airspace {

enum Class : uint8_t { CLASS_B, CLASS_C, CLASS_D };
struct Point { float lat, lon; };
// One ring of an area (its outline, or a hole in it): the points [first, first + count) of the map's list.
struct Area {
  Class cls;
  uint32_t first, count;
};
struct Runway {
  Point a, b;  // the middles of its two ends
  geo::Pixel world_a, world_b;  // the same at zoom 0 (Map world)
  int length_ft = 0;
  bool towered = false;  // at an airport with Class B, C or D airspace: drawn with its centerline lengthened
};
// Every point in one list, so a map of a hundred areas is one block of PSRAM rather than a hundred small pieces of the
// internal RAM. Each is also kept in world pixels at zoom 0, worked out once as it is read: the chip has no hardware
// for doubles, and the sine and logarithm of each point at every frame took most of a second.
struct Map {
  std::vector<Point> points;
  std::vector<geo::Pixel> world;
  std::vector<Area> areas;
  std::vector<Runway> runways;
  bool cut = false;  // the server or the screen stopped before the end: some are missing
};

// Runways are asked for up to this range (beyond it a long runway is a few pixels), and only those this long: the
// shortest a jet uses, which leaves out grass strips and helipads.
constexpr int RUNWAY_RANGE_NM = 80;
constexpr int SHORTEST_RUNWAY_FT = 2500;
// A towered airport's runways this long have their centerlines drawn out this far from each end.
constexpr int CENTERLINE_RUNWAY_FT = 5000;
constexpr double CENTERLINE_NM = 10;
// The most points kept of a reply (8 bytes each), and the most bytes of a reply read.
constexpr size_t MOST_POINTS = 40000;
constexpr size_t MOST_REPLY_BYTES = 512 * 1024;

// ---- What to ask for

struct Box { double south, west, north, east; };
// The frame's corners as places, with `margin` of its size more on each side, so a plane at the edge still has the
// airspace it is in.
inline Box box_of(const geo::View &v, double margin = 0.1) {
  const double mx = v.width * margin, my = v.height * margin;
  const geo::Place nw = v.place(-mx, -my), se = v.place(v.width + mx, v.height + my);
  return {se.lat, nw.lon, nw.lat, se.lon};
}

#define SKYSCOPE_ADDS "https://services6.arcgis.com/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
// A query of a layer for the features in `b`, in latitude and longitude, of `where` (written for a URL) with `fields`.
// `tolerance` (degrees) lets the server leave out points a line would not move by on the glass: an outline of a
// thousand points comes as a few dozen.
inline std::string query(const char *layer, const char *where, const char *fields, const Box &b, double tolerance) {
  char out[512];
  int n = std::snprintf(out, sizeof(out),
                        SKYSCOPE_ADDS "%s/FeatureServer/0/query?where=%s&geometry=%.3f,%.3f,%.3f,%.3f"
                                      "&geometryType=esriGeometryEnvelope&inSR=4326&spatialRel=esriSpatialRelIntersects"
                                      "&outFields=%s&outSR=4326&geometryPrecision=5&f=json",
                        layer, where, b.west, b.south, b.east, b.north, fields);
  if (tolerance > 0 && n > 0 && (size_t) n < sizeof(out))
    std::snprintf(out + n, sizeof(out) - n, "&maxAllowableOffset=%.5f", tolerance);
  return out;
}
#undef SKYSCOPE_ADDS

// The two requests for a frame: its airspace, and its runways ("" beyond RUNWAY_RANGE_NM). Each frame asks the same
// way, so the address is also what the screen keeps a reply by.
struct Ask {
  std::string airspace, runways;
};
inline Ask ask(const geo::View &v, int range_nm) {
  const Box b = box_of(v);
  // Half a pixel, in degrees of latitude.
  const double tolerance = v.metres_per_px() * 0.5 / 111320.0;
  Ask out;
  out.airspace = query("Class_Airspace", "CLASS%20IN%20(%27B%27,%27C%27,%27D%27)", "CLASS,ADHP_ID", b, tolerance);
  if (range_nm <= RUNWAY_RANGE_NM) {
    const std::string where = "LENGTH%3E%3D" + std::to_string(SHORTEST_RUNWAY_FT);  // LENGTH>=
    out.runways = query("Runways", where.c_str(), "AIRPORT_ID,LENGTH", b, 0);
  }
  return out;
}

// ---- Reading a reply

namespace detail {
// Calls `each(name, value, value_end)` for each member of the JSON object at `p`; false when it is not one.
template<typename F> bool members(const char *p, const char *end, F each) {
  using namespace feed::scan;
  p = space(p, end);
  if (p >= end || *p != '{') return false;
  p = space(p + 1, end);
  while (p < end && *p != '}') {
    if (*p != '"') return false;
    const char *key = p + 1, *after = string(p, end);
    if (!after) return false;
    const std::string_view name(key, after - 1 - key);
    p = space(after, end);
    if (p >= end || *p != ':') return false;
    p = space(p + 1, end);
    const char *value_end = value(p, end);
    if (!value_end) return false;
    each(name, p, value_end);
    p = space(value_end, end);
    if (p < end && *p == ',') p = space(p + 1, end);
    else if (p < end && *p != '}') return false;
  }
  return p < end;
}
// Calls `each(value, value_end)` for each item of the JSON list at `p`, stopping at the first it returns false for;
// false when it is not a list or `each` said so.
template<typename F> bool items(const char *p, const char *end, F each) {
  using namespace feed::scan;
  p = space(p, end);
  if (p >= end || *p != '[') return false;
  p = space(p + 1, end);
  while (p < end && *p != ']') {
    const char *item_end = value(p, end);
    if (!item_end || !each(p, item_end)) return false;
    p = space(item_end, end);
    if (p < end && *p == ',') p = space(p + 1, end);
    else if (p < end && *p != ']') return false;
  }
  return p < end;
}
// The rings of a polygon, [[[lon, lat], ...], ...], each to `ring(points)`; false when it is not that.
template<typename F> bool rings(const char *p, const char *end, std::vector<Point> &scratch, F ring) {
  return items(p, end, [&](const char *rp, const char *re) {
    scratch.clear();
    const bool ok = items(rp, re, [&](const char *pp, const char *pe) {
      double xy[2];
      int n = 0;
      const bool numbers = items(pp, pe, [&](const char *np, const char *ne) {
        char *stop = nullptr;
        const double d = std::strtod(np, &stop);
        if (stop != ne || !std::isfinite(d)) return false;
        if (n < 2) xy[n] = d;
        ++n;  // a third (a height) is passed over
        return true;
      });
      if (!numbers || n < 2 || std::fabs(xy[0]) > 180 || std::fabs(xy[1]) > 90) return false;
      scratch.push_back({(float) xy[1], (float) xy[0]});
      return true;
    });
    if (ok) ring(scratch);
    return ok;
  });
}
// An airport's id (a GUID, the same in both layers) kept as a hash: a hundred of them as numbers, not strings.
inline uint32_t id_hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) h = (h ^ (uint8_t) *s) * 16777619u;
  return h;
}
// The features of a reply, each one's attributes read through `keep` into `doc` and handed with its rings to
// `each(attributes, rings, rings_end)`, which says whether they could be read. False with `error` set when it is no
// reply of features: the server's own word where it sent one (ArcGIS answers a bad query with 200 and an `error`).
template<typename F>
bool features(const char *text, size_t length, JsonDocument &doc, const JsonDocument &keep, Map &out,
              std::string &error, F each) {
  const char *end = text + length, *list = nullptr, *list_end = nullptr, *problem = nullptr, *problem_end = nullptr;
  const bool object = members(text, end, [&](std::string_view name, const char *v, const char *ve) {
    if (name == "features") list = v, list_end = ve;
    else if (name == "error") problem = v, problem_end = ve;
    else if (name == "exceededTransferLimit" && std::string_view(v, ve - v) == "true") out.cut = true;
  });
  if (!object) {
    error = "JSON: InvalidInput";
    return false;
  }
  if (problem) {
    deserializeJson(doc, problem, problem_end - problem);
    error = std::string("ADDS: ") + (doc["message"] | "error");
    doc.clear();
    return false;
  }
  if (!list) {
    error = "ADDS: no features";
    return false;
  }
  const bool ok = items(list, list_end, [&](const char *fp, const char *fe) {
    const char *attributes = nullptr, *attributes_end = nullptr, *geometry = nullptr, *geometry_end = nullptr;
    if (!members(fp, fe, [&](std::string_view name, const char *v, const char *ve) {
          if (name == "attributes") attributes = v, attributes_end = ve;
          else if (name == "geometry") geometry = v, geometry_end = ve;
        }))
      return false;
    if (!attributes || !geometry) return true;  // a feature without a shape is passed over
    if (deserializeJson(doc, attributes, attributes_end - attributes, DeserializationOption::Filter(keep)))
      return false;
    const char *shape = nullptr, *shape_end = nullptr;
    members(geometry, geometry_end, [&](std::string_view name, const char *v, const char *ve) {
      if (name == "rings") shape = v, shape_end = ve;
    });
    return !shape || each(doc.as<JsonObjectConst>(), shape, shape_end);
  });
  doc.clear();
  if (!ok) error = "JSON: InvalidInput";
  return ok;
}
}  // namespace detail

// Reads a reply of Class_Airspace into `out`'s areas, and the airports they belong to into `airports` (sorted), for
// read_runways. False with `error` set when it is not one; what came before a broken feature is kept.
inline bool read_airspace(const char *text, size_t length, JsonDocument &doc, Map &out, std::vector<uint32_t> &airports,
                          std::string &error) {
  static const JsonDocument keep = [] {
    JsonDocument f;
    f["CLASS"] = true;
    f["ADHP_ID"] = true;
    return f;
  }();
  std::vector<Point> scratch;
  auto area = [&](JsonObjectConst a, const char *g, const char *ge) {
    const char *name = a["CLASS"] | "";
    const Class cls = name[0] == 'B' ? CLASS_B : name[0] == 'C' ? CLASS_C : CLASS_D;
    if (name[0] < 'B' || name[0] > 'D' || name[1]) return true;
    if (const char *id = a["ADHP_ID"]) airports.push_back(detail::id_hash(id));
    return detail::rings(g, ge, scratch, [&](const std::vector<Point> &ring) {
      if (ring.size() < 2) return;
      if (out.points.size() + ring.size() > MOST_POINTS) {
        out.cut = true;
        return;
      }
      out.areas.push_back({cls, (uint32_t) out.points.size(), (uint32_t) ring.size()});
      out.points.insert(out.points.end(), ring.begin(), ring.end());
      for (const Point &q : ring) out.world.push_back(geo::world(q.lat, q.lon, 0));
    });
  };
  const bool ok = detail::features(text, length, doc, keep, out, error, area);
  std::sort(airports.begin(), airports.end());
  airports.erase(std::unique(airports.begin(), airports.end()), airports.end());
  return ok;
}

// A runway's outline (a rectangle, its corner repeated) as the line down its middle: along its longest side, from end
// to end.
inline bool centerline(const std::vector<Point> &ring, Runway &out) {
  if (ring.size() < 3) return false;
  const double lat0 = ring[0].lat, lon0 = ring[0].lon, k = std::cos(geo::radians(lat0));
  // Degrees of latitude both ways, so a step east is as long as a step north.
  auto x = [&](const Point &p) { return (p.lon - lon0) * k; };
  auto y = [&](const Point &p) { return (double) p.lat - lat0; };
  double ax = 0, ay = 0, longest = 0, cx = 0, cy = 0;
  for (size_t i = 0; i + 1 < ring.size(); ++i) {
    const double dx = x(ring[i + 1]) - x(ring[i]), dy = y(ring[i + 1]) - y(ring[i]), d = std::hypot(dx, dy);
    if (d > longest) longest = d, ax = dx / d, ay = dy / d;
    cx += x(ring[i]);
    cy += y(ring[i]);
  }
  if (longest <= 0) return false;
  const size_t corners = ring.size() - 1;
  cx /= corners;
  cy /= corners;
  double lo = 0, hi = 0;
  for (const Point &p : ring) {
    const double t = (x(p) - cx) * ax + (y(p) - cy) * ay;
    lo = std::min(lo, t);
    hi = std::max(hi, t);
  }
  auto place = [&](double t) { return Point{(float) (lat0 + cy + ay * t), (float) (lon0 + (cx + ax * t) / k)}; };
  out.a = place(lo);
  out.b = place(hi);
  return true;
}

// Reads a reply of Runways into `out`'s runways, those of `airports` (read_airspace's) marked towered.
inline bool read_runways(const char *text, size_t length, JsonDocument &doc, const std::vector<uint32_t> &airports,
                         Map &out, std::string &error) {
  static const JsonDocument keep = [] {
    JsonDocument f;
    f["AIRPORT_ID"] = true;
    f["LENGTH"] = true;
    return f;
  }();
  std::vector<Point> scratch;
  return detail::features(text, length, doc, keep, out, error, [&](JsonObjectConst a, const char *g, const char *ge) {
    Runway r;
    r.length_ft = a["LENGTH"] | 0;
    const char *id = a["AIRPORT_ID"];
    r.towered = id && std::binary_search(airports.begin(), airports.end(), detail::id_hash(id));
    bool first = true;
    return detail::rings(g, ge, scratch, [&](const std::vector<Point> &ring) {
      if (first && centerline(ring, r)) {
        r.world_a = geo::world(r.a.lat, r.a.lon, 0);
        r.world_b = geo::world(r.b.lat, r.b.lon, 0);
        out.runways.push_back(r);
      }
      first = false;
    });
  });
}

// Whether a runway's centerline is drawn out past its ends.
inline bool lengthened(const Runway &r) { return r.towered && r.length_ft >= CENTERLINE_RUNWAY_FT; }

// ---- Drawing it

// Its colours (RGB565) and sizes on the glass (pixels).
struct Ink {
  uint16_t b, c, d, runway, centerline;
  float thin, wide;  // Class C and D, centerlines; Class B, runways
  float dash, gap;   // Class D's
};

namespace detail {
// The dashes of a line from a to b, `dash` pixels with `gap` between, each to `on(from, to)`. `phase` is how far along
// the pattern it starts, and is carried on, so an outline drawn piece by piece keeps one pattern round its corners.
template<typename F> void dashes(geo::Pixel a, geo::Pixel b, double dash, double gap, double &phase, F on) {
  const double length = std::hypot(b.x - a.x, b.y - a.y), period = dash + gap;
  if (length <= 0 || period <= 0) return;
  auto along = [&](double t) { return geo::Pixel{a.x + (b.x - a.x) * t / length, a.y + (b.y - a.y) * t / length}; };
  for (double t = 0; t < length;) {
    const double in = std::fmod(phase, period);
    // At least a thousandth of a pixel on: `in` a hair short of a dash's end must not leave t where it is for ever.
    const double next = std::min(length, t + std::max(1e-3, in < dash ? dash - in : period - in));
    if (in < dash) on(along(t), along(next));
    phase = std::fmod(phase + next - t, period);
    t = next;
  }
}
}  // namespace detail

// `map` into the frame `v` (RGB565, v.width x v.height): Class D under C under B, the larger over the smaller as the
// charts lay them; the centerlines of a towered airport's long runways drawn out from each end in dashes a mile long
// (or `ink.dash`, far out); every runway over them.
inline void draw(uint16_t *frame, const geo::View &v, const Map &map, const Ink &ink) {
  const int w = v.width, h = v.height;
  // From zoom 0 to the frame: a multiplication and a subtraction (the short way round the date line is left out: the
  // FAA's airspace is nowhere near it).
  const double scale = std::exp2(v.zoom);
  auto at = [&](const geo::Pixel &p) { return geo::Pixel{p.x * scale - v.left, p.y * scale - v.top}; };
  auto stroke = [&](geo::Pixel a, geo::Pixel b, uint16_t colour, float width) {
    pixels::line(frame, w, h, a.x, a.y, b.x, b.y, colour, width);
  };
  // A piece that misses the frame is only measured, for the dashes after it.
  auto off = [&](geo::Pixel a, geo::Pixel b) {
    const double m = ink.wide + 2;
    return (a.x < -m && b.x < -m) || (a.y < -m && b.y < -m) || (a.x > w + m && b.x > w + m) ||
           (a.y > h + m && b.y > h + m);
  };
  for (const Class cls : {CLASS_D, CLASS_C, CLASS_B}) {
    const uint16_t colour = cls == CLASS_B ? ink.b : cls == CLASS_C ? ink.c : ink.d;
    const float width = cls == CLASS_B ? ink.wide : ink.thin;
    for (const Area &area : map.areas) {
      if (area.cls != cls) continue;
      const geo::Pixel *p = &map.world[area.first];
      geo::Pixel from = at(p[0]);
      double phase = 0;
      for (uint32_t i = 1; i < area.count; ++i) {
        const geo::Pixel to = at(p[i]);
        if (off(from, to)) phase = std::fmod(phase + std::hypot(to.x - from.x, to.y - from.y), ink.dash + ink.gap);
        else if (cls != CLASS_D) stroke(from, to, colour, width);
        else detail::dashes(from, to, ink.dash, ink.gap, phase, [&](geo::Pixel a, geo::Pixel b) {
          stroke(a, b, colour, width);
        });
        from = to;
      }
    }
  }
  const double px_per_nm = geo::NM / v.metres_per_px(), step = std::max<double>(ink.dash, px_per_nm);
  for (const Runway &r : map.runways) {
    if (!lengthened(r)) continue;
    const geo::Pixel a = at(r.world_a), b = at(r.world_b);
    const double length = std::hypot(b.x - a.x, b.y - a.y), out = CENTERLINE_NM * px_per_nm;
    if (length <= 0) continue;
    const double ux = (b.x - a.x) / length, uy = (b.y - a.y) / length;
    for (const auto &[end, sign] : {std::pair<geo::Pixel, double>{a, -1}, {b, 1}}) {
      const geo::Pixel far{end.x + sign * ux * out, end.y + sign * uy * out};
      if (off(end, far)) continue;
      double phase = 0;
      detail::dashes(end, far, step, step, phase, [&](geo::Pixel p, geo::Pixel q) {
        stroke(p, q, ink.centerline, ink.thin);
      });
    }
  }
  for (const Runway &r : map.runways) stroke(at(r.world_a), at(r.world_b), ink.runway, ink.wide);
}
}  // namespace sky::airspace
