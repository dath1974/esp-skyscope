// The parts of Skyscope that need no screen or network, on a PC: settings, the feed's JSON, the tile plan, which planes
// a frame shows and where, and the label placement. tests/run.sh builds and runs it; a reply saved from a feed can be
// given as an argument to read it too.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../components/skyscope/beam.h"
#include "../components/skyscope/feed.h"
#include "../components/skyscope/geo.h"
#include "../components/skyscope/overlay.h"
#include "../components/skyscope/plan.h"
#include "../components/skyscope/providers.h"
#include "../components/skyscope/settings.h"
#include "../components/skyscope/sprites.h"
#include "../components/skyscope/tiles.h"

using namespace sky;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static bool near(double a, double b, double within) { return std::fabs(a - b) <= within; }

static void test_geo() {
  // Tile 0/0/0's middle is lat 0, lon 0.
  const geo::Pixel p = geo::world(0, 0, 0);
  CHECK(near(p.x, 128, 1e-9) && near(p.y, 128, 1e-9));
  // The well-known tile of Heathrow at zoom 10: x 511, y 340.
  const geo::Pixel lhr = geo::world(51.47, -0.4543, 10);
  CHECK((int) (lhr.x / 256) == 510 && (int) (lhr.y / 256) == 340);
  const geo::View v(40.64, -73.78, 10, 600, 450);
  const geo::Pixel mid = v.point(40.64, -73.78);
  CHECK(near(mid.x, 300, 1e-6) && near(mid.y, 225, 1e-6));
  // A radius fills the short side.
  const double z = geo::zoom_for_radius(40.64, 20 * geo::NM, 450);
  const geo::View r(40.64, -73.78, z, 600, 450);
  CHECK(near(20 * geo::NM / r.metres_per_px(), 225, 0.5));
  CHECK(near(geo::metres_between(51.47, -0.4543, 40.64, -73.78) / 1000, 5540, 15));
}

static void test_settings() {
  Settings s;
  JsonDocument in;
  deserializeJson(in, R"({"lat":51.47,"lon":-0.45,"range_nm":30,"map":"sectional","watch":[{"id":"g-ezab","colour":"blue"},{"id":"GEZAB"}]})");
  std::string error;
  CHECK(apply(in.as<JsonObjectConst>(), s, error));
  CHECK(s.located && s.range_nm == 30 && s.map == SECTIONAL);
  CHECK(s.watch.size() == 1 && s.watch[0].id == "GEZAB" && s.watch[0].colour == 4);
  // A wrong key changes nothing and is named.
  JsonDocument bad;
  deserializeJson(bad, R"({"range_nm":10,"map":"nowhere"})");
  CHECK(!apply(bad.as<JsonObjectConst>(), s, error) && error == "map" && s.range_nm == 30);
  deserializeJson(bad, R"({"alt_min":20000,"alt_max":10000})");
  CHECK(!apply(bad.as<JsonObjectConst>(), s, error) && error == "alt_min");
  deserializeJson(bad, R"({"rotation":45})");
  CHECK(!apply(bad.as<JsonObjectConst>(), s, error) && error == "rotation");
  deserializeJson(bad, R"({"rotation":270})");
  CHECK(apply(bad.as<JsonObjectConst>(), s, error) && s.rotation == 270);
  deserializeJson(bad, R"({"custom_url":"ftp://x/{z}"})");
  CHECK(!apply(bad.as<JsonObjectConst>(), s, error) && error == "custom_url");
  // Round trip.
  JsonDocument out;
  to_json(s, out.to<JsonObject>());
  Settings back;
  CHECK(apply(out.as<JsonObjectConst>(), back, error));
  CHECK(back.lat == s.lat && back.map == s.map && back.watch.size() == 1 && back.tint == s.tint);
  // The stored JSON fits its slot (skyscope.cpp Stored) even with every watched plane.
  for (int i = 0; i < 12; ++i) s.watch.push_back({"N" + std::to_string(10000 + i), i % 6});
  s.custom_url = std::string("https://") + std::string(180, 'a') + "/{z}/{x}/{y}.png";
  JsonDocument full;
  to_json(s, full.to<JsonObject>());
  CHECK(measureJson(full) < 1800);
}

static const char *REPLY = R"({"ac":[
 {"hex":"A1B2C3","flight":"TEST12  ","r":"N12345","t":"B738","alt_baro":35000,"gs":450.5,"track":90,"baro_rate":-1500,"squawk":"1234","lat":40.70,"lon":-73.70,"seen_pos":0.4},
 {"hex":"abc001","flight":"GND1","alt_baro":"ground","gs":12,"track":180,"lat":40.64,"lon":-73.78},
 {"hex":"abc002","flight":"NOPOS","alt_baro":5000},
 {"hex":"abc003","flight":"STALE","alt_baro":5000,"lat":40.6,"lon":-73.8,"seen_pos":300},
 {"hex":"abc004","alt_baro":2500,"lat":40.66,"lon":-73.79,"squawk":"7700"}
],"msg":"No error","now":1}
)";

