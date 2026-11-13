#pragma once
// The map card: an SD card in the board's slot, where the screen keeps every map tile it fetches, so a map seen once
// comes back without the network (after a restart too), and where it downloads an area of the FAA's charts or the
// USGS topographic map ahead of time, for a slow network or none. What goes where on the card, how long a tile is
// good for, and which tiles an area takes are worked out here, with no card or network, so the tests run on a PC;
// card.cpp reads and writes the card and worker.cpp decides when.
//
// A tile is a file of its own, under a folder for its map and layer, its zoom and its column:
//   /sd/tiles/06_0/11/534/779.t   the sectional (SRC_SECTIONAL 6), the map itself (layer 0), zoom 11, x 534, y 779
// Every name fits 8.3, so the card needs no long names (ESP-IDF's FAT is built without them), and no folder holds more
// files than a column of an area has. Each file starts with a Header, then the tile's bytes as the server sent them.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "geo.h"
#include "tiles.h"

namespace sky::card {

constexpr const char *ROOT = "/sd";
constexpr uint32_t DAY_S = 86400;
// Every file is written here first and then renamed into its place, so a write cut short (a crash, the power gone)
// leaves at most this one file, written over by the next.
inline std::string temp_path(const char *root = ROOT) { return std::string(root) + "/tiles/write.tmp"; }
// Tiles being cleared: the folder of tiles is moved in here at once (a rename), and its files deleted a few at a time
// as the screen has time, across restarts too.
inline std::string trash_path(const char *root = ROOT) { return std::string(root) + "/trash"; }
inline std::string trash_name(uint32_t n, const char *root = ROOT) {
  char text[16];
  std::snprintf(text, sizeof(text), "/t%07x", (unsigned) (n & 0xFFFFFFF));
  return trash_path(root) + text;
}

// Whether a tile is kept on the card: a map's own tiles and its layer of names, from the servers the screen knows. Not
// the weather radar's (its scans go old in minutes), nor a custom server's (its address can change under the same
// squares).
inline bool cacheable(const tiles::Key &k) {
  return k.layer <= 1 && k.style >= tiles::SRC_OSM && k.style <= tiles::SRC_IFR_HIGH;
}

// The FAA's charts and the USGS topographic map are works of the United States government, free of copyright, and
// may be downloaded by area. The others (OpenStreetMap's volunteers' servers, Esri's) ask that tiles are fetched only
// as they are looked at, so those are only kept as they are seen.
inline bool may_download(tiles::SourceId s) {
  return s == tiles::SRC_SECTIONAL || s == tiles::SRC_IFR_LOW || s == tiles::SRC_IFR_HIGH || s == tiles::SRC_TOPO;
}

// How long a tile fetched now is good for, in seconds; 0: not kept at all. A chart is published anew every 56 days,
// so its tiles are looked at again after 28; the topographic map changes slowly. OpenStreetMap's and Esri's are kept
// as long as their server says (Cache-Control max-age), a week where it says nothing.
inline uint32_t lifetime_s(uint8_t style, int max_age_s) {
  switch (style) {
    case tiles::SRC_SECTIONAL:
    case tiles::SRC_IFR_LOW:
    case tiles::SRC_IFR_HIGH:
      return 28 * DAY_S;
    case tiles::SRC_TOPO:
      return 90 * DAY_S;
    default:
      return max_age_s < 0 ? 7 * DAY_S : (uint32_t) max_age_s;
  }
}

// A tile's file on the card.
inline std::string path(const tiles::Key &k, const char *root = ROOT) {
  char text[80];
  std::snprintf(text, sizeof(text), "%s/tiles/%02u_%u/%u/%u/%u.t", root, (unsigned) k.style, (unsigned) k.layer,
                (unsigned) k.z, (unsigned) k.x, (unsigned) k.y);
  return text;
}
// The folders above it, outermost first, for making them.
inline std::vector<std::string> folders(const tiles::Key &k, const char *root = ROOT) {
  const std::string p = path(k, root);
  std::vector<std::string> out;
  for (size_t at = std::strlen(root) + 1; (at = p.find('/', at)) != std::string::npos; ++at)
    out.push_back(p.substr(0, at));
  return out;
}

// What a tile's file says of it before its bytes: when it was fetched and until when it is good (Unix seconds, 0
// when the screen's clock was not set yet), and whether the server had no tile there (outside a chart's country).
struct Header {
  uint32_t fetched = 0, expires = 0;
  bool empty = false;
};
constexpr size_t HEADER_BYTES = 16;
inline void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = (uint8_t) (v >> (8 * i));
}
inline uint32_t get32(const uint8_t *p) {
  return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}
