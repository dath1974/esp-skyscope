#pragma once
// Planes on the glass, and where each is drawn at a moment. This file only plans; view.cpp paints a plan in one LVGL
// object over the map, so thirty planes with their tracks and labels are one object.
//
// overlay.h places every plane on the frame (pixels, never a place) and says how far it flies in a minute, in pixels.
// Between two reports a plane that glides goes on along its heading for at most GLIDE_MS, as a flight map does; one that
// holds stands still until the next report.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sky::plan {
enum Marker : uint8_t { DOT, ARROW, PLANE };
// Bands of altitude (overlay.h BANDS): on the ground, below 5,000 ft, below 15,000, below 30,000, above.
// NO_BAND: the source gave no altitude.
constexpr int BANDS = 5, NO_BAND = -1, NO_HEADING = -1;
// How long a plane glides on after its report: a source that stopped reporting leaves its planes where the last report
// and this much flight put them.
constexpr uint32_t GLIDE_MS = 30000;
// Watched aircraft (settings.h WATCH_COLOURS): a plane's colour is 1 to WATCH_COLOURS, 0 for one nobody watches.
constexpr int WATCH_COLOURS = 6;
// A watched plane is drawn this much larger, with a ring around it.
constexpr float WATCH_SCALE = 1.35f;
// The most planes a frame shows, the watched ones beside them.
constexpr size_t MOST_PLANES = 72, MOST_TRACK = 24;

struct Plane {
  int x = 0, y = 0;            // on the frame, `width` x `height`
  int heading = NO_HEADING;    // degrees from north, clockwise
  int band = NO_BAND;
  int vx = 0, vy = 0;          // pixels a minute
  std::string label;           // its lines, joined by '\n'
  std::vector<int16_t> track;  // x, y, x, y ... oldest first, ending where the plane is
  int watch = 0;               // 1 to WATCH_COLOURS for a watched plane, in its colour
  std::string id;              // the transponder's address: what a finger picks it by
};

// A map drawn as a radar scope: its phosphor (colours.h TINTS), the middle of the scope on the frame, and whether its
// beam turns.
struct Radar {
  bool on = false;
  int tint = 0;
  int x = 0, y = 0;
  int rose = 0;  // the radius of its compass rose, the circle the beam turns in; 0: out to the farthest corner
  bool sweep = false;
};

struct Overlay {
  int width = 0, height = 0;  // the frame the planes are placed on
  Marker marker = DOT;
  bool by_altitude = true;  // coloured by band, else in the accent
  bool glide = true;
  bool light = false;       // the light appearance: the pills and rings in its palette (colours.h)
  uint32_t at = 0;          // millis() when it came
  std::vector<Plane> planes;
  Radar radar;
};

struct Point { float x, y; };

// How far `o`'s report is behind `now`, as far as a plane glides.
inline uint32_t since(const Overlay &o, uint32_t now) {
  if (!o.glide) return 0;
  return std::min<uint32_t>(now - o.at, GLIDE_MS);
}
// Where a plane is at `now`, on a frame shown at `sx`, `sy` of the one it was placed on.
inline Point at(const Overlay &o, const Plane &p, uint32_t now, float sx = 1, float sy = 1) {
  const float minutes = since(o, now) / 60000.0f;
  return {(p.x + p.vx * minutes) * sx, (p.y + p.vy * minutes) * sy};
}