static void test_feed() {
  JsonDocument doc;
  std::vector<feed::Flight> flights;
  std::string error;
  CHECK(feed::parse(REPLY, std::strlen(REPLY), doc, flights, error));
  CHECK(flights.size() == 3);
  const feed::Flight &a = flights[0];
  CHECK(a.hex == "a1b2c3" && a.callsign == "TEST12" && a.has_altitude && a.altitude == 35000 && a.has_heading && a.climb == -1500);
  CHECK(flights[1].on_ground && !flights[1].has_altitude);
  CHECK(flights[2].emergency);
  // adsb.fi's list is `aircraft`.
  std::string fi = REPLY;
  fi.replace(fi.find("\"ac\""), 4, "\"aircraft\"");
  CHECK(feed::parse(fi.c_str(), fi.size(), doc, flights, error) && flights.size() == 3);
  CHECK(!feed::parse("{\"msg\":\"bad\"}", 13, doc, flights, error) && error == "bad");
  CHECK(!feed::parse("<html>", 6, doc, flights, error));
  // adsb.lol's announced limits: 500 a day is the tightest, one request every 172.8 s.
  CHECK(feed::min_interval_ms("500, 4;w=1, 60;w=60, 500;w=86400") == 172800);
  CHECK(feed::min_interval_ms("") == 0 && feed::min_interval_ms("100") == 0);
  CHECK(feed::min_interval_ms("10;w=1") == 100);
  // The countdown to the next request: m:ss below an hour and a half, hours and minutes above, "now" when due.
  CHECK(feed::countdown(1000 + 105000, 1000) == "1:45");
  CHECK(feed::countdown(26798000, 0) == "7 h 26 min");
  CHECK(feed::countdown(5000, 5000) == "now" && feed::countdown(4000, 5000) == "now");
  CHECK(feed::countdown(0x10, 0xFFFFFFF0u) == "0:01");  // across millis() wrapping
  CHECK(feed::url(ADSB_LOL, 40.64, -73.78, 400) == "https://api.adsb.lol/v2/point/40.6400/-73.7800/250");
  CHECK(feed::url(ADSB_FI, 1, 2, 5) == "https://opendata.adsb.fi/api/v2/lat/1.0000/lon/2.0000/dist/5");
}