inline void encode(const Header &h, uint8_t out[HEADER_BYTES]) {
  std::memset(out, 0, HEADER_BYTES);
  std::memcpy(out, "SKT1", 4);
  put32(out + 4, h.fetched);
  put32(out + 8, h.expires);
  out[12] = h.empty ? 1 : 0;
}
inline bool decode(const uint8_t *in, size_t size, Header &h) {
  if (size < HEADER_BYTES || std::memcmp(in, "SKT1", 4) != 0) return false;
  h.fetched = get32(in + 4);
  h.expires = get32(in + 8);
  h.empty = in[12] & 1;
  return true;
}
// The header of a tile fetched at `now` (Unix seconds, 0 while the clock is not set) for `lifetime` seconds: one
// written before the clock was set is old as soon as it is.
inline Header header_for(uint32_t now, uint32_t lifetime, bool empty) {
  Header h;
  h.fetched = now;
  h.expires = now ? now + lifetime : 0;
  h.empty = empty;
  return h;
}
// Whether a tile on the card is still good at `now`. With the clock not set (no network to set it), every tile is:
// there is nothing better to show.
inline bool fresh(const Header &h, uint32_t now) { return now == 0 || h.expires > now; }

// ---- An area to download: one map's tiles in a square around a place, every zoom from the map's first to `z_max`,
// the lowest zoom first (a download cut short still covers the whole area, if not as sharply).
constexpr int MOST_RADIUS_NM = 250;
constexpr size_t MOST_TILES = 40000;
struct Area {
  uint8_t style = tiles::SRC_SECTIONAL;
  double lat = 0, lon = 0;
  int radius_nm = 100;
  int z_max = 11;
};
struct Range {
  int z;
  uint32_t x0, x1, y0, y1;  // inclusive
  size_t count() const { return (size_t) (x1 - x0 + 1) * (y1 - y0 + 1); }
};
struct Plan {
  Area area;
  std::vector<Range> ranges;
  size_t total = 0;
  // The i-th tile of the area (i < total).
  tiles::Key at(size_t i) const {
    for (const Range &r : ranges) {
      if (i < r.count()) {
        const uint32_t w = r.x1 - r.x0 + 1;
        tiles::Key k{area.style, (uint8_t) r.z, r.x0 + (uint32_t) (i % w), r.y0 + (uint32_t) (i / w)};
        return k;
      }
      i -= r.count();
    }
    return tiles::Key{area.style, 0, 0, 0};
  }
};
// The zooms a download of this map may reach, and the one offered first: every zoom of a chart (each is sharp at
// its own), the topographic map to a town's streets.
inline int first_zoom(uint8_t style) { return tiles::source((tiles::SourceId) style).min_z; }
inline int last_zoom(uint8_t style) { return tiles::source((tiles::SourceId) style).max_z; }
inline int default_zoom(uint8_t style) { return style == tiles::SRC_TOPO ? 12 : last_zoom(style); }