// The triangles of an airliner seen from above, `size` pixels nose to tail, turned to `heading`: a nose cone, the
// fuselage, a tail cone, swept wings and a tailplane, each rooted in the fuselage so the outline is one solid piece
// even a few millimetres across. The first triangle is the nose cone, its tip first.
struct Triangle { std::array<Point, 3> p; };
constexpr size_t PLANE_TRIANGLES = 12;
inline std::array<Triangle, PLANE_TRIANGLES> plane_shape(Point c, float size, int heading) {
  const float a = (heading < 0 ? 0 : heading) * 3.14159265f / 180.0f, s = std::sin(a), k = std::cos(a);
  const float h = size / 2, f = 0.07f * size, r = 0.9f * f;  // half the fuselage's width; where the wings take root
  // In the plane's own frame (x right, y forward), then turned: forward is north at heading 0, up on the glass.
  auto turn = [&](float x, float y) { return Point{c.x + x * k + y * s, c.y + x * s - y * k}; };
  const float shoulder = 0.62f * h, tail_start = -0.72f * h;
  std::array<Triangle, PLANE_TRIANGLES> out{};
  size_t n = 0;
  auto quad = [&](Point p0, Point p1, Point p2, Point p3) {
    out[n++] = {{turn(p0.x, p0.y), turn(p1.x, p1.y), turn(p2.x, p2.y)}};
    out[n++] = {{turn(p0.x, p0.y), turn(p2.x, p2.y), turn(p3.x, p3.y)}};
  };
  out[n++] = {{turn(0, h), turn(-f, shoulder), turn(f, shoulder)}};                       // nose cone
  quad({-f, shoulder}, {f, shoulder}, {f, tail_start}, {-f, tail_start});                // fuselage
  out[n++] = {{turn(-f, tail_start), turn(f, tail_start), turn(0, -h)}};                 // tail cone
  for (const float side : {-1.0f, 1.0f}) {
    quad({side * r, 0.30f * h}, {side * 0.50f * size, -0.22f * h}, {side * 0.50f * size, -0.34f * h}, {side * r, -0.08f * h});  // wing
    quad({side * r, -0.60f * h}, {side * 0.19f * size, -0.86f * h}, {side * 0.19f * size, -0.96f * h}, {side * r, -0.80f * h});  // tailplane
  }
  return out;
}
// The short line an arrow marker points along: from the dot's edge out by `length`, toward `heading`.
inline std::array<Point, 2> heading_tick(Point c, float radius, float length, int heading) {
  const float a = heading * 3.14159265f / 180.0f, s = std::sin(a), k = -std::cos(a);
  return {{{c.x + s * radius, c.y + k * radius}, {c.x + s * (radius + length), c.y + k * (radius + length)}}};
}

// ---- A radar scope's planes
// Its phosphor by band of altitude, as a share of the way from the ground to the glow (up to 1) and on from the glow to
// its hottest light (over 1): low planes dim, high ones bright, all in one hue. NO_BAND is drawn as the ground's.
inline constexpr float BAND_LIGHT[BANDS] = {0.5f, 0.75f, 1.0f, 1.35f, 1.7f};
inline float band_light(int band) { return BAND_LIGHT[band >= 0 && band < BANDS ? band : 0]; }
// The beam of a scope that sweeps: once round in SWEEP_MS, clockwise from north, its afterglow a trail of
// SWEEP_TRAIL degrees behind it, drawn every SWEEP_FRAME_MS (on the clock, view.cpp's sweep: 18 steps a second
// measured on the AMOLED 2.41 standing up with a few planes, of the 20 asked for). A plane it passes lights up and fades over AFTERGLOW
// degrees of the turn. Every pixel of the trail changes each frame, so it is kept short: each is a see-through pixel to
// draw again (about a microsecond each in PSRAM on an ESP32-S3) and to send to the panel, every frame.
constexpr uint32_t SWEEP_MS = 4000, SWEEP_FRAME_MS = 50;
constexpr float SWEEP_TRAIL = 10.0f, AFTERGLOW = 360.0f;
// The beam's bearing at `now`, degrees from north, clockwise.
inline float beam(uint32_t now) { return (now % SWEEP_MS) * 360.0f / SWEEP_MS; }
// The bearing of `p` from `c` on the glass, degrees from north (up), clockwise.
inline float bearing(Point c, Point p) {
  const float a = std::atan2(p.x - c.x, c.y - p.y) * 180.0f / 3.14159265f;
  return a < 0 ? a + 360.0f : a;
}
// How many degrees the beam at `beam_at` has turned since it passed `at`: 0 just as it passes, up to 360.
inline float behind(float beam_at, float at) { return std::fmod(beam_at - at + 720.0f, 360.0f); }
// Whether the beam, turning from `was` to `now` (less than a turn), passed bearing `at`.
inline bool crossed(float was, float now, float at) { return was != now && behind(now, at) < behind(was, at); }
// How lit a plane is the beam passed `since` degrees ago: 1 as it passes, fading to 0 at AFTERGLOW.
inline float afterglow(float since) { return since >= AFTERGLOW ? 0.0f : 1.0f - since / AFTERGLOW; }