static void test_tiles() {
  Settings s;
  s.lat = 40.64;
  s.lon = -73.78;
  s.located = true;
  s.range_nm = 20;
  s.map = DARK_GRAY;
  tiles::Plan p = tiles::plan(s, 600, 450, 2);
  CHECK(p.view.zoom == std::floor(p.view.zoom));
  CHECK(p.tile_z == (int) p.view.zoom - 1);
  CHECK(!p.keys.empty() && std::count_if(p.keys.begin(), p.keys.end(), [](const tiles::Key &k) { return k.layer == 0; }) <= 9);
  // Every pixel of the frame is under one of the tiles.
  for (int y = 0; y < 450; y += 449)
    for (int x = 0; x < 600; x += 599) {
      bool covered = false;
      for (const auto &k : p.keys) {
        const geo::TileOrigin o = geo::tile_origin(p.view, k.z, k.x, k.y);
        covered |= x >= o.x && x < o.x + 256 * o.scale && y >= o.y && y < o.y + 256 * o.scale;
      }
      CHECK(covered);
    }
  // The middle's tile first.
  const geo::TileOrigin first = geo::tile_origin(p.view, p.keys[0].z, p.keys[0].x, p.keys[0].y);
  CHECK(300 >= first.x && 300 < first.x + 256 * first.scale);
  // ArcGIS orders a tile's address {z}/{y}/{x}.
  CHECK(tiles::url(s, p.keys[0]) == "https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/" +
                                         std::to_string(p.keys[0].z) + "/" + std::to_string(p.keys[0].y) + "/" + std::to_string(p.keys[0].x));
  // Place names: each map tile is followed by its labels tile, the same square; off, the map alone.
  s.place_names = true;
  p = tiles::plan(s, 600, 450, 2);
  CHECK(p.keys.size() % 2 == 0 && p.keys[0].layer == 0 && p.keys[1].layer == 1);
  CHECK(p.keys[1].x == p.keys[0].x && p.keys[1].y == p.keys[0].y && p.keys[1].z == p.keys[0].z);
  CHECK(tiles::url(s, p.keys[1]).find("World_Dark_Gray_Reference") != std::string::npos);
  CHECK(!(p.keys[0] == p.keys[1]));
  const size_t with_names = p.keys.size();
  s.place_names = false;
  p = tiles::plan(s, 600, 450, 2);
  CHECK(p.keys.size() * 2 == with_names);
  for (const auto &k : p.keys) CHECK(k.layer == 0);
  // A map without a layer of names takes none, the setting on or not.
  s.place_names = true;
  s.map = OSM;
  p = tiles::plan(s, 600, 450, 2);
  for (const auto &k : p.keys) CHECK(k.layer == 0);
  // The credits: the map's own, and over the imagery its names' sources too.
  s.map = SATELLITE;
  CHECK(tiles::credit(s) == "Esri, Vantor, Earthstar Geographics, HERE, Garmin, \xC2\xA9 OpenStreetMap");
  s.place_names = false;
  CHECK(tiles::credit(s) == "Esri, Vantor, Earthstar Geographics");
  s.place_names = true;
  s.map = DARK_GRAY;
  CHECK(tiles::credit(s) == "Esri, HERE, Garmin, \xC2\xA9 OpenStreetMap");
  // A screen that kept CARTO's style takes Esri's in its place.
  Settings old;
  JsonDocument carto;
  deserializeJson(carto, R"({"map":"carto_light"})");
  std::string why;
  CHECK(apply(carto.as<JsonObjectConst>(), old, why) && old.map == LIGHT_GRAY);
  // A chart is drawn from its sharpest zoom, enlarged, and not at all far out.
  s.map = SECTIONAL;
  s.range_nm = 3;
  p = tiles::plan(s, 600, 450, 2);
  CHECK(p.tile_z == 11);
  // At 6 NM the view is zoom 11: the sectional's own level 11 at its pixels, not level 10 enlarged (a scan's words are
  // the chart's, and the server's resampling is smooth).
  s.range_nm = 6;
  p = tiles::plan(s, 450, 600, 2);
  CHECK(p.view.zoom == 11 && p.tile_z == 11);
  // A drawn map at the same view still takes a level out, enlarged, so its words can be read.
  s.map = TOPO;
  p = tiles::plan(s, 450, 600, 2);
  CHECK(p.tile_z == 10);
  s.map = SECTIONAL;
  s.range_nm = 250;
  p = tiles::plan(s, 600, 450, 2);
  CHECK(p.tile_z == -1 && p.keys.empty());
  // The radar is drawn from nothing, at the exact range.
  s.map = RADAR;
  s.range_nm = 20;
  p = tiles::plan(s, 600, 450, 2);
  CHECK(p.keys.empty() && near(20 * geo::NM / p.view.metres_per_px(), 225, 0.5));
  s.map = CUSTOM;
  s.custom_url = "https://t.example/{z}/{x}/{y}.png";
  CHECK(tiles::url(s, {CUSTOM, 3, 1, 2}) == "https://t.example/3/1/2.png");
  const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, jpg[] = {0xFF, 0xD8, 0xFF};
  CHECK(tiles::format_of(png, 8) == tiles::PNG && tiles::format_of(jpg, 3) == tiles::JPEG);
  CHECK(tiles::rgb565(255, 255, 255) == 0xFFFF && tiles::rgb565(255, 0, 0) == 0xF800);
  // Inverted, white paper goes black and black ink white.
  const tiles::Filter night{FILTER_INVERT, 256};
  CHECK(tiles::paint(night, 255, 255, 255) == 0 && tiles::paint(night, 0, 0, 0) == 0xFFFF);
}