// The tiles of `a`, or false with why not.
inline bool plan(const Area &a, Plan &out, std::string &error) {
  out = Plan{};
  out.area = a;
  if (!may_download((tiles::SourceId) a.style)) {
    error = "This map is only kept as it is seen: only the FAA's charts and the USGS topographic map are downloaded";
    return false;
  }
  if (!(a.lat >= -geo::MAX_LATITUDE && a.lat <= geo::MAX_LATITUDE && a.lon >= -180 && a.lon <= 180)) {
    error = "No place";
    return false;
  }
  if (a.radius_nm < 1 || a.radius_nm > MOST_RADIUS_NM) {
    error = "The radius is 1 to " + std::to_string(MOST_RADIUS_NM) + " NM";
    return false;
  }
  const int z0 = first_zoom(a.style), z1 = last_zoom(a.style);
  if (a.z_max < z0 || a.z_max > z1) {
    error = "This map's zooms are " + std::to_string(z0) + " to " + std::to_string(z1);
    return false;
  }
  // The square's corners: the radius north and south, and east and west at the place's latitude.
  const double dlat = a.radius_nm * geo::NM / (geo::EARTH_METERS * geo::PI / 180.0);
  const double dlon = std::min(180.0, dlat / std::max(0.01, std::cos(geo::radians(a.lat))));
  for (int z = z0; z <= a.z_max; ++z) {
    const uint32_t n = 1u << z;
    const geo::Pixel nw = geo::world(std::min(geo::MAX_LATITUDE, a.lat + dlat), std::max(-180.0, a.lon - dlon), z);
    const geo::Pixel se = geo::world(std::max(-geo::MAX_LATITUDE, a.lat - dlat), std::min(180.0, a.lon + dlon), z);
    auto tile = [&](double px) {
      return (uint32_t) std::max(0.0, std::min((double) n - 1, std::floor(px / geo::TILE_PX)));
    };
    Range r{z, tile(nw.x), tile(se.x), tile(nw.y), tile(se.y)};
    out.ranges.push_back(r);
    out.total += r.count();
  }
  if (out.total > MOST_TILES) {
    error = "That is " + std::to_string(out.total) + " tiles, more than " + std::to_string(MOST_TILES) +
            " at once: a smaller radius or a lower zoom";
    return false;
  }
  return true;
}
// About how many bytes a tile of this map is, as it is downloaded: a chart's scanned tiles are larger than the
// topographic map's.
inline uint32_t tile_bytes(uint8_t style) { return style == tiles::SRC_TOPO ? 24 * 1024 : 40 * 1024; }
inline uint64_t estimate_bytes(const Plan &p) { return (uint64_t) p.total * tile_bytes(p.area.style); }
// The card's clusters (its blocks), where it is not known yet: what FAT32 cards of 8 to 32 GB come formatted with,
// and what the screen formats with (card.cpp).
constexpr uint32_t CLUSTER_GUESS = 32 * 1024;
// About how much of the card an area takes: every file fills whole clusters, so each tile (its bytes and its header)
// takes on average half a cluster more than it is, the sizes of tiles being spread wider than a cluster.
inline uint64_t card_bytes(const Plan &p, uint32_t cluster) {
  if (!cluster) cluster = CLUSTER_GUESS;
  return (uint64_t) p.total * (tile_bytes(p.area.style) + HEADER_BYTES + cluster / 2);
}

// A download under way, as it is kept on the card (so it carries on after a restart): its area and how far it got.
inline std::string job_text(const Area &a, size_t next) {
  char text[96];
  std::snprintf(text, sizeof(text), "%u %.6f %.6f %d %d %zu\n", (unsigned) a.style, a.lat, a.lon, a.radius_nm, a.z_max,
                next);
  return text;
}
inline bool read_job(const char *text, Area &a, size_t &next) {
  unsigned style = 0;
  double lat = 0, lon = 0;
  int radius = 0, z = 0;
  unsigned long long n = 0;
  if (std::sscanf(text, "%u %lf %lf %d %d %llu", &style, &lat, &lon, &radius, &z, &n) != 6) return false;
  a.style = (uint8_t) style;
  a.lat = lat;
  a.lon = lon;
  a.radius_nm = radius;
  a.z_max = z;
  next = (size_t) n;
  return true;
}

