#pragma once
// What each kind of aircraft is drawn as, and which kind a plane is. A kind is an outline seen from above in its own
// frame (x to the right wing, y forward, the nose at y = 0.5 and the tail at -0.5, the span within -0.5 ... 0.5), made
// of convex parts, each with its ink: a helicopter's rotor is a faint disc under a solid body. sprites.h rasterises an
// outline at each size and heading once; the screen only stamps it.
//
// A plane's kind comes from its type (the ICAO designator, `t`) where the table below knows it, else from the class its
// transponder sends (`category`, A1 ... C3), else from words in its description, else it is drawn as an airliner, as
// every plane was before there were kinds.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace sky::shapes {
enum Kind : uint8_t {
  AIRLINER,   // a narrow-body jet: the plane every plane was
  HEAVY,      // a wide-body twin (and the three-engined ones)
  QUAD,       // four engines (747, A380, A340, C-17, KC-135)
  JET,        // a business or regional jet: engines at the tail, a T-tail
  TURBOPROP,  // straight wings, an engine on each
  LIGHT,      // a light aircraft: a high straight wing and a propeller (and ultralights)
  HELICOPTER,
  FAST,       // high performance, military: a delta
  GLIDER,
  BALLOON,    // never turned: it has no heading to speak of
  AIRSHIP,
  DRONE,      // four rotors on an X
  VEHICLE,    // on the ground: a car seen from above
  KINDS
};
inline constexpr const char *NAMES[KINDS] = {"airliner", "heavy",  "quad",    "jet",     "turboprop", "light",  "helicopter",
                                             "fast",     "glider", "balloon", "airship", "drone",     "vehicle"};
// How large each kind is drawn, as a share of the plane size: a heavy stands out from a Cessna.
inline constexpr float SCALE[KINDS] = {1.0f, 1.2f, 1.25f, 0.85f, 0.9f, 0.8f, 0.85f, 0.85f, 0.95f, 0.75f, 0.95f, 0.7f, 0.55f};
// Whether a kind turns to its heading.
inline constexpr bool TURNS[KINDS] = {true, true, true, true, true, true, true, true, true, false, true, true, true};

struct Point { float x, y; };
constexpr int MOST_CORNERS = 16;
struct Part {
  uint8_t corners;
  uint8_t ink;  // 255 solid
  std::array<Point, MOST_CORNERS> p;
};