static void test_overlay() {
  JsonDocument doc;
  std::vector<feed::Flight> flights;
  std::string error;
  feed::parse(REPLY, std::strlen(REPLY), doc, flights, error);
  Settings s;
  s.lat = 40.64;
  s.lon = -73.78;
  s.range_nm = 20;
  s.map = RADAR;
  tiles::Plan p = tiles::plan(s, 600, 450, 1);
  overlay::Tracks tracks;
  tracks.update(flights, 1000);
  // Ground traffic is hidden unless asked for.
  plan::Overlay o = overlay::build(flights, s, p.view, tracks, 1000, 12.25f, true);
  CHECK(o.planes.size() == 2);
  s.ground = true;
  o = overlay::build(flights, s, p.view, tracks, 1000, 12.25f, true);
  CHECK(o.planes.size() == 3);
  // The closest to the middle first; the one on the airport is in the middle.
  CHECK(o.planes[0].id == "abc001" && std::abs(o.planes[0].x - 300) <= 1 && std::abs(o.planes[0].y - 225) <= 1);
  // East at 450 kt: about 7.5 NM a minute, to the right.
  const auto &east = *std::find_if(o.planes.begin(), o.planes.end(), [](const plan::Plane &q) { return q.id == "a1b2c3"; });
  CHECK(east.vx > 0 && std::abs(east.vy) <= 1);
  CHECK(near(east.vx / (225.0 / 20), 7.5, 0.3));
  CHECK(east.band == 4);
  // Watched planes come last and always show.
  s.watch = {{"N12345", 2}};
  s.alt_max = 10000;
  o = overlay::build(flights, s, p.view, tracks, 1000, 12.25f, true);
  CHECK(o.planes.back().id == "a1b2c3" && o.planes.back().watch == 3);
  // A track is the places it was.
  flights[0].lat += 0.01;
  tracks.update(flights, 11000);
  s.track_s = 60;
  o = overlay::build(flights, s, p.view, tracks, 11000, 12.25f, true);
  CHECK(o.planes.back().track.size() == 4);
  CHECK(overlay::label_of(flights[0], LABEL_FULL, false) == "TEST12 B738\nFL350 451 kt" ||
        overlay::label_of(flights[0], LABEL_FULL, false) == "TEST12 B738\nFL350 450 kt");
  CHECK(overlay::label_of(flights[2], LABEL_CALLSIGN, false) == "ABC004");
  CHECK(overlay::altitude_text(flights[2], false) == "2500 ft" && overlay::altitude_text(flights[2], true) == "760 m");
  // Gone from the feed for two minutes: forgotten.
  tracks.update({}, 11000 + overlay::FORGET_MS + 1);
  CHECK(tracks.size() == 0);
}

static void test_sprites() {
  // A plane pointing north: its nose at the top, solid in the middle, nothing in the corners.
  const sprites::Mask m = sprites::render(30, 0);
  CHECK(m.size == 33 && m.alpha.size() == 33u * 33u);
  auto at = [&](int x, int y) { return m.alpha[(size_t) y * m.size + x]; };
  CHECK(at(16, 16) == 255 && at(0, 0) == 0 && at(32, 32) == 0);
  CHECK(at(16, 3) > 0 && at(16, 30) > 0);  // nose and tail
  // Turned east, the nose is on the right.
  const sprites::Mask e = sprites::render(30, 90 / sprites::HEADING_STEP);
  CHECK(e.alpha[16 * e.size + 30] > 0 && e.alpha[3 * e.size + 16] == 0);
  CHECK(sprites::heading_step(359) == 0 && sprites::heading_step(2) == 1 && sprites::heading_step(-1) == 0);
  sprites::Cache<int> cache;
  cache.get(30, 90);
  cache.get(30, 91);
  cache.get(31, 90);
  CHECK(cache.size() == 2);
  // Past a frame's budget, a heading not made yet borrows the nearest made one of its length.
  CHECK(cache.get(30, 180, 7, 1) != nullptr && cache.size() == 3);
  auto *borrowed = cache.get(30, 270, 7, 1);
  CHECK(cache.size() == 3 && borrowed == cache.get(30, 180, 7, 1));
  CHECK(cache.get(30, 270, 8, 1) != borrowed && cache.size() == 4);  // the next frame makes it
}