// The map a download is for, by the name the settings and the page use ("sectional"), and back.
inline bool style_of(const std::string &map, uint8_t &style) {
  if (map == "sectional") style = tiles::SRC_SECTIONAL;
  else if (map == "ifr_low") style = tiles::SRC_IFR_LOW;
  else if (map == "ifr_high") style = tiles::SRC_IFR_HIGH;
  else if (map == "topo") style = tiles::SRC_TOPO;
  else return false;
  return true;
}
inline const char *map_of(uint8_t style) {
  switch (style) {
    case tiles::SRC_SECTIONAL: return "sectional";
    case tiles::SRC_IFR_LOW: return "ifr_low";
    case tiles::SRC_IFR_HIGH: return "ifr_high";
    case tiles::SRC_TOPO: return "topo";
    default: return "";
  }
}

// ---- What is on the card, for the menu (menu.cpp) and the page.

// The card itself, as it says when it is mounted: its name and maker, when it was made, its kind and size, and the
// bus it talks on.
struct About {
  std::string name;  // the few letters its maker gives it: "SD32G"
  uint8_t maker = 0;  // the SD Association's number for its maker
  uint16_t made = 0;  // the year since 2000 times 16, and the month, as the card keeps it
  bool high_capacity = false, mmc = false;
  uint64_t bytes = 0;
  uint32_t khz = 0;  // the clock it runs at
  int lines = 1;  // its data lines
};
// The makers most cards come from, by the numbers they are commonly known to carry (the SD Association does not
// publish its list); nullptr for another.
inline const char *maker_name(uint8_t id) {
  switch (id) {
    case 0x01: return "Panasonic";
    case 0x02: return "Toshiba";
    case 0x03: return "SanDisk";
    case 0x1B: return "Samsung";
    case 0x1D: return "ADATA";
    case 0x27: return "Phison";
    case 0x28: return "Lexar";
    case 0x31: return "Silicon Power";
    case 0x41: return "Kingston";
    case 0x74: return "Transcend";
    case 0x76: return "Patriot";
    case 0x82: return "Sony";
    default: return nullptr;
  }
}
// Its kind, as printed on it: SDSC to 2 GB, SDHC to 32, SDXC above.
inline const char *kind_of(const About &a) {
  if (a.mmc) return "MMC";
  if (!a.high_capacity) return "SDSC";
  return a.bytes > 32ULL * 1024 * 1024 * 1024 ? "SDXC" : "SDHC";
}
// When it was made, "April 2023", or "" where it does not say.
inline std::string made_words(uint16_t made) {
  static const char *const MONTHS[] = {"January", "February", "March",     "April",   "May",      "June",
                                       "July",    "August",   "September", "October", "November", "December"};
  if (!made) return "";
  const int year = 2000 + (made >> 4), month = made & 0xF;
  const std::string y = std::to_string(year);
  return month >= 1 && month <= 12 ? std::string(MONTHS[month - 1]) + " " + y : y;
}
// The card in a line: "SanDisk SD32G, SDHC, made April 2023".
inline std::string about_words(const About &a) {
  std::string out;
  if (const char *m = maker_name(a.maker)) out = m;
  if (!a.name.empty()) out += (out.empty() ? "" : " ") + a.name;
  out += (out.empty() ? "" : ", ") + std::string(kind_of(a));
  const std::string made = made_words(a.made);
  if (!made.empty()) out += ", made " + made;
  return out;
}
// The bus it talks on: "40 MHz on 4 lines".
inline std::string bus_words(const About &a) {
  char text[40];
  std::snprintf(text, sizeof(text), "%u MHz on %d line%s", (unsigned) ((a.khz + 500) / 1000), a.lines,
                a.lines == 1 ? "" : "s");
  return text;
}