struct Box { int x, y, w, h; };
// The box round the wedge of a circle of radius `r` about `c`, from bearing `from` clockwise to bearing `to` (less than
// a turn): its middle, its two ends and every quarter of north, east, south and west it passes.
inline Box wedge_box(Point c, float r, float from, float to) {
  auto at = [&](float b) { const float a = b * 3.14159265f / 180.0f; return Point{c.x + std::sin(a) * r, c.y - std::cos(a) * r}; };
  float x0 = c.x, y0 = c.y, x1 = c.x, y1 = c.y;
  auto take = [&](Point p) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); };
  const float span = std::fmod(to - from + 720.0f, 360.0f);
  take(at(from));
  take(at(from + span));
  for (int q = 0; q < 4; ++q) if (behind(q * 90.0f, from) <= span) take(at(q * 90.0f));
  const int x = (int) std::floor(x0) - 2, y = (int) std::floor(y0) - 2;
  return {x, y, (int) std::ceil(x1) + 2 - x + 1, (int) std::ceil(y1) + 2 - y + 1};
}
inline bool overlaps(const Box &a, const Box &b);
inline Box join(const Box &a, const Box &b) {
  const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
  return {x0, y0, std::max(a.x + a.w, b.x + b.w) - x0, std::max(a.y + a.h, b.y + b.h) - y0};
}
inline bool holds(const Box &outer, const Box &inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}
inline Box moved(const Box &b, float dx, float dy) {
  return {b.x + (int) std::floor(dx), b.y + (int) std::floor(dy), b.w + 1, b.h + 1};
}
// A label's place beside its plane: to the right of the marker, its middle on the plane's height; to the left where the
// right has no room; and kept inside the frame.
inline Box label_box(Point c, int reach, int w, int h, int frame_w, int frame_h, int gap) {
  int x = (int) std::lround(c.x) + reach + gap;
  if (x + w > frame_w) x = (int) std::lround(c.x) - reach - gap - w;
  int y = (int) std::lround(c.y) - h / 2;
  x = std::max(0, std::min(x, frame_w - w));
  y = std::max(0, std::min(y, frame_h - h));
  return {x, y, w, h};
}

// Labels kept apart, as a radar display de-conflicts its data blocks: each label takes the first spot round its plane
// that is inside the frame and clear of every label placed before it and of every plane's marker, and one moved off its
// plane is drawn with a leader line back to it. The spots, in order: beside the plane (right, left), its corners, above
// and below, then the same eight directions on rings `step` further out (LABEL_RINGS of them). Watched planes are
// placed first, then the rest in the order given. A label with no clear spot is left out (`shown` false), as a pile of
// text helps nobody; a watched plane's takes the spot it overlaps
// least instead, so its label always shows. A label keeps the spot it had (`kept`) while that spot stays clear, so
// labels hold still while their planes glide.
constexpr int LABEL_DIRECTIONS = 8, LABEL_RINGS = 6, LABEL_SPOTS = LABEL_DIRECTIONS * (LABEL_RINGS + 1), NO_SPOT = -1;

struct Label {
  Point c;          // the plane, on the frame
  int reach = 0;    // from its middle to its marker's edge
  int w = 0, h = 0;
  bool watched = false;
};
struct Placed {
  Box box{0, 0, 0, 0};
  int spot = NO_SPOT;
  bool leader = false;
  bool shown = false;
};

inline bool overlaps(const Box &a, const Box &b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
inline long overlap_area(const Box &a, const Box &b) {
  const int w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x), h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
  return w > 0 && h > 0 ? (long) w * h : 0;
}
// Spot `spot` of a label: direction spot % 8 (right, left, up-right, down-right, up-left, down-left, up, down), its near
// edge or corner `reach + gap + ring * step` from the plane.
inline Box spot_box(const Label &l, int spot, int gap, int step) {
  const int ring = spot / LABEL_DIRECTIONS, dir = spot % LABEL_DIRECTIONS;
  const int cx = (int) std::lround(l.c.x), cy = (int) std::lround(l.c.y);
  const int d = l.reach + gap + ring * step, k = (int) std::lround(d * 0.7071f);
  switch (dir) {
    case 0: return {cx + d, cy - l.h / 2, l.w, l.h};
    case 1: return {cx - d - l.w, cy - l.h / 2, l.w, l.h};
    case 2: return {cx + k, cy - k - l.h, l.w, l.h};
    case 3: return {cx + k, cy + k, l.w, l.h};
    case 4: return {cx - k - l.w, cy - k - l.h, l.w, l.h};
    case 5: return {cx - k - l.w, cy + k, l.w, l.h};
    case 6: return {cx - l.w / 2, cy - d - l.h, l.w, l.h};
    default: return {cx - l.w / 2, cy + d, l.w, l.h};
  }
}