static void test_beam() {
  // A 101 x 101 scope round its middle, a grey ground; the beam at 90 degrees (east), having come from 85.
  const int n = 101;
  std::vector<uint16_t> canvas(n * n, 0x1234), ground(n * n, 0x1234);
  beam::Painter p;
  p.canvas = canvas.data();
  p.background = ground.data();
  p.width = p.height = n;
  p.cx = p.cy = n / 2.0f;
  p.reach = 48;
  p.beam_width = 3;
  p.set_tint(colours::tint(0));
  // Something left behind the trail's end: cleaned within the band this step swept (to 18 degrees back), left beyond.
  auto at = [&](float bearing, float r) {
    const float a = bearing * 3.14159265f / 180.0f;
    return (int) std::floor(p.cy - std::cos(a) * r) * n + (int) std::floor(p.cx + std::sin(a) * r);
  };
  canvas[at(74, 30)] = 0xFFFF;
  canvas[at(60, 30)] = 0xFFFF;
  p.paint(85, 90, {0, 0, n, n});
  CHECK(canvas[at(90, 30)] == p.beam);                        // on the beam
  const uint16_t trail = canvas[at(85, 30)];                 // 5 degrees behind: the afterglow
  CHECK(trail != 0x1234 && trail != p.beam);
  CHECK(canvas[at(120, 30)] == 0x1234);                      // ahead: untouched
  CHECK(canvas[at(74, 30)] == 0x1234);                       // 16 degrees behind, past the trail: clean again
  CHECK(canvas[at(60, 30)] == 0xFFFF);                       // 30 degrees behind, cleaned by an earlier step: left
  CHECK(canvas[at(270, 30)] == 0x1234);                      // the far side: untouched
  // The afterglow fades: nearer the beam is brighter (more of the glow's green).
  const uint16_t near_beam = canvas[at(88, 30)], far_back = canvas[at(81, 30)];
  CHECK((near_beam >> 5 & 0x3F) > (far_back >> 5 & 0x3F));
}