// A size as the page writes it too: "640 KB", "52 MB", "1.2 GB" (of 1024 each).
inline std::string size_words(uint64_t bytes) {
  constexpr uint64_t KB = 1024, MB = KB * 1024, GB = MB * 1024;
  char text[24];
  if (bytes < MB) std::snprintf(text, sizeof(text), "%u KB", (unsigned) ((bytes + KB / 2) / KB));
  else if (bytes < GB) std::snprintf(text, sizeof(text), "%u MB", (unsigned) ((bytes + MB / 2) / MB));
  else std::snprintf(text, sizeof(text), "%.1f GB", (double) bytes / GB);
  return text;
}
// A count with commas between its thousands: "12,345".
inline std::string count_words(uint64_t n) {
  std::string digits = std::to_string(n), out;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i && (digits.size() - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

// What the screen keeps on the card, counted file by file (card.cpp walks it a few files at a time): each map's
// tiles (its names with it) and the log of the planes seen, by what they take of the card, in whole clusters.
constexpr int STYLES = tiles::SRC_CUSTOM + 1;
struct Tally {
  uint32_t tiles[STYLES] = {};
  uint64_t bytes[STYLES] = {};
  uint32_t logs = 0;
  uint64_t log_bytes = 0;
  bool counted = false;   // a count has finished: the numbers are the card's
  bool counting = false;  // one is under way: the numbers are the last count's
  uint32_t tile_count() const {
    uint32_t n = 0;
    for (uint32_t t : tiles) n += t;
    return n;
  }
  uint64_t tile_bytes() const {
    uint64_t n = 0;
    for (uint64_t b : bytes) n += b;
    return n;
  }
};
// What a file of `size` bytes takes of the card: whole clusters.
inline uint64_t on_card(uint64_t size, uint32_t cluster) {
  if (!cluster) cluster = CLUSTER_GUESS;
  return (size + cluster - 1) / cluster * cluster;
}
// The map a folder under tiles holds, by its name ("06_0", as path() makes it), or false for another name.
inline bool style_folder(const char *name, uint8_t &style) {
  if (std::strlen(name) != 4 || name[2] != '_' || name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' ||
      name[3] < '0' || name[3] > '9')
    return false;
  const int s = (name[0] - '0') * 10 + (name[1] - '0');
  if (s >= STYLES) return false;
  style = (uint8_t) s;
  return true;
}

// Each map's share of the tiles, the largest first: its name as the settings have it, as the menu shows it, its tiles
// and their bytes. Esri's light and dark streets are one map.
struct MapShare {
  const char *map, *label;
  uint32_t tiles;
  uint64_t bytes;
};
inline std::vector<MapShare> by_map(const Tally &t) {
  struct Name {
    uint8_t style;
    const char *map, *label;
  };
  static const Name NAMES[] = {{tiles::SRC_OSM, "osm", "OSM"},
                               {tiles::SRC_GRAY_LIGHT, "streets", "Streets"},
                               {tiles::SRC_GRAY_DARK, "streets", "Streets"},
                               {tiles::SRC_SATELLITE, "satellite", "Satellite"},
                               {tiles::SRC_TOPO, "topo", "Topo"},
                               {tiles::SRC_SECTIONAL, "sectional", "Sectional"},
                               {tiles::SRC_IFR_LOW, "ifr_low", "IFR low"},
                               {tiles::SRC_IFR_HIGH, "ifr_high", "IFR high"}};
  std::vector<MapShare> out;
  for (const Name &n : NAMES) {
    if (!t.tiles[n.style] && !t.bytes[n.style]) continue;
    auto same = std::find_if(out.begin(), out.end(), [&](const MapShare &m) { return !std::strcmp(m.map, n.map); });
    if (same == out.end()) {
      out.push_back({n.map, n.label, t.tiles[n.style], t.bytes[n.style]});
    } else {
      same->tiles += t.tiles[n.style];
      same->bytes += t.bytes[n.style];
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const MapShare &a, const MapShare &b) { return a.bytes > b.bytes; });
  return out;
}

// The card's space in four: the screen's tiles, its log, everything else (other files, and FAT's own tables), and
// what is free. A count from before files went makes no share larger than what is used.
struct Space {
  uint64_t tiles = 0, log = 0, other = 0, free = 0;
};
inline Space space_of(uint64_t total, uint64_t free, const Tally &t) {
  Space s;
  s.free = std::min(free, total);
  const uint64_t used = total - s.free;
  s.tiles = std::min(t.tile_bytes(), used);
  s.log = std::min(t.log_bytes, used - s.tiles);
  s.other = used - s.tiles - s.log;
  return s;
}
// The widths of a bar `px` wide shared out among `parts`, in proportion: one that is not nothing at least `least`
// pixels (where there is room), and together exactly `px`, the largest taking or giving what rounding leaves.
inline std::vector<int> widths(const std::vector<uint64_t> &parts, int px, int least = 2) {
  std::vector<int> out(parts.size(), 0);
  uint64_t sum = 0;
  for (uint64_t p : parts) sum += p;
  if (!sum || px <= 0) return out;
  int used = 0;
  size_t largest = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i] > parts[largest]) largest = i;
    if (!parts[i]) continue;
    out[i] = std::max(least, (int) ((double) parts[i] * px / sum + 0.5));
    used += out[i];
  }
  out[largest] = std::max(0, out[largest] + px - used);
  return out;
}