inline std::vector<Placed> place_labels(const std::vector<Label> &labels, int frame_w, int frame_h, int gap, int step,
                                        const std::vector<int> &kept = {}) {
  std::vector<Placed> out(labels.size());
  std::vector<Box> markers;
  markers.reserve(labels.size());
  for (const auto &l : labels) {
    const int cx = (int) std::lround(l.c.x), cy = (int) std::lround(l.c.y);
    markers.push_back({cx - l.reach, cy - l.reach, 2 * l.reach + 1, 2 * l.reach + 1});
  }
  std::vector<size_t> order;
  order.reserve(labels.size());
  for (size_t i = 0; i < labels.size(); ++i) if (labels[i].watched) order.push_back(i);
  for (size_t i = 0; i < labels.size(); ++i) if (!labels[i].watched) order.push_back(i);
  std::vector<Box> placed;
  placed.reserve(labels.size());
  for (const size_t i : order) {
    const Label &l = labels[i];
    if (l.w <= 0 || l.h <= 0) continue;
    auto inside = [&](const Box &b) { return b.x >= 0 && b.y >= 0 && b.x + b.w <= frame_w && b.y + b.h <= frame_h; };
    auto cost = [&](const Box &b) {
      long sum = 0;
      for (const auto &p : placed) sum += overlap_area(b, p);
      for (size_t m = 0; m < markers.size(); ++m) if (m != i) sum += overlap_area(b, markers[m]);
      return sum;
    };
    int best = NO_SPOT;
    long best_cost = -1;
    auto consider = [&](int spot) {
      const Box b = spot_box(l, spot, gap, step);
      if (!inside(b)) return false;
      const long c = cost(b);
      if (best == NO_SPOT || c < best_cost) { best = spot; best_cost = c; }
      return c == 0;
    };
    bool clear = i < kept.size() && kept[i] >= 0 && kept[i] < LABEL_SPOTS && consider(kept[i]);
    for (int spot = 0; !clear && spot < LABEL_SPOTS; ++spot) clear = consider(spot);
    Placed &p = out[i];
    if (!clear && !l.watched) continue;
    p.shown = true;
    if (best == NO_SPOT) {
      // Larger than every spot the frame has room for: beside its plane, kept inside the frame as before.
      p.box = label_box(l.c, l.reach, l.w, l.h, frame_w, frame_h, gap);
    } else {
      p.box = spot_box(l, best, gap, step);
      p.spot = best;
      p.leader = best >= LABEL_DIRECTIONS;
    }
    placed.push_back(p.box);
  }
  return out;
}
// A leader line from a plane's marker to its label moved away: from the marker's edge toward the nearest point of the
// box, ending on the box.
inline std::array<Point, 2> leader(const Label &l, const Box &b) {
  const Point near{std::max<float>(b.x, std::min<float>(l.c.x, b.x + b.w - 1)), std::max<float>(b.y, std::min<float>(l.c.y, b.y + b.h - 1))};
  const float dx = near.x - l.c.x, dy = near.y - l.c.y, d = std::sqrt(dx * dx + dy * dy);
  if (d <= l.reach) return {{near, near}};
  return {{{l.c.x + dx / d * l.reach, l.c.y + dy / d * l.reach}, near}};
}

// The lines of a label.
inline std::vector<std::string> lines(const std::string &label) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= label.size()) {
    const size_t end = label.find('\n', start);
    const std::string line = label.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!line.empty()) out.push_back(line);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

// The plane nearest `x`, `y` on a picture shown at `sx`, `sy` of the one the planes were placed on, within `reach`
// pixels of it, at `now`; "" for none.
inline std::string pick(const Overlay &o, float x, float y, float reach, uint32_t now, float sx = 1, float sy = 1) {
  std::string best;
  float nearest = reach * reach;
  for (const auto &p : o.planes) {
    if (p.id.empty()) continue;
    const Point c = at(o, p, now, sx, sy);
    const float d = (c.x - x) * (c.x - x) + (c.y - y) * (c.y - y);
    if (d <= nearest) { nearest = d; best = p.id; }
  }
  return best;
}
}  // namespace sky::plan
