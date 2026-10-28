#pragma once
// The map under the planes: 256-pixel raster tiles in Web Mercator squares from a public tile server, fetched and
// decoded on the screen itself. Which tiles a frame needs, at which zoom, and where each lands is worked out here, with
// no network or decoder, so the tests run on a PC; http.cpp fetches and decode.cpp paints.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "geo.h"
#include "settings.h"

namespace sky::tiles {

// How a map that has no dark version of its own is drawn in the dark appearance (Tessera's night rules):
//  NIGHT_INVERT: its light turned round and its hue back half a circle (a browser's invert(1) hue-rotate(180deg)),
//    fitted between the dark screen's ground and its ink: white paper dark, black lines light, blue water bluish. For
//    drawn maps of line work on white (OpenStreetMap, USGS topo).
//  NIGHT_COLOURS: only its paper and ink turned round; every coloured mark keeps its own colour, lifted so it stands out
//    on the dark paper (as ForeFlight's inverted charts): for the IFR charts, whose colours mean something.
//  NIGHT_DIM: a share of its light (NIGHT_DIM_KEEP): for the sectional, whose colour areas (yellow towns, terrain
//    tints) turn muddy inverted.
//  NIGHT_NONE: as it is: a dark map already, or imagery.
enum Night : uint8_t { NIGHT_NONE, NIGHT_INVERT, NIGHT_COLOURS, NIGHT_DIM };
constexpr int NIGHT_DIM_KEEP = 154;  // of 256: 60 %

struct Source {
  const char *url;     // {z} {x} {y}
  int min_z, max_z;    // the zooms the server draws; a chart's are those it is sharp at
  const char *credit;  // what the screen credits, as the server's terms ask
  const char *labels = nullptr;  // a layer of place names over it (transparent PNG, same squares), where it has one
  const char *labels_credit = nullptr;  // what that layer credits, where it is not the map's own
  // A scan of a printed chart (the FAA's): its words are part of the chart, at the chart's scale, so its tiles are drawn
  // at their own pixels from the level nearest the view (the server's own resampling of the scan is smooth), never one
  // level out and enlarged as a drawn map's are to make its words readable.
  bool scanned = false;
  Night night = NIGHT_NONE;
};
#define SKYSCOPE_ESRI "https://server.arcgisonline.com/ArcGIS/rest/services/"

// The FAA's charts and the USGS topographic map are United States government services and cover the United States.
// The sectional is scanned at about zoom 10 and the IFR charts at 8 (high) and 10 (low); one level closer is still sharp.
#define SKYSCOPE_FAA "https://tiles.arcgis.com/tiles/ssFJjBXIUyZDrSYZ/arcgis/rest/services/"
// The tile servers behind the maps a person chooses (settings.h MapStyle): `streets` is one of two, by the appearance.
enum SourceId : uint8_t { SRC_NONE, SRC_OSM, SRC_GRAY_LIGHT, SRC_GRAY_DARK, SRC_SATELLITE, SRC_TOPO, SRC_SECTIONAL, SRC_IFR_LOW,
                          SRC_IFR_HIGH, SRC_CUSTOM };
inline constexpr Source SOURCES[] = {
    /* SRC_NONE (the radar) */ {"", 0, 0, ""},
    /* SRC_OSM */ {"https://tile.openstreetmap.org/{z}/{x}/{y}.png", 0, 19, "\xC2\xA9 OpenStreetMap contributors", nullptr,
                   nullptr, false, NIGHT_INVERT},
    // Esri's gray canvas maps: streets and water, no place names (Esri keeps those in a layer of their own).
    /* SRC_GRAY_LIGHT */ {SKYSCOPE_ESRI "Canvas/World_Light_Gray_Base/MapServer/tile/{z}/{y}/{x}", 0, 16,
                      "Esri, HERE, Garmin, \xC2\xA9 OpenStreetMap",
                      SKYSCOPE_ESRI "Canvas/World_Light_Gray_Reference/MapServer/tile/{z}/{y}/{x}"},
    /* SRC_GRAY_DARK */ {SKYSCOPE_ESRI "Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}", 0, 16,
                     "Esri, HERE, Garmin, \xC2\xA9 OpenStreetMap",
                     SKYSCOPE_ESRI "Canvas/World_Dark_Gray_Reference/MapServer/tile/{z}/{y}/{x}"},
    // The credits are Esri's own (each service's copyrightText), shortened.
    /* SRC_SATELLITE */ {SKYSCOPE_ESRI "World_Imagery/MapServer/tile/{z}/{y}/{x}", 0, 19, "Esri, Vantor, Earthstar Geographics",
                     SKYSCOPE_ESRI "Reference/World_Boundaries_and_Places/MapServer/tile/{z}/{y}/{x}",
                     "HERE, Garmin, \xC2\xA9 OpenStreetMap"},
    /* SRC_TOPO */ {"https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/tile/{z}/{y}/{x}", 0, 16, "USGS",
                    nullptr, nullptr, false, NIGHT_INVERT},
    /* SRC_SECTIONAL */ {SKYSCOPE_FAA "VFR_Sectional/MapServer/tile/{z}/{y}/{x}", 8, 11, "FAA", nullptr, nullptr, true, NIGHT_DIM},
    /* SRC_IFR_LOW */ {SKYSCOPE_FAA "IFR_AreaLow/MapServer/tile/{z}/{y}/{x}", 7, 11, "FAA", nullptr, nullptr, true, NIGHT_COLOURS},
    /* SRC_IFR_HIGH */ {SKYSCOPE_FAA "IFR_High/MapServer/tile/{z}/{y}/{x}", 5, 9, "FAA", nullptr, nullptr, true, NIGHT_COLOURS},
    // A server of one's own is taken as it is: nothing is known of it.
    /* SRC_CUSTOM */ {"", 0, 19, ""},
};
#undef SKYSCOPE_FAA
#undef SKYSCOPE_ESRI
inline const Source &source(SourceId id) { return SOURCES[id < sizeof(SOURCES) / sizeof(SOURCES[0]) ? id : 0]; }
// The server of the map `s` shows.
inline SourceId source_id(const Settings &s) {
  switch (s.map) {
    case OSM: return SRC_OSM;
    case STREETS: return s.theme == LIGHT ? SRC_GRAY_LIGHT : SRC_GRAY_DARK;
    case SATELLITE: return SRC_SATELLITE;
    case TOPO: return SRC_TOPO;
    case SECTIONAL: return SRC_SECTIONAL;
    case IFR_LOW: return SRC_IFR_LOW;
    case IFR_HIGH: return SRC_IFR_HIGH;
    case CUSTOM: return SRC_CUSTOM;
    default: return SRC_NONE;
  }
}
inline const Source &source(const Settings &s) { return source(source_id(s)); }
inline bool has_tiles(MapStyle style) { return style != RADAR; }
// Whether the frame of `s` takes a layer of place names over its map.
inline bool has_labels(const Settings &s) { return s.place_names && has_tiles(s.map) && source(s).labels; }
// What the screen credits for the map of `s`: its own credit, and its layer of names' where that is another.
inline std::string credit(const Settings &s) {
  const Source &src = source(s);
  std::string out = src.credit;
  if (has_labels(s) && src.labels_credit) out += std::string(", ") + src.labels_credit;
  return out;
}

// The most tiles one frame asks for; a frame that needs more (a chart far before its first zoom) is drawn without one.
constexpr size_t MOST_TILES = 30;
// How far a tile may be enlarged or made smaller, as a power of two.
constexpr int MOST_UP = 3, MOST_DOWN = 1;

// A tile: its server (SourceId), zoom and square, and its layer (0 the map, 1 its place names).
struct Key {
  uint8_t style, z;
  uint32_t x, y;
  uint8_t layer = 0;
  bool operator==(const Key &o) const { return style == o.style && z == o.z && x == o.x && y == o.y && layer == o.layer; }
};

struct Plan {
  geo::View view;          // the frame, at a whole zoom for a map of tiles
  int tile_z = -1;         // the zoom of its tiles; -1: none (the radar, or a chart that does not reach this range)
  std::vector<Key> keys;   // nearest the middle first, so the middle of the map comes first
};

// The frame of `range_nm` round a place on a glass `w` x `h`, with the tiles under it. `tile_scale` is how many pixels
// of the glass one tile pixel takes (1 or 2), so a dense glass can draw the words of a map large enough to read.
inline Plan plan(const Settings &s, int w, int h, int tile_scale) {
  Plan out;
  const double wanted = geo::zoom_for_radius(s.lat, s.range_nm * geo::NM, std::min(w, h));
  if (!has_tiles(s.map)) {
    out.view = geo::View(s.lat, s.lon, wanted, w, h);
    return out;
  }
  const int zoom = (int) std::floor(wanted + 0.5);
  out.view = geo::View(s.lat, s.lon, zoom, w, h);
  const Source &src = source(s);
  // A drawn map one level out, enlarged twice, on a dense glass (its words readable); a scanned chart at its own pixels.
  const int up = tile_scale >= 2 && !src.scanned ? 1 : 0;
  int tz = std::max(src.min_z, std::min(src.max_z, zoom - up));
  const int shift = (int) out.view.zoom - tz;
  if (shift > MOST_UP || shift < -MOST_DOWN) return out;
  const double span = geo::TILE_PX * std::pow(2.0, out.view.zoom - tz);
  const int count = 1 << tz;
  const int x0 = (int) std::floor(out.view.left / span), x1 = (int) std::floor((out.view.left + w - 1) / span);
  const int y0 = (int) std::floor(out.view.top / span), y1 = (int) std::floor((out.view.top + h - 1) / span);
  struct Near { double d; Key k; };
  // Each map tile's place names come right after it, so the names show as each piece of the map does.
  const bool labels = has_labels(s);
  std::vector<Near> near;
  const double cx = out.view.left + w / 2.0, cy = out.view.top + h / 2.0;
  for (int y = std::max(0, y0); y <= std::min(count - 1, y1); ++y)
    for (int x = x0; x <= x1; ++x) {
      const double d = std::hypot((x + 0.5) * span - cx, (y + 0.5) * span - cy);
      near.push_back({d, {(uint8_t) source_id(s), (uint8_t) tz, (uint32_t) (((x % count) + count) % count), (uint32_t) y}});
    }
  if (near.size() > MOST_TILES) return out;
  std::sort(near.begin(), near.end(), [](const Near &a, const Near &b) { return a.d < b.d; });
  out.tile_z = tz;
  for (const auto &n : near) {
    out.keys.push_back(n.k);
    if (labels) {
      Key l = n.k;
      l.layer = 1;
      out.keys.push_back(l);
    }
  }
  return out;
}

// The address of a tile.
inline std::string url(const Settings &s, const Key &k) {
  const Source &src = source((SourceId) k.style);
  std::string t = k.layer && src.labels ? src.labels : k.style == SRC_CUSTOM ? s.custom_url : src.url;
  auto put = [&](const char *name, uint32_t value) {
    const size_t at = t.find(name);
    if (at != std::string::npos) t.replace(at, 3, std::to_string(value));
  };
  put("{z}", k.z);
  put("{x}", k.x);
  put("{y}", k.y);
  return t;
}

// What a tile's bytes are.
enum Format { UNKNOWN, PNG, JPEG };
inline Format format_of(const uint8_t *data, size_t size) {
  if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return PNG;
  if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return JPEG;
  return UNKNOWN;
}

// How a tile's colours are turned on their way to the glass: the map's night rule in the dark appearance (Source
// night), then the map brightness the person set, in either appearance.
struct Filter {
  Night night = NIGHT_NONE;
  uint16_t keep = 256;  // of 256: the map brightness
};
inline Filter filter_of(const Settings &s) {
  return {s.theme == DARK && s.map != CUSTOM ? source(s).night : NIGHT_NONE, (uint16_t) (s.map_brightness * 256 / 100)};
}

inline uint16_t rgb565(int r, int g, int b) {
  auto c = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
  return (uint16_t) ((c(r) & 0xF8) << 8 | (c(g) & 0xFC) << 3 | c(b) >> 3);
}
// The dark appearance's ground and ink, which a turned map is fitted between (colours.h BACKGROUND is black; a map's
// darkest is a touch above it, so its edges still read against the glass's black).
constexpr int NIGHT_LOW = 24, NIGHT_HIGH = 226;
struct Rgb { int r, g, b; };
// Light turned round, hue back half a circle (CSS invert(1) hue-rotate(180deg), the matrix in thousandths), fitted
// between NIGHT_LOW and NIGHT_HIGH.
inline Rgb turned(int r, int g, int b) {
  const int ir = 255 - r, ig = 255 - g, ib = 255 - b;
  auto clamp = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
  auto fit = [&](int v) { return NIGHT_LOW + clamp(v) * (NIGHT_HIGH - NIGHT_LOW) / 255; };
  return {fit((-574 * ir + 1430 * ig + 144 * ib) / 1000), fit((426 * ir + 430 * ig + 144 * ib) / 1000),
          fit((426 * ir + 1430 * ig - 856 * ib) / 1000)};
}
// 255 at or below `full`, 0 from `none`, a straight line between.
inline int ramp(int v, int full, int none) { return v <= full ? 255 : v >= none ? 0 : 255 * (none - v) / (none - full); }
// NIGHT_COLOURS: a pixel this grey (saturation, 0 to 255) or this dark (value) is paper or ink, and turned round; a
// coloured one keeps its colour, lifted to at least COLOUR_VALUE, so it stands out on the dark paper.
constexpr int GREY_FULL = 50, GREY_NONE = 110, INK_FULL = 70, INK_NONE = 120, COLOUR_VALUE = 190;
inline Rgb night_colours(int r, int g, int b) {
  const int v = std::max({r, g, b}), mn = std::min({r, g, b});
  const int sat = v ? (v - mn) * 255 / v : 0;
  const int grey = std::max(ramp(sat, GREY_FULL, GREY_NONE), ramp(v, INK_FULL, INK_NONE));  // 255: paper or ink
  const Rgb t = turned(r, g, b);
  Rgb lifted{r, g, b};
  if (v > 0 && v < COLOUR_VALUE) lifted = {r * COLOUR_VALUE / v, g * COLOUR_VALUE / v, b * COLOUR_VALUE / v};
  return {(t.r * grey + lifted.r * (255 - grey)) / 255, (t.g * grey + lifted.g * (255 - grey)) / 255,
          (t.b * grey + lifted.b * (255 - grey)) / 255};
}
inline uint16_t paint(const Filter &f, int r, int g, int b) {
  Rgb c{r, g, b};
  switch (f.night) {
    case NIGHT_INVERT: c = turned(r, g, b); break;
    case NIGHT_COLOURS: c = night_colours(r, g, b); break;
    case NIGHT_DIM: c = {r * NIGHT_DIM_KEEP >> 8, g * NIGHT_DIM_KEEP >> 8, b * NIGHT_DIM_KEEP >> 8}; break;
    default: break;
  }
  if (f.keep < 256) c = {c.r * f.keep >> 8, c.g * f.keep >> 8, c.b * f.keep >> 8};
  return rgb565(c.r, c.g, c.b);
}
}  // namespace sky::tiles