static void test_providers() {
  providers::Providers p;
  p.set_order(ADSB_FI, true);
  Feed f;
  uint32_t wait = 0;
  CHECK(p.choose(0, f, wait) && f == ADSB_FI);
  // A good answer: the refresh asked for.
  CHECK(p.succeeded(ADSB_FI, 0, 10000, {200}) == 10000);
  // adsb.fi refuses with the end of its quota: left alone that long, and adsb.lol is asked meanwhile.
  providers::Answer limited{429, 0, 26798, "500, 4;w=1, 60;w=60, 500;w=86400", "HTTP 429"};
  CHECK(p.failed(ADSB_FI, 1000, limited) == 26798u * 1000 + 5000);
  CHECK(p.state(ADSB_FI).error == "limit reached" && p.state(ADSB_FI).failures == 1);
  CHECK(p.choose(2000, f, wait) && f == ADSB_LOL);
  // adsb.lol fails too, with no word on when: 15 s, then 30, 60 ... at most 15 minutes; meanwhile nothing is free.
  providers::Answer down{0, 0, 0, "", "ESP_ERR_HTTP_CONNECT"};
  CHECK(p.failed(ADSB_LOL, 2000, down) == 15000);
  CHECK(!p.choose(3000, f, wait) && wait == 14000 && p.all_blocked(3000));
  CHECK(p.choose(17000, f, wait) && f == ADSB_LOL);  // its 15 s are up: asked again
  CHECK(p.failed(ADSB_LOL, 17000, down) == 30000);
  CHECK(p.failed(ADSB_LOL, 47000, down) == 60000);
  for (int i = 0; i < 10; ++i) p.failed(ADSB_LOL, 107000, down);
  CHECK(p.state(ADSB_LOL).until - 107000 == providers::FAILED_MOST_MS);
  // A refusal with no word on when: 2, 4, 8 minutes ... at most 30. Retry-After wins over doubling.
  providers::Providers q;
  providers::Answer refused{429, 0, 0, "", "HTTP 429"};
  CHECK(q.failed(ADSB_LOL, 0, refused) == 120000 && q.failed(ADSB_LOL, 0, refused) == 240000);
  for (int i = 0; i < 10; ++i) q.failed(ADSB_LOL, 0, refused);
  CHECK(q.state(ADSB_LOL).until == providers::REFUSED_MOST_MS);
  CHECK(q.failed(ADSB_LOL, 0, {429, 90, 0, "", ""}) == 90000);
  // A good answer clears the record.
  q.succeeded(ADSB_LOL, 0, 10000, {200});
  CHECK(!q.state(ADSB_LOL).blocked && q.state(ADSB_LOL).failures == 0);
  // Back to the preferred one once its quota is back.
  CHECK(p.choose(1000 + 26798u * 1000 + 5000, f, wait) && f == ADSB_FI);
  // An announced pace holds until the quota's end, then the refresh asked for again.
  providers::Providers r;
  CHECK(r.succeeded(ADSB_LOL, 0, 10000, {200, 0, 600, "500, 500;w=86400", ""}) == 172800);
  CHECK(r.succeeded(ADSB_LOL, 300000, 10000, {200}) == 172800);   // still within its 600 s
  CHECK(r.succeeded(ADSB_LOL, 700000, 10000, {200}) == 10000);    // past it
  // Without falling back, a source that waits is waited for.
  providers::Providers alone;
  alone.set_order(ADSB_LOL, false);
  alone.failed(ADSB_LOL, 0, down);
  CHECK(!alone.choose(1000, f, wait) && wait == 14000);
  // Across millis() wrapping.
  providers::Providers wrap;
  wrap.failed(ADSB_FI, 0xFFFFF000u, down);
  CHECK(!wrap.choose(0xFFFFF000u + 1000, f, wait) || f == ADSB_LOL);
  wrap.set_order(ADSB_FI, false);
  CHECK(!wrap.choose(0x00001000u, f, wait) && wait == 15000 - 0x2000);
  CHECK(wrap.choose(0xFFFFF000u + 15000, f, wait) && f == ADSB_FI);
}

static void test_labels() {
  std::vector<plan::Label> labels(3);
  labels[0] = {{100, 100}, 6, 60, 20, false};
  labels[1] = {{104, 100}, 6, 60, 20, false};
  labels[2] = {{590, 100}, 6, 60, 20, true};
  const auto placed = plan::place_labels(labels, 600, 450, 4, 20);
  CHECK(placed[0].shown && placed[1].shown && placed[2].shown);
  CHECK(!plan::overlaps(placed[0].box, placed[1].box));
  CHECK(placed[2].box.x + placed[2].box.w <= 600);
}

int main(int argc, char **argv) {
  test_geo();
  test_settings();
  test_feed();
  test_tiles();
  test_overlay();
  test_labels();
  test_sprites();
  test_beam();
  test_providers();
  for (int i = 1; i < argc; ++i) {
    std::ifstream file(argv[i]);
    std::stringstream text;
    text << file.rdbuf();
    const std::string reply = text.str();
    JsonDocument doc;
    std::vector<feed::Flight> flights;
    std::string error;
    const bool ok = feed::parse(reply.c_str(), reply.size(), doc, flights, error);
    std::printf("%s: %s, %zu aircraft with a position\n", argv[i], ok ? "read" : error.c_str(), flights.size());
    CHECK(ok && !flights.empty());
  }
  std::printf(failures ? "%d FAILED\n" : "All passed\n", failures);
  return failures ? 1 : 0;
}