namespace detail {
inline Part poly(std::initializer_list<Point> pts, uint8_t ink = 255) {
  Part part{(uint8_t) pts.size(), ink, {}};
  int i = 0;
  for (const Point &q : pts) part.p[i++] = q;
  return part;
}
// A mirror image of a part across the fuselage (its corners reversed, so it winds the same way).
inline Part mirror(const Part &a) {
  Part b = a;
  for (int i = 0; i < a.corners; ++i) b.p[i] = {-a.p[a.corners - 1 - i].x, a.p[a.corners - 1 - i].y};
  return b;
}
// An ellipse of `n` corners round `c`, `rx` across and `ry` along.
inline Part ellipse(Point c, float rx, float ry, uint8_t ink = 255, int n = MOST_CORNERS) {
  Part part{(uint8_t) n, ink, {}};
  for (int i = 0; i < n; ++i) {
    const float a = 6.2831853f * i / n;
    part.p[i] = {c.x + rx * std::cos(a), c.y + ry * std::sin(a)};
  }
  return part;
}
inline Part rect(float x0, float y0, float x1, float y1, uint8_t ink = 255) {
  return poly({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, ink);
}
// A bar `w` wide from `a` to `b`.
inline Part bar(Point a, Point b, float w, uint8_t ink = 255) {
  const float dx = b.x - a.x, dy = b.y - a.y, n = std::sqrt(dx * dx + dy * dy), ox = -dy / n * w / 2, oy = dx / n * w / 2;
  return poly({{a.x + ox, a.y + oy}, {b.x + ox, b.y + oy}, {b.x - ox, b.y - oy}, {a.x - ox, a.y - oy}}, ink);
}
}  // namespace detail

// The parts of a kind, at most 16.
struct Outline {
  int count = 0;
  std::array<Part, 16> parts{};
  void add(const Part &p) { parts[count++] = p; }
  void pair(const Part &p) {  // a part and its mirror image
    add(p);
    add(detail::mirror(p));
  }
};

// A fuselage `w` across from `nose` to `tail`, its nose and tail pointed: the spine every winged kind is built on.
inline void fuselage(Outline &o, float w, float shoulder, float tail_start, float nose = 0.5f, float tail = -0.5f) {
  using detail::poly;
  o.add(poly({{0, nose}, {-w, shoulder}, {w, shoulder}}));
  o.add(detail::rect(-w, tail_start, w, shoulder));
  o.add(poly({{-w, tail_start}, {0, tail}, {w, tail_start}}));
}

inline Outline outline(Kind k) {
  using detail::ellipse;
  using detail::poly;
  using detail::rect;
  Outline o;
  switch (k) {
    case AIRLINER:
    default: {
      // As plan.h drew it: swept wings and tailplane rooted in a narrow fuselage.
      const float f = 0.07f, r = 0.9f * f;
      fuselage(o, f, 0.31f, -0.36f);
      o.pair(poly({{r, 0.15f}, {r, -0.04f}, {0.5f, -0.17f}, {0.5f, -0.11f}}));
      o.pair(poly({{r, -0.30f}, {r, -0.40f}, {0.19f, -0.48f}, {0.19f, -0.43f}}));
      break;
    }
    case HEAVY:
    case QUAD: {
      // Wider: a broad fuselage, long swept wings of deep chord, an engine under each (two under each for a quad).
      const float f = 0.085f, r = 0.9f * f;
      fuselage(o, f, 0.32f, -0.34f);
      o.pair(poly({{r, 0.17f}, {r, -0.06f}, {0.5f, -0.2f}, {0.5f, -0.13f}}));
      o.pair(poly({{r, -0.30f}, {r, -0.41f}, {0.21f, -0.49f}, {0.21f, -0.43f}}));
      if (k == HEAVY) {
        o.pair(rect(0.17f, 0.04f, 0.23f, 0.15f));
      } else {
        o.pair(rect(0.15f, 0.06f, 0.20f, 0.16f));
        o.pair(rect(0.30f, -0.01f, 0.35f, 0.09f));
      }
      break;
    }
    case JET: {
      // Slender, wings set back, two engines beside the tail, a small T-tail.
      const float f = 0.06f, r = 0.9f * f;
      fuselage(o, f, 0.33f, -0.38f);
      o.pair(poly({{r, 0.06f}, {r, -0.10f}, {0.45f, -0.20f}, {0.45f, -0.15f}}));
      o.pair(rect(0.06f, -0.34f, 0.12f, -0.18f));
      o.pair(poly({{r, -0.40f}, {r, -0.46f}, {0.17f, -0.50f}, {0.17f, -0.46f}}));
      break;
    }
    case TURBOPROP: {
      // Straight wings across the middle tapering to their tips, a big nacelle on each standing out ahead.
      const float f = 0.075f;
      fuselage(o, f, 0.36f, -0.34f);
      o.pair(poly({{0.0f, 0.17f}, {0.0f, 0.0f}, {0.5f, 0.04f}, {0.5f, 0.11f}}));
      o.pair(poly({{0.12f, 0.31f}, {0.12f, -0.03f}, {0.25f, -0.03f}, {0.25f, 0.31f}}));
      o.pair(poly({{0.0f, -0.36f}, {0.0f, -0.48f}, {0.21f, -0.47f}, {0.21f, -0.41f}}));
      break;
    }
    case LIGHT: {
      // A stubby body with its propeller across a blunt nose, a deep straight wing well forward, a broad tailplane.
      const float f = 0.09f;
      o.add(rect(-f, -0.22f, f, 0.40f));
      o.add(poly({{-f, -0.22f}, {-0.035f, -0.44f}, {0.035f, -0.44f}, {f, -0.22f}}));
      o.add(rect(-0.18f, 0.43f, 0.18f, 0.49f));  // the propeller
      o.add(rect(-0.04f, 0.40f, 0.04f, 0.43f));
      o.pair(rect(0.0f, 0.06f, 0.47f, 0.26f));
      o.pair(rect(0.0f, -0.50f, 0.2f, -0.38f));
      break;
    }
    case HELICOPTER: {
      // The rotor's disc, faint; the cabin, its boom back to a tail rotor, all solid over it.
      o.add(ellipse({0, 0.1f}, 0.4f, 0.4f, 110));
      o.add(ellipse({0, 0.12f}, 0.13f, 0.2f));
      o.add(rect(-0.03f, -0.44f, 0.03f, -0.02f));
      o.add(rect(-0.12f, -0.47f, 0.12f, -0.42f));
      break;
    }
    case FAST: {
      // A delta from behind the nose to the tail, a long pointed nose, twin fins.
      fuselage(o, 0.06f, 0.2f, -0.42f);
      o.pair(poly({{0.04f, 0.18f}, {0.04f, -0.36f}, {0.42f, -0.36f}}));
      o.pair(poly({{0.04f, -0.36f}, {0.04f, -0.48f}, {0.18f, -0.48f}, {0.18f, -0.42f}}));
      break;
    }
    case GLIDER: {
      // A wing as wide as the glider is long and very narrow, a thin fuselage, a small tailplane.
      const float f = 0.05f;
      fuselage(o, f, 0.3f, -0.36f);
      o.pair(poly({{0.0f, 0.2f}, {0.0f, 0.07f}, {0.5f, 0.1f}, {0.5f, 0.15f}}));
      o.pair(rect(0.0f, -0.49f, 0.15f, -0.42f));
      break;
    }
    case BALLOON: {
      // The envelope, and its basket below.
      o.add(ellipse({0, 0.08f}, 0.36f, 0.4f));
      o.add(poly({{-0.07f, -0.3f}, {0.07f, -0.3f}, {0.04f, -0.36f}, {-0.04f, -0.36f}}));
      o.add(rect(-0.07f, -0.5f, 0.07f, -0.38f));
      break;
    }
    case AIRSHIP: {
      // A long envelope, four fins at its tail.
      o.add(ellipse({0, 0.06f}, 0.16f, 0.44f));
      o.pair(poly({{0.1f, -0.26f}, {0.1f, -0.4f}, {0.24f, -0.48f}, {0.24f, -0.36f}}));
      break;
    }
    case DRONE: {
      // Four rotors on the arms of an X round a small body.
      for (const float sx : {-1.0f, 1.0f})
        for (const float sy : {-1.0f, 1.0f}) {
          o.add(ellipse({sx * 0.33f, sy * 0.33f}, 0.17f, 0.17f, 150, 12));
          o.add(detail::bar({0, 0}, {sx * 0.33f, sy * 0.33f}, 0.07f));
        }
      o.add(rect(-0.1f, -0.12f, 0.1f, 0.14f));
      o.add(poly({{-0.05f, 0.14f}, {0.05f, 0.14f}, {0, 0.22f}}));  // which way is forward
      break;
    }
    case VEHICLE: {
      // A car from above: its body, the windscreen left out as a gap.
      o.add(rect(-0.24f, 0.18f, 0.24f, 0.5f));
      o.add(rect(-0.24f, -0.5f, 0.24f, 0.08f));
      break;
    }
  }
  return o;
}

// ---- Which kind a plane is

// Types by their ICAO designator: the common ones of each kind, enough to cover most of what is in the air. A type not
// here is known by its transponder's class instead.
struct Type {
  const char *code;
  Kind kind;
};
// clang-format off
inline constexpr Type TYPES[] = {
  // Narrow-body airliners
  {"A318", AIRLINER}, {"A319", AIRLINER}, {"A320", AIRLINER}, {"A321", AIRLINER}, {"A19N", AIRLINER}, {"A20N", AIRLINER},
  {"A21N", AIRLINER}, {"B712", AIRLINER}, {"B733", AIRLINER}, {"B734", AIRLINER}, {"B735", AIRLINER}, {"B736", AIRLINER},
  {"B737", AIRLINER}, {"B738", AIRLINER}, {"B739", AIRLINER}, {"B37M", AIRLINER}, {"B38M", AIRLINER}, {"B39M", AIRLINER},
  {"B3XM", AIRLINER}, {"B752", AIRLINER}, {"B753", AIRLINER}, {"BCS1", AIRLINER}, {"BCS3", AIRLINER}, {"E170", AIRLINER},
  {"E75L", AIRLINER}, {"E75S", AIRLINER}, {"E190", AIRLINER}, {"E195", AIRLINER}, {"E290", AIRLINER}, {"E295", AIRLINER},
  {"MD82", AIRLINER}, {"MD83", AIRLINER}, {"MD88", AIRLINER}, {"MD90", AIRLINER}, {"C919", AIRLINER}, {"SU95", AIRLINER},
  {"A221", AIRLINER}, {"A223", AIRLINER}, {"B722", AIRLINER},
  // Wide-body twins, and the three-engined
  {"A306", HEAVY}, {"A30B", HEAVY}, {"A310", HEAVY}, {"A332", HEAVY}, {"A333", HEAVY}, {"A338", HEAVY}, {"A339", HEAVY},
  {"A359", HEAVY}, {"A35K", HEAVY}, {"B762", HEAVY}, {"B763", HEAVY}, {"B764", HEAVY}, {"B772", HEAVY}, {"B773", HEAVY},
  {"B77L", HEAVY}, {"B77W", HEAVY}, {"B778", HEAVY}, {"B779", HEAVY}, {"B788", HEAVY}, {"B789", HEAVY}, {"B78X", HEAVY},
  {"MD11", HEAVY}, {"DC10", HEAVY}, {"L101", HEAVY}, {"A3ST", HEAVY}, {"BLCF", QUAD}, {"KC46", HEAVY}, {"K35R", QUAD},
  // Four engines
  {"A342", QUAD}, {"A343", QUAD}, {"A345", QUAD}, {"A346", QUAD}, {"A388", QUAD}, {"B741", QUAD}, {"B742", QUAD},
  {"B743", QUAD}, {"B744", QUAD}, {"B748", QUAD}, {"B74S", QUAD}, {"C17", QUAD}, {"C5M", QUAD}, {"A124", QUAD},
  {"A225", QUAD}, {"IL76", QUAD}, {"E3TF", QUAD}, {"E3CF", QUAD}, {"E6", QUAD}, {"B52", QUAD}, {"A400", QUAD},
  {"B461", QUAD}, {"B462", QUAD}, {"B463", QUAD}, {"RJ85", QUAD}, {"RJ1H", QUAD},
  // Business and regional jets
  {"C25A", JET}, {"C25B", JET}, {"C25C", JET}, {"C25M", JET}, {"C500", JET}, {"C510", JET}, {"C525", JET}, {"C550", JET},
  {"C560", JET}, {"C56X", JET}, {"C650", JET}, {"C680", JET}, {"C68A", JET}, {"C700", JET}, {"C750", JET}, {"CL30", JET},
  {"CL35", JET}, {"CL60", JET}, {"GLF4", JET}, {"GLF5", JET}, {"GLF6", JET}, {"GLEX", JET}, {"GL5T", JET}, {"GL7T", JET},
  {"G280", JET}, {"GA5C", JET}, {"GA6C", JET}, {"GA7C", JET}, {"GA8C", JET}, {"F2TH", JET}, {"F900", JET}, {"FA7X", JET},
  {"FA8X", JET}, {"FA50", JET}, {"E50P", JET}, {"E55P", JET}, {"E545", JET}, {"E550", JET}, {"E135", JET}, {"E145", JET},
  {"LJ35", JET}, {"LJ40", JET}, {"LJ45", JET}, {"LJ60", JET}, {"LJ75", JET}, {"H25B", JET}, {"PRM1", JET}, {"HDJT", JET},
  {"SF50", JET}, {"BE40", JET}, {"PC24", JET}, {"ASTR", JET}, {"GALX", JET}, {"CRJ1", JET}, {"CRJ2", JET}, {"CRJ7", JET},
  {"CRJ9", JET}, {"CRJX", JET}, {"EA50", JET},
  // Turboprops
  {"AT43", TURBOPROP}, {"AT45", TURBOPROP}, {"AT72", TURBOPROP}, {"AT75", TURBOPROP}, {"AT76", TURBOPROP},
  {"DH8A", TURBOPROP}, {"DH8B", TURBOPROP}, {"DH8C", TURBOPROP}, {"DH8D", TURBOPROP}, {"SF34", TURBOPROP},
  {"SB20", TURBOPROP}, {"B190", TURBOPROP}, {"BE20", TURBOPROP}, {"BE9L", TURBOPROP}, {"BE30", TURBOPROP},
  {"B350", TURBOPROP}, {"PC12", TURBOPROP}, {"PC6T", TURBOPROP}, {"C208", TURBOPROP}, {"TBM7", TURBOPROP},
  {"TBM8", TURBOPROP}, {"TBM9", TURBOPROP}, {"P46T", TURBOPROP}, {"PAY2", TURBOPROP}, {"PAY3", TURBOPROP},
  {"E120", TURBOPROP}, {"JS31", TURBOPROP}, {"JS32", TURBOPROP}, {"JS41", TURBOPROP}, {"D228", TURBOPROP},
  {"D328", TURBOPROP}, {"DHC6", TURBOPROP}, {"DHC7", TURBOPROP}, {"F50", TURBOPROP}, {"L410", TURBOPROP},
  {"C130", TURBOPROP}, {"C30J", TURBOPROP}, {"KODI", TURBOPROP}, {"E110", TURBOPROP}, {"SW4", TURBOPROP},
  {"AN26", TURBOPROP}, {"P3", TURBOPROP}, {"P8", AIRLINER}, {"M600", TURBOPROP}, {"C212", TURBOPROP},
  {"C295", TURBOPROP}, {"CN35", TURBOPROP}, {"PC21", TURBOPROP}, {"TEX2", TURBOPROP}, {"AC90", TURBOPROP},
  // Light aircraft
  {"C150", LIGHT}, {"C152", LIGHT}, {"C162", LIGHT}, {"C170", LIGHT}, {"C172", LIGHT}, {"C175", LIGHT}, {"C177", LIGHT},
  {"C180", LIGHT}, {"C182", LIGHT}, {"C185", LIGHT}, {"C195", LIGHT}, {"C206", LIGHT}, {"C207", LIGHT}, {"C210", LIGHT},
  {"P210", LIGHT}, {"C310", LIGHT}, {"C340", LIGHT}, {"C414", LIGHT}, {"C421", LIGHT}, {"P28A", LIGHT}, {"P28B", LIGHT},
  {"P28R", LIGHT}, {"P28T", LIGHT}, {"P32R", LIGHT}, {"PA32", LIGHT}, {"PA34", LIGHT}, {"PA44", LIGHT}, {"PA46", LIGHT},
  {"PA18", LIGHT}, {"PA22", LIGHT}, {"PA24", LIGHT}, {"PA30", LIGHT}, {"PA31", LIGHT}, {"J3", LIGHT}, {"SR20", LIGHT},
  {"SR22", LIGHT}, {"S22T", LIGHT}, {"BE33", LIGHT}, {"BE35", LIGHT}, {"BE36", LIGHT}, {"BE55", LIGHT}, {"BE58", LIGHT},
  {"BE76", LIGHT}, {"BE23", LIGHT}, {"M20P", LIGHT}, {"M20T", LIGHT}, {"DA20", LIGHT}, {"DA40", LIGHT}, {"DA42", LIGHT},
  {"DA62", LIGHT}, {"DR40", LIGHT}, {"TB10", LIGHT}, {"TB20", LIGHT}, {"AA5", LIGHT}, {"RV4", LIGHT}, {"RV6", LIGHT},
  {"RV7", LIGHT}, {"RV8", LIGHT}, {"RV9", LIGHT}, {"RV10", LIGHT}, {"RV12", LIGHT}, {"RV14", LIGHT}, {"CH7A", LIGHT},
  {"CH7B", LIGHT}, {"AMUN", LIGHT}, {"G115", LIGHT}, {"EV97", LIGHT}, {"P208", LIGHT}, {"C82R", LIGHT}, {"C72R", LIGHT},
  {"GA8", LIGHT}, {"COL4", LIGHT}, {"LNC4", LIGHT}, {"AT3", LIGHT}, {"TOBA", LIGHT}, {"SIRA", LIGHT}, {"PIVI", LIGHT},
  {"ULAC", LIGHT}, {"P92", LIGHT}, {"C42", LIGHT}, {"FDCT", LIGHT}, {"BL8", LIGHT}, {"AR11", LIGHT}, {"PTS2", LIGHT},
  {"T6", LIGHT}, {"P51", LIGHT}, {"SPIT", LIGHT}, {"BT36", LIGHT},
  // Helicopters
  {"R22", HELICOPTER}, {"R44", HELICOPTER}, {"R66", HELICOPTER}, {"AS50", HELICOPTER}, {"AS55", HELICOPTER},
  {"AS65", HELICOPTER}, {"AS32", HELICOPTER}, {"AS3B", HELICOPTER}, {"EC20", HELICOPTER}, {"EC30", HELICOPTER},
  {"EC35", HELICOPTER}, {"EC45", HELICOPTER}, {"EC55", HELICOPTER}, {"EC75", HELICOPTER}, {"EC25", HELICOPTER},
  {"H160", HELICOPTER}, {"B06", HELICOPTER}, {"B06T", HELICOPTER}, {"B204", HELICOPTER}, {"B205", HELICOPTER},
  {"B212", HELICOPTER}, {"B222", HELICOPTER}, {"B230", HELICOPTER}, {"B407", HELICOPTER}, {"B412", HELICOPTER},
  {"B427", HELICOPTER}, {"B429", HELICOPTER}, {"B430", HELICOPTER}, {"B505", HELICOPTER}, {"B525", HELICOPTER},
  {"S61", HELICOPTER}, {"S64", HELICOPTER}, {"S70", HELICOPTER}, {"S76", HELICOPTER}, {"S92", HELICOPTER},
  {"H60", HELICOPTER}, {"H47", HELICOPTER}, {"H53", HELICOPTER}, {"H53S", HELICOPTER}, {"H64", HELICOPTER},
  {"UH1", HELICOPTER}, {"UH1Y", HELICOPTER}, {"AH1", HELICOPTER}, {"A109", HELICOPTER}, {"A119", HELICOPTER},
  {"A139", HELICOPTER}, {"A149", HELICOPTER}, {"A169", HELICOPTER}, {"A189", HELICOPTER}, {"AW09", HELICOPTER},
  {"BK17", HELICOPTER}, {"MD52", HELICOPTER}, {"MD60", HELICOPTER}, {"H500", HELICOPTER}, {"H269", HELICOPTER},
  {"EN28", HELICOPTER}, {"EN48", HELICOPTER}, {"G2CA", HELICOPTER}, {"MI8", HELICOPTER}, {"NH90", HELICOPTER},
  {"LYNX", HELICOPTER}, {"PUMA", HELICOPTER}, {"GAZL", HELICOPTER}, {"ALO3", HELICOPTER}, {"V22", HELICOPTER},
  {"GYRO", HELICOPTER}, {"CDUS", HELICOPTER},
  // High performance, military
  {"F16", FAST}, {"F15", FAST}, {"F18", FAST}, {"F18S", FAST}, {"F18H", FAST}, {"F35", FAST}, {"F22", FAST},
  {"F14", FAST}, {"F4", FAST}, {"F5", FAST}, {"A10", FAST}, {"EUFI", FAST}, {"TOR", FAST}, {"RFAL", FAST},
  {"MIR2", FAST}, {"GRIF", FAST}, {"SU27", FAST}, {"SU30", FAST}, {"MG29", FAST}, {"T38", FAST}, {"T45", FAST},
  {"HAWK", FAST}, {"L39", FAST}, {"AJET", FAST}, {"M346", FAST}, {"B1", FAST}, {"B2", FAST}, {"EA18", FAST},
  {"T7", FAST}, {"U2", FAST},
  // Gliders, balloons, airships
  {"GLID", GLIDER}, {"ASK2", GLIDER}, {"AS21", GLIDER}, {"DG80", GLIDER}, {"DISC", GLIDER}, {"DUOD", GLIDER},
  {"LS8", GLIDER}, {"ARCP", GLIDER}, {"VENT", GLIDER}, {"NIMB", GLIDER}, {"ASW2", GLIDER}, {"ASG2", GLIDER},
  {"BALL", BALLOON}, {"SHIP", AIRSHIP}, {"ZZ1", AIRSHIP},
  // Drones (most say so by their class, B6, or their description instead)
  {"Q4", DRONE}, {"Q9", DRONE}, {"Q1", DRONE}, {"MQ9", DRONE}, {"RQ4", DRONE}, {"HRON", DRONE}, {"UAV", DRONE},
  {"TRTN", DRONE},
  // On the ground
  {"GRND", VEHICLE}, {"SERV", VEHICLE}, {"EMER", VEHICLE}, {"TWR", VEHICLE},
};
// clang-format on

// The kind of a type, or KINDS when the table does not know it.
inline Kind kind_of_type(const std::string &type) {
  if (type.empty()) return KINDS;
  for (const Type &t : TYPES)
    if (type == t.code) return t.kind;
  return KINDS;
}

// The kind of a transponder's class (DO-260B emitter category), or KINDS for one that says nothing of its shape.
inline Kind kind_of_category(const std::string &c) {
  if (c.size() != 2) return KINDS;
  switch (c[0]) {
    case 'A':
      switch (c[1]) {
        case '1': return LIGHT;      // light, under 15,500 lb
        case '2': return JET;        // small, to 75,000 lb
        case '3': return AIRLINER;   // large, to 300,000 lb
        case '4': return AIRLINER;   // a high vortex large (757)
        case '5': return HEAVY;      // heavy, over 300,000 lb
        case '6': return FAST;       // high performance
        case '7': return HELICOPTER; // rotorcraft
      }
      return KINDS;
    case 'B':
      switch (c[1]) {
        case '1': return GLIDER;
        case '2': return BALLOON;    // lighter than air
        case '4': return LIGHT;      // ultralight, hang glider, paraglider
        case '6': return DRONE;      // unmanned
      }
      return KINDS;
    case 'C':
      return c[1] == '1' || c[1] == '2' ? VEHICLE : KINDS;  // emergency and service vehicles (C3 and on: obstacles)
  }
  return KINDS;
}

// The kind told by a description's words (an exchange's `desc`, "ROBINSON R-44 RAVEN II"), or KINDS.
inline Kind kind_of_words(const std::string &desc) {
  struct Word {
    const char *word;
    Kind kind;
  };
  static constexpr Word WORDS[] = {{"DRONE", DRONE},       {"UAV", DRONE},          {"REAPER", DRONE},
                                   {"PREDATOR", DRONE},    {"GLOBAL HAWK", DRONE},  {"UNMANNED", DRONE},
                                   {"HELICOPTER", HELICOPTER}, {"GYROPLANE", HELICOPTER}, {"GYROCOPTER", HELICOPTER},
                                   {"GLIDER", GLIDER},     {"SAILPLANE", GLIDER},   {"BALLOON", BALLOON},
                                   {"AIRSHIP", AIRSHIP},   {"BLIMP", AIRSHIP}};
  if (desc.empty()) return KINDS;
  std::string upper = desc;
  for (auto &ch : upper) if (ch >= 'a' && ch <= 'z') ch -= 32;
  for (const Word &w : WORDS)
    if (upper.find(w.word) != std::string::npos) return w.kind;
  return KINDS;
}

// A plane's kind: its type where the table knows it (the most exact), else its words (a drone says so), else its
// transponder's class (a heavy says it is heavy, a light aircraft light), else an airliner. A lighter-than-air class
// that names an airship is one.
inline Kind kind_of(const std::string &category, const std::string &type, const std::string &desc) {
  const Kind by_type = kind_of_type(type);
  if (by_type != KINDS) return by_type;
  const Kind by_words = kind_of_words(desc);
  if (by_words != KINDS) return by_words;
  const Kind by_class = kind_of_category(category);
  if (by_class != KINDS) return by_class;
  return AIRLINER;
}
}  // namespace sky::shapes