// ---- The menu's lines for the card (menu.cpp); the page has its own (page.h).

// "1.4 GB used of 29.7 GB".
inline std::string space_words(uint64_t total, uint64_t free) {
  return size_words(total - std::min(free, total)) + " used of " + size_words(total);
}
// Each map's tiles a line, the largest first: "Sectional: 12,345 tiles, 480 MB"; and the log's. "Counting..." before
// the first count is done.
inline std::string kept_words(const Tally &t) {
  if (!t.counted) return t.counting ? "Counting..." : "Not counted yet";
  std::string out;
  for (const MapShare &m : by_map(t))
    out += (out.empty() ? "" : "\n") + std::string(m.label) + ": " + count_words(m.tiles) + (m.tiles == 1 ? " tile, " : " tiles, ") +
           size_words(m.bytes);
  if (t.logs)
    out += (out.empty() ? "" : "\n") + std::string("Log: ") + count_words(t.logs) + (t.logs == 1 ? " day, " : " days, ") +
           size_words(t.log_bytes);
  return out.empty() ? "No tiles yet" : out;
}
// The tiles read from it and written to it since the start: "12 read, 340 written, 2 failed".
inline std::string since_words(uint32_t read, uint32_t written, uint32_t failed) {
  std::string out = count_words(read) + " read, " + count_words(written) + " written";
  if (failed) out += ", " + count_words(failed) + " failed";
  return out;
}
// A download under way: "Sectional: 215 of 860 tiles (25%)".
inline std::string download_words(uint8_t style, size_t next, size_t total) {
  const char *name = style == tiles::SRC_TOPO ? "Topo"
                   : style == tiles::SRC_IFR_LOW ? "IFR low"
                   : style == tiles::SRC_IFR_HIGH ? "IFR high"
                   : "Sectional";
  const size_t done = std::min(next, total);
  return std::string(name) + ": " + count_words(done) + " of " + count_words(total) + " tiles (" +
         std::to_string(total ? done * 100 / total : 0) + "%)";
}
// What the card is doing or why it is not there, or "" when all is well.
inline std::string note_words(bool mounted, const std::string &error, bool formatting, bool clearing,
                              uint32_t cleared) {
  if (formatting) return "Formatting the card (some seconds)...";
  std::string why = error;
  if (!why.empty()) why[0] = (char) std::toupper((unsigned char) why[0]);
  if (!mounted) return why.empty() ? "No map card." : "No map card: " + error + ".";
  if (clearing) return "Deleting the old tiles: " + count_words(cleared) + " files so far.";
  return why.empty() ? "" : why + ".";
}

}  // namespace sky::card
