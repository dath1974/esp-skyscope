// The parts of Skyscope that need no screen or network, on a PC: settings, the feed's JSON, the tile plan, which planes
// a frame shows and where, and the label placement. tests/run.sh builds and runs it; a reply saved from a feed can be
// given as an argument to read it too.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../components/skyscope/adsb.h"
#include "../components/skyscope/airspace.h"
#include "../components/skyscope/beam.h"
#include "../components/skyscope/card.h"
#include "../components/skyscope/feed.h"
#include "../components/skyscope/firmware.h"
#include "../components/skyscope/geo.h"
#include "../components/skyscope/glass.h"
#include "../components/skyscope/http.h"
#include "../components/skyscope/pinch.h"
#include "../components/skyscope/keyboard.h"
#include "../components/skyscope/menu_rows.h"
#include "../components/skyscope/modes.h"
#include "../components/skyscope/offline.h"
#include "../components/skyscope/overlay.h"
#include "../components/skyscope/pixels.h"
#include "../components/skyscope/places.h"
#include "../components/skyscope/plan.h"
#include "../components/skyscope/providers.h"
#include "../components/skyscope/radar.h"
#include "../components/skyscope/schedule.h"
#include "../components/skyscope/settings.h"
#include "../components/skyscope/sprites.h"
#include "../components/skyscope/swipe.h"
#include "../components/skyscope/tiles.h"
#include "../components/skyscope/touch.h"
#include "../components/skyscope/weather.h"
#include "../components/skyscope/wifi_list.h"

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
  // place() is point() the other way.
  const geo::Place back = r.place(17, 431);
  const geo::Pixel there = r.point(back.lat, back.lon);
  CHECK(near(there.x, 17, 1e-6) && near(there.y, 431, 1e-6));
  CHECK(near(r.place(300, 225).lat, 40.64, 1e-9) && near(r.place(300, 225).lon, -73.78, 1e-9));
}

static void test_scope() {
  // The radar's scope is on unless switched off, and its beam turns only round it, on the radar. A screen starts on
  // the light street map, not the radar.
  Settings s;
  CHECK(s.map == STREETS && s.theme == LIGHT && !scope_shown(s) && !sweeps(s));
  s.map = RADAR;
  CHECK(s.scope && s.sweep && scope_shown(s) && sweeps(s));
  s.scope = false;
  CHECK(!scope_shown(s) && !sweeps(s));
  s.scope = true;
  s.sweep = false;
  CHECK(scope_shown(s) && !sweeps(s));
  s.sweep = true;
  s.map = STREETS;
  CHECK(!scope_shown(s) && !sweeps(s));
  JsonDocument in;
  deserializeJson(in, R"({"scope":false})");
  std::string error;
  CHECK(apply(in.as<JsonObjectConst>(), s, error) && !s.scope);
  deserializeJson(in, R"({"scope":"off"})");
  CHECK(!apply(in.as<JsonObjectConst>(), s, error) && error == "scope");
  JsonDocument out;
  to_json(s, out.to<JsonObject>());
  CHECK(out["scope"] == false);
}

static void test_airspace() {
  // What a frame asks for: its box with a margin, Class B, C and D, and runways only up to RUNWAY_RANGE_NM.
  Settings s;
  s.lat = 42.36;
  s.lon = -71.01;
  s.range_nm = 30;
  const tiles::Plan p = tiles::plan(s, 450, 600, 1);
  const airspace::Box b = airspace::box_of(p.view);
  CHECK(b.south < 42.36 - 0.5 && b.north > 42.36 + 0.5 && b.west < -71.01 - 0.5 && b.east > -71.01 + 0.5);
  CHECK(b.north - b.south < 2.5);
  const airspace::Ask ask = airspace::ask(p.view, s.range_nm);
  CHECK(ask.airspace.find("Class_Airspace/FeatureServer/0/query?where=CLASS%20IN%20(%27B%27,%27C%27,%27D%27)") !=
        std::string::npos);
  CHECK(ask.airspace.find("&maxAllowableOffset=0.0") != std::string::npos);
  CHECK(ask.airspace.find("&f=json") != std::string::npos);
  CHECK(ask.runways.find("Runways/FeatureServer/0/query?where=LENGTH%3E%3D2500&") != std::string::npos);
  CHECK(ask.runways.find("maxAllowableOffset") == std::string::npos);
  CHECK(airspace::ask(p.view, 81).runways.empty());
  // The same frame asks the same way: the address is what a reply is kept by.
  CHECK(airspace::ask(tiles::plan(s, 450, 600, 1).view, s.range_nm).airspace == ask.airspace);

  // A reply of airspace, as ADDS writes it (trimmed): a Class B with a hole, a Class D, and what is passed over: a
  // Class E, a feature with no shape, a point with a height.
  const std::string areas = R"({"objectIdFieldName":"OBJECTID","geometryType":"esriGeometryPolygon","fields":[{"name":"CLASS"}],
    "features":[
     {"attributes":{"CLASS":"B","ADHP_ID":"8A175D7D-C058-4D14-A55F-D354936393D2"},
      "geometry":{"rings":[[[-71.2,42.2],[-70.8,42.2],[-70.8,42.5],[-71.2,42.5],[-71.2,42.2]],[[-71.1,42.3],[-71.0,42.3],[-71.0,42.4,0],[-71.1,42.3]]]}},
     {"attributes":{"CLASS":"D","ADHP_ID":"518881FD-886B-407F-BEF1-E199641186ED"},
      "geometry":{"rings":[[[-71.9,42.2],[-71.8,42.3],[-71.9,42.3],[-71.9,42.2]]]}},
     {"attributes":{"CLASS":"E","ADHP_ID":"00000000-0000-0000-0000-000000000000"},"geometry":{"rings":[[[-71,42],[-70,42],[-71,42]]]}},
     {"attributes":{"CLASS":"C"}}
    ],"exceededTransferLimit":true})";
  airspace::Map map;
  std::vector<uint32_t> airports;
  JsonDocument doc;
  std::string error;
  CHECK(airspace::read_airspace(areas.c_str(), areas.size(), doc, map, airports, error));
  CHECK(map.areas.size() == 3 && map.points.size() == 13 && map.cut && airports.size() == 2);
  CHECK(map.areas[0].cls == airspace::CLASS_B && map.areas[0].count == 5 && map.areas[1].first == 5);
  CHECK(map.areas[2].cls == airspace::CLASS_D);
  CHECK(near(map.points[9].lat, 42.2, 1e-5) && near(map.points[9].lon, -71.9, 1e-5));

  // Runways: Boston's 15R/33L as its outline, the line down its middle from end to end; at an airport of the
  // airspace's, so towered. Another airport's is not.
  const std::string runways = R"({"features":[
     {"attributes":{"AIRPORT_ID":"8A175D7D-C058-4D14-A55F-D354936393D2","LENGTH":10083},
      "geometry":{"rings":[[[-70.99178,42.35451],[-71.01809,42.37413],[-71.0177,42.37442],[-70.99139,42.3548],[-70.99178,42.35451]]]}},
     {"attributes":{"AIRPORT_ID":"20CEC5FA-A9B3-4C3A-AE66-654BD00F2200","LENGTH":3654},
      "geometry":{"rings":[[[-71.3,42.1],[-71.3,42.11],[-71.2999,42.11],[-71.2999,42.1],[-71.3,42.1]]]}}]})";
  CHECK(airspace::read_runways(runways.c_str(), runways.size(), doc, airports, map, error));
  CHECK(map.runways.size() == 2);
  const airspace::Runway &r = map.runways[0];
  CHECK(r.towered && r.length_ft == 10083 && airspace::lengthened(r));
  CHECK(near(geo::metres_between(r.a.lat, r.a.lon, r.b.lat, r.b.lon) / 0.3048, 10083, 60));
  const double mid_lat = (r.a.lat + r.b.lat) / 2, mid_lon = (r.a.lon + r.b.lon) / 2;
  CHECK(near(mid_lat, 42.364465, 1e-4) && near(mid_lon, -71.00474, 1e-4));
  CHECK(std::min(r.a.lat, r.b.lat) < 42.3548 && std::max(r.a.lat, r.b.lat) > 42.3741);
  CHECK(!map.runways[1].towered && !airspace::lengthened(map.runways[1]));
  CHECK(near(geo::metres_between(map.runways[1].a.lat, map.runways[1].a.lon, map.runways[1].b.lat, map.runways[1].b.lon),
             1112, 5));

  // The server's own word for a bad query; a reply that is no JSON, or cut short.
  const std::string refused = R"({"error":{"code":400,"message":"Cannot perform query. Invalid query parameters.","details":[]}})";
  airspace::Map none;
  CHECK(!airspace::read_airspace(refused.c_str(), refused.size(), doc, none, airports, error));
  CHECK(error == "ADDS: Cannot perform query. Invalid query parameters.");
  for (const std::string &bad : {std::string("<html>"), std::string(""), areas.substr(0, areas.size() / 2),
                                std::string(R"({"features":[{"attributes":{"CLASS":"B"},"geometry":{"rings":[[["x",1]]]}}]})")}) {
    error.clear();
    CHECK(!airspace::read_airspace(bad.c_str(), bad.size(), doc, none, airports, error) && !error.empty());
  }

  // Lines straight into the frame: covered along the line and across its width, nothing past its ends or beside it,
  // smoothed at its edges; a line far off the frame, or through it from far off, is cut to it.
  {
    const int w = 60, h = 40;
    std::vector<uint16_t> f(w * h, 0);
    const uint16_t white = pixels::rgb565(0xFFFFFF);
    CHECK(white == 0xFFFF && pixels::rgb565(0xFF0000) == 0xF800 && pixels::rgb565(0x0000FF) == 0x001F);
    pixels::line(f.data(), w, h, 10, 10, 40, 10, white, 2);
    CHECK(f[10 * w + 25] == white && f[10 * w + 10] == white && f[10 * w + 40] == white);
    CHECK(f[13 * w + 25] == 0 && f[10 * w + 43] == 0 && f[10 * w + 6] == 0);
    CHECK(f[11 * w + 25] != 0 && f[12 * w + 25] != white);  // a smoothed edge, not a hard one
    pixels::line(f.data(), w, h, 5, 30, 25, 20, white, 1);
    CHECK(f[25 * w + 15] != 0);
    std::fill(f.begin(), f.end(), 0);
    pixels::line(f.data(), w, h, -1e9f, -5e8f, 1e9f, 5e8f, white, 3);
    pixels::line(f.data(), w, h, -1e9f, 20, 1e9f, 20, white, 1);
    pixels::line(f.data(), w, h, 1e6f, 1e6f, 2e6f, 2e6f, white, 1);
    CHECK(f[20 * w + 0] == white && f[20 * w + 59] == white && f[0] == white && f[39 * w] == 0 && f[39 * w + 59] == 0);
  }
  // Dashes: as many as fit, the pattern carried from one piece to the next; never stuck, whatever the pattern's phase.
  {
    int count = 0;
    double phase = 0, drawn = 0;
    auto on = [&](geo::Pixel a, geo::Pixel b) { ++count, drawn += b.x - a.x; };
    airspace::detail::dashes({0, 0}, {100, 0}, 6, 4, phase, on);
    CHECK(count == 10 && near(drawn, 60, 1e-9) && near(phase, 0, 1e-9));
    count = 0;
    airspace::detail::dashes({0, 0}, {3, 0}, 6, 4, phase, on);
    airspace::detail::dashes({3, 0}, {13, 0}, 6, 4, phase, on);
    CHECK(count == 3 && near(phase, 3, 1e-9));  // 0-3, 3-6, then 10-13
    for (int i = 0; i < 20000; ++i) {
      double p = std::nextafter(6.0 * (i % 7), 0.0) + i * 1e-13;
      airspace::detail::dashes({0, 0}, {900 + i * 0.37, 20 + i * 0.11}, 9, 6, p, [](geo::Pixel, geo::Pixel) {});
    }
  }
  // The map drawn: the Class B's edge in its colour, the D's in dashes, the long towered runway and its centerline
  // out past its end; nothing in the middle of an area.
  {
    Settings at;
    at.lat = 42.36;
    at.lon = -71.01;
    at.range_nm = 30;
    const geo::View v = tiles::plan(at, 450, 600, 1).view;
    std::vector<uint16_t> f(450 * 600, 0);
    const airspace::Ink ink{0xF800, 0x07E0, 0x001F, 0xFFFF, 0x8410, 1, 2, 9, 6};
    airspace::draw(f.data(), v, map, ink);
    auto at_place = [&](double lat, double lon) {
      const geo::Pixel p = v.point(lat, lon);
      return f[(int) std::lround(p.y) * 450 + (int) std::lround(p.x)];
    };
    CHECK(at_place(42.2, -71.0) == 0xF800);  // the Class B's south edge
    CHECK(at_place(42.45, -70.95) == 0);     // inside it, between its edge and its hole
    CHECK(at_place(42.364465, -71.00474) == 0xFFFF);  // the runway's middle
    const geo::Pixel a = v.point(r.a.lat, r.a.lon), b = v.point(r.b.lat, r.b.lon);
    const double len = std::hypot(b.x - a.x, b.y - a.y), nm = geo::NM / v.metres_per_px();
    int lit = 0;
    for (double d = 1; d < 9 * nm; d += 1) {
      const int x = (int) std::lround(b.x + (b.x - a.x) / len * d), y = (int) std::lround(b.y + (b.y - a.y) / len * d);
      if (x >= 0 && x < 450 && y >= 0 && y < 600) lit += f[y * 450 + x] != 0;
    }
    CHECK(lit > 3 * nm && lit < 6 * nm);  // dashed: about half of it
  }
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
  deserializeJson(bad, R"({"sleep_min":-1})");
  CHECK(!apply(bad.as<JsonObjectConst>(), s, error) && error == "sleep_min");
  deserializeJson(bad, R"({"sleep_min":15})");
  CHECK(apply(bad.as<JsonObjectConst>(), s, error) && s.sleep_min == 15);
  deserializeJson(bad, R"({"place":"KJFK"})");
  CHECK(apply(bad.as<JsonObjectConst>(), s, error) && s.place == "KJFK");
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
  s.receiver_url = std::string("http://") + std::string(150, 'r') + ".json";
  JsonDocument full;
  to_json(s, full.to<JsonObject>());
  CHECK(measureJson(full) < 1800);
  // A receiver's address: http or https, not too long; empty is none.
  {
    Settings r;
    JsonDocument change;
    change["receiver_url"] = "ftp://receiver/aircraft.json";
    CHECK(!apply(change.as<JsonObjectConst>(), r, error) && error == "receiver_url");
    change["receiver_url"] = "http://" + std::string(160, 'r');
    CHECK(!apply(change.as<JsonObjectConst>(), r, error));
    change["receiver_url"] = "http://192.0.2.10:8080/data/aircraft.json";
    change["feed"] = "receiver";
    CHECK(apply(change.as<JsonObjectConst>(), r, error) && r.feed == RECEIVER);
  }
}

// Off a network, a screen with a radio shows the radar from it alone, after a while; back on one, at once.
static void test_offline() {
  using offline::AFTER_MS;
  {
    offline::Tracker t;
    CHECK(!t.update(true, false, 1000));  // the start, no network yet
    CHECK(!t.update(true, false, 1000 + AFTER_MS - 1));
    CHECK(t.update(true, false, 1000 + AFTER_MS));
    CHECK(!t.update(true, true, 1000 + AFTER_MS + 5));  // the network back: online at once
    // A blip does not take the map away, and the wait starts again from the next loss.
    CHECK(!t.update(true, false, 100000));
    CHECK(!t.update(true, true, 100000 + AFTER_MS / 2));
    CHECK(!t.update(true, false, 100000 + AFTER_MS));
    CHECK(!t.update(true, false, 100000 + 2 * AFTER_MS - 1));
    CHECK(t.update(true, false, 100000 + 2 * AFTER_MS));
  }
  {
    // A screen without a radio never runs offline: it has nothing to show without the network.
    offline::Tracker t;
    CHECK(!t.update(false, false, 0) && !t.update(false, false, 10 * AFTER_MS) && !t.offline());
  }
  {
    // millis() wrapping round counts as time going on.
    offline::Tracker t;
    CHECK(!t.update(true, false, UINT32_MAX - 100));
    CHECK(t.update(true, false, AFTER_MS));
  }
  Settings s;
  s.map = SECTIONAL;
  s.feed = ADSB_LOL;
  s.fallback = true;
  s.airspace = s.radar_overlay = s.radar_loop = true;
  s.tint = AMBER;
  const Settings o = offline::shown(s);
  CHECK(o.map == RADAR && o.feed == RADIO && !o.fallback && !o.airspace && !o.radar_overlay && !o.radar_loop);
  CHECK(o.tint == AMBER && s.map == SECTIONAL);  // the rest as set, and the settings themselves kept
  CHECK(!tiles::has_tiles(o.map) && tiles::plan(o, 400, 400, 1).keys.empty());
  // With a map card the map stays, drawn from it; a custom server's never is.
  const Settings c = offline::shown(s, true);
  CHECK(c.map == SECTIONAL && c.feed == RADIO && !c.airspace && !c.radar_overlay);
  s.map = CUSTOM;
  CHECK(offline::shown(s, true).map == RADAR);
}

// The map card: where a tile goes, how long it is good for, and which tiles an area takes.
static void test_card() {
  using namespace tiles;
  const Key k{SRC_SECTIONAL, 11, 534, 779};
  CHECK(card::path(k) == "/sd/tiles/06_0/11/534/779.t");
  Key names{SRC_SATELLITE, 19, 524287, 300000, 1};
  CHECK(card::path(names) == "/sd/tiles/04_1/19/524287/300000.t");
  // Every part of a path fits 8.3, as the card's FAT has no long names.
  {
    const std::string p = card::path(names);
    size_t from = 1;
    while (from < p.size()) {
      size_t to = p.find('/', from);
      if (to == std::string::npos) to = p.size();
      const std::string part = p.substr(from, to - from);
      const size_t dot = part.find('.');
      CHECK((dot == std::string::npos ? part.size() : dot) <= 8);
      CHECK(dot == std::string::npos || part.size() - dot - 1 <= 3);
      from = to + 1;
    }
  }
  // The one file every write goes through, and the trash's folders: all 8.3 too.
  CHECK(card::temp_path() == "/sd/tiles/write.tmp");
  CHECK(card::trash_name(0x12345678) == "/sd/trash/t2345678" && card::trash_name(7) == "/sd/trash/t0000007");
  const std::vector<std::string> f = card::folders(k);
  CHECK(f.size() == 4 && f[0] == "/sd/tiles" && f[1] == "/sd/tiles/06_0" && f[3] == "/sd/tiles/06_0/11/534");
  // What is kept: maps and their names, not the weather radar nor a custom server.
  CHECK(card::cacheable(k) && card::cacheable(names));
  Key radar = k;
  radar.layer = 2;
  CHECK(!card::cacheable(radar));
  CHECK(!card::cacheable(Key{SRC_CUSTOM, 5, 1, 1}) && !card::cacheable(Key{SRC_NONE, 5, 1, 1}));
  // Downloaded by area: the FAA's charts and USGS only.
  CHECK(card::may_download(SRC_SECTIONAL) && card::may_download(SRC_IFR_LOW) && card::may_download(SRC_IFR_HIGH));
  CHECK(card::may_download(SRC_TOPO) && !card::may_download(SRC_OSM) && !card::may_download(SRC_SATELLITE));
  // How long: the charts and topo by their own pace, the others as their server says.
  CHECK(card::lifetime_s(SRC_SECTIONAL, 60) == 28 * card::DAY_S && card::lifetime_s(SRC_TOPO, -1) == 90 * card::DAY_S);
  CHECK(card::lifetime_s(SRC_OSM, -1) == 7 * card::DAY_S && card::lifetime_s(SRC_OSM, 3600) == 3600);
  CHECK(card::lifetime_s(SRC_GRAY_DARK, 0) == 0);
  CHECK(http::max_age("max-age=604800") == 604800 && http::max_age("public, max-age=3600, s-maxage=99") == 3600);
  CHECK(http::max_age("no-store") == 0 && http::max_age("private, no-cache") == 0 && http::max_age("public") == -1);
  CHECK(http::max_age("max-age=-5") == 0 && http::max_age(nullptr) == -1);
  // The header: written and read back, and when a tile is still good.
  {
    uint8_t b[card::HEADER_BYTES];
    const card::Header h = card::header_for(1700000000, 3600, true);
    card::encode(h, b);
    card::Header back;
    CHECK(card::decode(b, sizeof(b), back) && back.fetched == 1700000000 && back.expires == 1700003600 && back.empty);
    CHECK(!card::decode(b, 8, back));
    b[0] = 'X';
    CHECK(!card::decode(b, sizeof(b), back));
    CHECK(card::fresh(h, 1700003599) && !card::fresh(h, 1700003600));
    CHECK(card::fresh(h, 0));  // no clock (no network): whatever the card has
    const card::Header unknown = card::header_for(0, 3600, false);
    CHECK(unknown.expires == 0 && !card::fresh(unknown, 1700000000) && card::fresh(unknown, 0));
  }
  // An area: every zoom of the sectional, the lowest first, each tile once, all of them in the square.
  {
    card::Area a;
    a.style = SRC_SECTIONAL;
    a.lat = 40.64;
    a.lon = -73.78;
    a.radius_nm = 100;
    a.z_max = 11;
    card::Plan p;
    std::string error;
    CHECK(card::plan(a, p, error));
    CHECK(p.ranges.size() == 4 && p.ranges.front().z == 8 && p.ranges.back().z == 11);
    CHECK(p.total > 500 && p.total < 1500);
    std::vector<std::string> seen;
    int last_z = 0;
    for (size_t i = 0; i < p.total; ++i) {
      const Key t = p.at(i);
      CHECK(t.style == SRC_SECTIONAL && t.z >= last_z && t.z >= 8 && t.z <= 11);
      last_z = t.z;
      seen.push_back(card::path(t));
    }
    std::sort(seen.begin(), seen.end());
    CHECK(std::unique(seen.begin(), seen.end()) == seen.end());
    // The place's own tile at the deepest zoom is one of them.
    const geo::Pixel here = geo::world(a.lat, a.lon, 11);
    const std::string mine = card::path(Key{SRC_SECTIONAL, 11, (uint32_t) (here.x / 256), (uint32_t) (here.y / 256)});
    CHECK(std::binary_search(seen.begin(), seen.end(), mine));
    CHECK(card::estimate_bytes(p) == p.total * 40 * 1024);
    // On the card, whole clusters: half of one more a tile on average, and the header.
    CHECK(card::card_bytes(p, 32 * 1024) == p.total * (40 * 1024 + card::HEADER_BYTES + 16 * 1024));
    CHECK(card::card_bytes(p, 0) == card::card_bytes(p, card::CLUSTER_GUESS));
    CHECK(card::card_bytes(p, 64 * 1024) > card::card_bytes(p, 32 * 1024));
    // Wider takes more; what may not be downloaded, or is too much, is refused with words.
    card::Area wide = a;
    wide.radius_nm = 200;
    card::Plan pw;
    CHECK(card::plan(wide, pw, error) && pw.total > 3 * p.total);
    card::Area osm = a;
    osm.style = SRC_OSM;
    CHECK(!card::plan(osm, pw, error) && !error.empty());
    card::Area deep = a;
    deep.z_max = 12;
    CHECK(!card::plan(deep, pw, error));
    card::Area topo = a;
    topo.style = SRC_TOPO;
    topo.radius_nm = 250;
    topo.z_max = 16;
    CHECK(!card::plan(topo, pw, error) && error.find("tiles") != std::string::npos);
    topo.z_max = card::default_zoom(SRC_TOPO);
    CHECK(card::plan(topo, pw, error) && pw.ranges.front().z == 0);
    card::Area far = a;
    far.radius_nm = 0;
    CHECK(!card::plan(far, pw, error));
  }
  // A download's record on the card, read back.
  {
    card::Area a;
    a.style = SRC_IFR_LOW;
    a.lat = 47.45;
    a.lon = -122.31;
    a.radius_nm = 60;
    a.z_max = 10;
    card::Area back;
    size_t next = 0;
    CHECK(card::read_job(card::job_text(a, 123).c_str(), back, next));
    CHECK(back.style == SRC_IFR_LOW && std::fabs(back.lat - 47.45) < 1e-6 && back.radius_nm == 60 && back.z_max == 10);
    CHECK(next == 123 && !card::read_job("rubbish", back, next));
    uint8_t style = 0;
    CHECK(card::style_of("ifr_high", style) && style == SRC_IFR_HIGH && !card::style_of("osm", style));
    CHECK(std::string(card::map_of(SRC_TOPO)) == "topo");
  }
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
  // Read plane by plane: what is no plane in the list is passed over, and brackets and quotes inside strings are words.
  const std::string odd = R"({"now":1,"note":"a } and a ] and \" in words","aircraft":[7,"plane",null,[],true,
    {"hex":"a1","lat":1.5,"lon":2.5,"flight":"X]}\"Y","nested":{"a":[1,{"b":"}"}]}},
    {"hex":"a2","lat":"1","lon":2},{"hex":"a3","lat":91,"lon":2},{"hex":"","lat":1,"lon":2},
    {"hex":"a4","lat":-1,"lon":-2,"alt_baro":-40}],"messages":5})";
  CHECK(feed::parse(odd.c_str(), odd.size(), doc, flights, error) && flights.size() == 2);
  CHECK(flights[0].hex == "a1" && flights[0].callsign == "X]}\"Y" && flights[1].altitude == -40);
  // Broken off anywhere, or wrong anywhere: an error, never a part of the list.
  for (size_t cut = 1; cut + 1 < odd.size(); cut += 7) {
    flights.clear();
    CHECK(!feed::parse(odd.c_str(), cut, doc, flights, error) && !error.empty());
  }
  const std::string wrong = R"({"aircraft":[{"hex":"a1","lat":1,"lon":2}}]})";
  CHECK(!feed::parse(wrong.c_str(), wrong.size(), doc, flights, error));
  CHECK(!feed::parse("", 0, doc, flights, error) && error == "JSON: IncompleteInput");
  CHECK(!feed::parse("{\"now\":1}", 9, doc, flights, error) && error == "no aircraft list");
  // A crowd: five thousand planes, all read (read whole, a 32-bit chip ran out of slots, and took a minute).
  std::string crowd = "{\"aircraft\":[";
  for (int i = 0; i < 5000; ++i) {
    char one[160];
    std::snprintf(one, sizeof(one), "%s{\"hex\":\"d%05x\",\"flight\":\"CRWD%d\",\"alt_baro\":%d,\"lat\":%.4f,\"lon\":%.4f}",
                  i ? "," : "", i, i, 1000 + i, 40 + i * 1e-4, -73 - i * 1e-4);
    crowd += one;
  }
  crowd += "]}";
  CHECK(feed::parse(crowd.c_str(), crowd.size(), doc, flights, error) && flights.size() == 5000);
  CHECK(flights[4999].hex == "d01387" && flights[4999].callsign == "CRWD4999");
  // Words cut at a byte count, but never inside a character.
  CHECK(feed::trimmed("\xC3\x84\xC3\x96\xC3\x9C\xE2\x9C\x88\xC3\x85X", 10) == "\xC3\x84\xC3\x96\xC3\x9C\xE2\x9C\x88");
  CHECK(feed::trimmed("\xE2\x9C\x88\xE2\x9C\x88", 4) == "\xE2\x9C\x88");
  CHECK(feed::trimmed("\xE2\x9C\x88", 2) == "" && feed::trimmed("ABCDEFGHIJK", 10) == "ABCDEFGHIJ");
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
  // A receiver's address as it is, with the place and range put in where it asks for them.
  CHECK(feed::url(RECEIVER, 1, 2, 5, "http://r.local/data/aircraft.json") == "http://r.local/data/aircraft.json");
  CHECK(feed::url(RECEIVER, 40.5, -73.25, 300, "http://r:8080/v2/{lat}/{lon}/{dist}?a={lat}") ==
        "http://r:8080/v2/40.5000/-73.2500/250?a=40.5000");
}

static void test_tiles() {
  Settings s;
  s.theme = DARK;  // Esri's dark gray below; the light one is the default
  s.lat = 40.64;
  s.lon = -73.78;
  s.located = true;
  s.range_nm = 20;
  s.map = STREETS;
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
  s.map = STREETS;
  CHECK(tiles::credit(s) == "Esri, HERE, Garmin, \xC2\xA9 OpenStreetMap");
  // A screen that kept CARTO's style takes Esri's in its place.
  Settings old;
  JsonDocument carto;
  deserializeJson(carto, R"({"map":"carto_light"})");
  std::string why;
  CHECK(apply(carto.as<JsonObjectConst>(), old, why) && old.map == STREETS && old.theme == LIGHT);
  JsonDocument gray;
  deserializeJson(gray, R"({"map":"dark_gray"})");
  CHECK(apply(gray.as<JsonObjectConst>(), old, why) && old.map == STREETS && old.theme == DARK);
  // Streets is Esri's dark or light gray by the appearance; the tiles of each are their own in the cache.
  Settings st;
  st.map = STREETS;
  st.theme = DARK;
  CHECK(tiles::source_id(st) == tiles::SRC_GRAY_DARK);
  st.theme = LIGHT;
  CHECK(tiles::source_id(st) == tiles::SRC_GRAY_LIGHT);
  CHECK(tiles::url(st, {tiles::SRC_GRAY_LIGHT, 3, 1, 2}).find("World_Light_Gray_Base") != std::string::npos);
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
  CHECK(tiles::url(s, {tiles::SRC_CUSTOM, 3, 1, 2}) == "https://t.example/3/1/2.png");
  const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}, jpg[] = {0xFF, 0xD8, 0xFF};
  CHECK(tiles::format_of(png, 8) == tiles::PNG && tiles::format_of(jpg, 3) == tiles::JPEG);
  CHECK(tiles::rgb565(255, 255, 255) == 0xFFFF && tiles::rgb565(255, 0, 0) == 0xF800);
  // The night rules. Turned round: white paper dark, black ink light, both within the dark screen's ground and ink.
  const tiles::Filter invert{tiles::NIGHT_INVERT, 256};
  CHECK(tiles::paint(invert, 255, 255, 255) == tiles::rgb565(tiles::NIGHT_LOW, tiles::NIGHT_LOW, tiles::NIGHT_LOW));
  CHECK(tiles::paint(invert, 0, 0, 0) == tiles::rgb565(tiles::NIGHT_HIGH, tiles::NIGHT_HIGH, tiles::NIGHT_HIGH));
  // Paper and ink only: white paper dark as above, a chart's blue keeps its hue and stays bright.
  const tiles::Filter colours{tiles::NIGHT_COLOURS, 256};
  CHECK(tiles::paint(colours, 255, 255, 255) == tiles::rgb565(tiles::NIGHT_LOW, tiles::NIGHT_LOW, tiles::NIGHT_LOW));
  const tiles::Rgb blue = tiles::night_colours(0, 60, 160);
  CHECK(blue.b > blue.g && blue.g > blue.r && blue.b >= tiles::COLOUR_VALUE);
  // Dimmed to 60 %, and the map brightness after any rule.
  CHECK(tiles::paint({tiles::NIGHT_DIM, 256}, 200, 200, 200) == tiles::rgb565(120, 120, 120));
  CHECK(tiles::paint({tiles::NIGHT_NONE, 128}, 200, 100, 50) == tiles::rgb565(100, 50, 25));
  // Which rule each map takes in the dark, and none in the light or on a server of one's own.
  Settings n;
  for (const auto &[map, rule] : {std::pair{SECTIONAL, tiles::NIGHT_DIM}, {IFR_LOW, tiles::NIGHT_COLOURS},
                                  {IFR_HIGH, tiles::NIGHT_COLOURS}, {TOPO, tiles::NIGHT_INVERT}, {OSM, tiles::NIGHT_INVERT},
                                  {SATELLITE, tiles::NIGHT_NONE}, {STREETS, tiles::NIGHT_NONE}, {CUSTOM, tiles::NIGHT_NONE}}) {
    n.map = map;
    n.theme = DARK;
    CHECK(tiles::filter_of(n).night == rule);
    n.theme = LIGHT;
    CHECK(tiles::filter_of(n).night == tiles::NIGHT_NONE);
  }
}

static void test_frame_key() {
  // A frame is known again by everything that shows in it, and by nothing else.
  Settings s;
  s.lat = 40.64;
  s.lon = -73.78;
  s.located = true;
  s.map = SECTIONAL;
  s.theme = DARK;  // so the light appearance below is a change
  const tiles::Plan p = tiles::plan(s, 600, 450, 2);
  const std::string key = tiles::frame_key(s, p);
  CHECK(!key.empty() && key == tiles::frame_key(s, tiles::plan(s, 600, 450, 2)));
  Settings t = s;
  t.tint = AMBER;  // the radar's phosphor: no part of a map
  t.rings = false;  // drawn over the frame, not in it
  CHECK(tiles::frame_key(t, tiles::plan(t, 600, 450, 2)) == key);
  for (auto change : std::vector<void (*)(Settings &)>{
           [](Settings &x) { x.map = IFR_LOW; }, [](Settings &x) { x.theme = LIGHT; },
           [](Settings &x) { x.map_brightness = 70; }, [](Settings &x) { x.lat += 0.2; },
           [](Settings &x) { x.range_nm = 60; }}) {
    t = s;
    change(t);
    CHECK(tiles::frame_key(t, tiles::plan(t, 600, 450, 2)) != key);
  }
  // Place names are a layer of their own tiles; the custom server's address is part of its map.
  t = s;
  t.map = STREETS;
  const std::string named = tiles::frame_key(t, tiles::plan(t, 600, 450, 2));
  t.place_names = false;
  CHECK(tiles::frame_key(t, tiles::plan(t, 600, 450, 2)) != named);
  t.map = CUSTOM;
  t.custom_url = "https://a.example/{z}/{x}/{y}.png";
  const std::string a = tiles::frame_key(t, tiles::plan(t, 600, 450, 2));
  t.custom_url = "https://b.example/{z}/{x}/{y}.png";
  CHECK(tiles::frame_key(t, tiles::plan(t, 600, 450, 2)) != a);
  // The radar has no tiles, and so no frame to keep.
  t.map = RADAR;
  CHECK(tiles::frame_key(t, tiles::plan(t, 600, 450, 2)).empty());
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
  // On round glass a plane in a corner is off the glass: not shown, so it takes no place from one that is.
  {
    Settings r = s;
    tiles::Plan q = tiles::plan(r, 720, 720, 1);
    q.view.round = true;
    const geo::Place corner = q.view.place(30, 30), rim = q.view.place(360, 20);
    std::vector<feed::Flight> two(2, flights[0]);
    two[0].hex = "corner";
    two[0].lat = corner.lat;
    two[0].lon = corner.lon;
    two[1].hex = "rim";
    two[1].lat = rim.lat;
    two[1].lon = rim.lon;
    const auto seen = overlay::shown(two, r, q.view);
    CHECK(seen.size() == 1 && seen[0]->hex == "rim");
    q.view.round = false;
    CHECK(overlay::shown(two, r, q.view).size() == 2);
  }
  // A busy sky cut to what can be shown: the nearest `most` and the extra, each watched one, nothing off the map.
  {
    std::vector<feed::Flight> sky;
    for (int i = 0; i < 300; ++i) {
      feed::Flight f;
      f.hex = "a" + std::to_string(i);
      f.lat = s.lat + (i % 2 ? 1 : -1) * 0.0004 * i;
      f.lon = s.lon;
      f.has_altitude = true;
      f.altitude = 10000;
      sky.push_back(f);
    }
    feed::Flight far = sky[0];
    far.hex = "far";
    far.lat = s.lat + 10;  // off the map
    sky.push_back(far);
    feed::Flight watched = sky[299];
    watched.hex = "w1";
    watched.registration = "N12345";
    sky.push_back(watched);
    Settings busy = s;
    busy.most = 10;
    busy.watch = {{"N12345", 0}};
    overlay::keep_showable(sky, busy, p.view, 20);
    CHECK(sky.size() <= 31);
    CHECK(std::any_of(sky.begin(), sky.end(), [](const feed::Flight &f) { return f.hex == "a0"; }));
    CHECK(std::any_of(sky.begin(), sky.end(), [](const feed::Flight &f) { return f.hex == "w1"; }));
    CHECK(std::none_of(sky.begin(), sky.end(), [](const feed::Flight &f) { return f.hex == "far" || f.hex == "a299"; }));
    // What is shown of it is what would have been shown of the whole.
    CHECK(overlay::shown(sky, busy, p.view).size() == 11);
  }
  // Below sea level, or above the highest ceiling: shown while no limit is set, hidden by one.
  {
    std::vector<feed::Flight> low = flights;
    low[0].altitude = -40;
    low[1].altitude = ALTITUDE_CEILING + 5000;
    Settings open = s;
    open.alt_min = 0;
    open.alt_max = ALTITUDE_CEILING;
    CHECK(overlay::shown(low, open, p.view).size() == overlay::shown(flights, open, p.view).size());
    open.alt_min = 100;
    open.alt_max = ALTITUDE_CEILING - 1000;
    CHECK(overlay::shown(low, open, p.view).size() < overlay::shown(flights, open, p.view).size());
  }
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
  {
    // Descending at 1,500 ft a minute: the arrow between its altitude and its speed.
    const std::string down = std::string("TEST12 B738\n350") + plan::ARROW_DOWN;
    CHECK(overlay::label_of(flights[0], LABEL_FULL, false) == down + " 451 kt" ||
          overlay::label_of(flights[0], LABEL_FULL, false) == down + " 450 kt");
  }
  CHECK(overlay::label_of(flights[2], LABEL_CALLSIGN, false) == "ABC004");
  {
    // A scope's data block: altitude in hundreds of feet, speed in tens of knots, the wake category.
    feed::Flight block = flights[0];
    block.has_climb = false;
    block.speed = 432;
    CHECK(overlay::label_of(block, LABEL_SCOPE, false) == "TEST12\n350 43 F");
    // The FAA's categories by type, A to I, else the transponder's class only where it leaves no doubt.
    for (const auto &[type, cwt] : std::initializer_list<std::pair<const char *, const char *>>{
             {"A388", "A"}, {"B77W", "B"}, {"B763", "C"}, {"B703", "D"}, {"B752", "E"}, {"A320", "F"},
             {"CRJ9", "G"}, {"C56X", "H"}, {"C172", "I"}, {"A35K", "D"}}) {
      block.type = type;
      CHECK(overlay::wake_of(block) == cwt);
    }
    block.type = "XXXX";
    block.category = "A1";
    CHECK(overlay::wake_of(block) == "I");
    block.category = "A4";
    CHECK(overlay::wake_of(block) == "E");
    block.category = "A5";
    CHECK(overlay::wake_of(block) == "");
    block.type = "";
    block.category = "";
    CHECK(overlay::wake_of(block) == "");
    block.type = "B738";
    block.speed = 95;
    block.has_climb = true;
    block.climb = 900;
    CHECK(overlay::label_of(block, LABEL_SCOPE, false) == std::string("TEST12\n350") + plan::ARROW_UP + " 10 F");
    CHECK(overlay::block_speed(block, true) == "18");
    block.has_speed = false;
    block.has_altitude = false;
    CHECK(overlay::label_of(block, LABEL_SCOPE, false) == "TEST12\nF");
  }
  CHECK(overlay::altitude_text(flights[2], false) == "2500 ft" && overlay::altitude_text(flights[2], true) == "760 m");
  CHECK(overlay::label_of(flights[2], LABEL_ALTITUDE, false) == "ABC004\n025");
  {
    // A climb or a descent of 200 ft a minute or more puts an arrow after the altitude; the glass draws it.
    feed::Flight moving = flights[2];
    moving.has_climb = true;
    moving.climb = 1500;
    CHECK(overlay::label_of(moving, LABEL_ALTITUDE, false) == std::string("ABC004\n025") + plan::ARROW_UP);
    moving.climb = -800;
    CHECK(overlay::label_of(moving, LABEL_ALTITUDE, false) == std::string("ABC004\n025") + plan::ARROW_DOWN);
    moving.climb = 150;
    CHECK(overlay::label_of(moving, LABEL_ALTITUDE, false) == "ABC004\n025");
    moving.climb = 1500;
    moving.on_ground = true;
    CHECK(overlay::label_of(moving, LABEL_ALTITUDE, false) == "ABC004\nGND");
    const auto cut = plan::pieces(std::string("025") + plan::ARROW_DOWN + " 451 kt");
    CHECK(cut.size() == 2 && cut[0].text == "025" && cut[0].arrow == -1 && cut[1].text == " 451 kt" && !cut[1].arrow);
    const auto end = plan::pieces(std::string("350") + plan::ARROW_UP);
    CHECK(end.size() == 1 && end[0].text == "350" && end[0].arrow == 1);
    CHECK(plan::pieces("ABC004").size() == 1 && plan::pieces("ABC004")[0].text == "ABC004");
  }
  CHECK(overlay::block_altitude(flights[2], true) == "760 m");
  {
    feed::Flight low = flights[2];
    low.altitude = 40;
    CHECK(overlay::block_altitude(low, false) == "000");
    low.altitude = -40;
    CHECK(overlay::block_altitude(low, false) == "000");
    low.altitude = -120;
    CHECK(overlay::block_altitude(low, false) == "-001");
    low.altitude = -1350;
    CHECK(overlay::block_altitude(low, false) == "-014");
    low.altitude = 45049;
    CHECK(overlay::block_altitude(low, false) == "450");
  }
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

static void test_shapes() {
  using namespace shapes;
  // Each kind drawn: something solid in its middle third, nothing past the mask's edge, and the same at every size.
  for (int k = 0; k < KINDS; ++k) {
    const sprites::Mask m = sprites::render(40, 0, (Kind) k);
    int inked = 0, edge = 0;
    for (int y = 0; y < m.size; ++y)
      for (int x = 0; x < m.size; ++x) {
        const int a = m.alpha[(size_t) y * m.size + x];
        inked += a > 0;
        if ((x == 0 || y == 0 || x == m.size - 1 || y == m.size - 1) && a) ++edge;
      }
    CHECK(inked > 60 && edge == 0);
    CHECK(outline((Kind) k).count <= 16);
    if (inked <= 60 || edge) std::printf("  kind %s: %d inked, %d on the edge\n", NAMES[k], inked, edge);
  }
  // The airliner is the plane as it was drawn: its nose up, solid in the middle, wing tips at the sides.
  const sprites::Mask a = sprites::render(30, 0, AIRLINER);
  CHECK(a.alpha[16 * a.size + 16] == 255 && a.alpha[3 * a.size + 16] > 0 && a.alpha[20 * a.size + 2] > 0);
  // A balloon does not turn; a helicopter's rotor is faint and its body solid.
  const sprites::Mask b0 = sprites::render(30, 0, BALLOON), b1 = sprites::render(30, 45, BALLOON);
  CHECK(b0.alpha == b1.alpha);
  const sprites::Mask h = sprites::render(40, 0, HELICOPTER);
  const int rotor = h.alpha[(size_t) (h.size / 2 - 1) * h.size + h.size / 2 + 13], body = h.alpha[(size_t) (h.size / 2 - 4) * h.size + h.size / 2];
  CHECK(rotor > 60 && rotor < 160 && body == 255);
  // The cache keeps kinds apart, and a kind that does not turn has one mask for every heading.
  sprites::Cache<int> cache;
  CHECK(cache.get(30, 0, 0, 1 << 30, AIRLINER) != cache.get(30, 0, 0, 1 << 30, HELICOPTER));
  cache.get(30, 90, 0, 1 << 30, BALLOON);
  cache.get(30, 200, 0, 1 << 30, BALLOON);
  CHECK(cache.size() == 3);
  // A kind by its type first, its words next, its class after, else an airliner.
  CHECK(kind_of("A3", "B738", "") == AIRLINER && kind_of("A5", "B77W", "") == HEAVY && kind_of("", "B744", "") == QUAD);
  CHECK(kind_of("", "C172", "") == shapes::LIGHT && kind_of("", "R44", "") == HELICOPTER && kind_of("", "PC12", "") == TURBOPROP);
  CHECK(kind_of("", "GLF6", "") == JET && kind_of("", "F16", "") == FAST && kind_of("", "BALL", "") == BALLOON);
  CHECK(kind_of("A3", "AT76", "") == TURBOPROP);  // the type over the class: a large turboprop is still a turboprop
  CHECK(kind_of("A7", "XXXX", "") == HELICOPTER && kind_of("A1", "", "") == shapes::LIGHT && kind_of("B6", "", "") == DRONE);
  CHECK(kind_of("B1", "", "") == GLIDER && kind_of("B2", "", "") == BALLOON && kind_of("C2", "", "") == VEHICLE);
  CHECK(kind_of("A2", "", "General Atomics MQ-9 Reaper") == DRONE);  // the words over the class
  CHECK(kind_of("B2", "", "GOODYEAR BLIMP") == AIRSHIP && kind_of("", "", "ROBINSON HELICOPTER R-44") == HELICOPTER);
  CHECK(kind_of("", "", "") == AIRLINER && kind_of("C3", "", "") == AIRLINER && kind_of("Z9", "", "") == AIRLINER);
  // The table names each type once.
  for (size_t i = 0; i < std::size(TYPES); ++i)
    for (size_t j = i + 1; j < std::size(TYPES); ++j) CHECK(std::strcmp(TYPES[i].code, TYPES[j].code) != 0);
  // By type, an overlay's planes carry their kinds; a plain plane marker draws every one as an airliner.
  feed::Flight heli;
  heli.hex = "h1";
  heli.lat = 40.65;
  heli.lon = -73.78;
  heli.type = "EC35";
  heli.has_altitude = true;
  heli.altitude = 1000;
  Settings s;
  s.lat = 40.65;
  s.lon = -73.78;
  s.map = RADAR;
  const tiles::Plan q = tiles::plan(s, 600, 450, 1);
  overlay::Tracks tracks;
  plan::Overlay o = overlay::build({heli}, s, q.view, tracks, 0, 8, false);
  CHECK(s.marker == MARKER_TYPE && o.marker == plan::PLANE && o.planes.size() == 1 && o.planes[0].kind == HELICOPTER);
  s.marker = MARKER_PLANE;
  o = overlay::build({heli}, s, q.view, tracks, 0, 8, false);
  CHECK(o.planes.size() == 1 && o.planes[0].kind == AIRLINER);
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
  // A receiver at a new address starts afresh: what the old one earned is forgotten.
  providers::Providers moved;
  moved.set_order(RECEIVER, false, true);
  for (int i = 0; i < 5; ++i) moved.failed(RECEIVER, 0, down);
  CHECK(!moved.choose(1000, f, wait) && wait > 60000);
  moved.forget(RECEIVER);
  CHECK(moved.choose(1000, f, wait) && f == RECEIVER && moved.state(RECEIVER).failures == 0);
  // Back to the preferred one once its quota is back.
  CHECK(p.choose(1000 + 26798u * 1000 + 5000, f, wait) && f == ADSB_FI);
  // An announced pace holds until the quota's end, then the refresh asked for again.
  providers::Providers r;
  CHECK(r.succeeded(ADSB_LOL, 0, 10000, {200, 0, 600, "500, 500;w=86400", ""}) == 172800);
  CHECK(r.succeeded(ADSB_LOL, 300000, 10000, {200}) == 172800);   // still within its 600 s
  CHECK(r.succeeded(ADSB_LOL, 700000, 10000, {200}) == 10000);    // past it
  // Without falling back, a source that waits is waited for.
  providers::Providers alone;
  // A receiver is tried only where it has an address: preferred, then the exchanges; else as if it were not there.
  {
    providers::Providers own;
    own.set_order(RECEIVER, true, true);
    const auto o = own.order();
    CHECK(o.size == 3 && o.list[0] == RECEIVER && o.list[1] == ADSB_LOL && o.list[2] == ADSB_FI);
    Feed got;
    uint32_t w;
    CHECK(own.choose(0, got, w) && got == RECEIVER);
    own.failed(RECEIVER, 0, {0, 0, 0, "", "no answer"});
    CHECK(own.choose(10, got, w) && got == ADSB_LOL);  // falls back to the exchanges
    own.set_order(ADSB_FI, true, false);
    CHECK(own.order().size == 2 && own.order().list[0] == ADSB_FI);
    own.set_order(RECEIVER, true, false);  // preferred without an address: the exchanges
    CHECK(own.order().size == 2 && own.choose(20, got, w) && got != RECEIVER);
    own.set_order(RECEIVER, false, false);  // and without falling back: nothing, looked at again in a minute
    CHECK(own.order().size == 0 && !own.choose(30, got, w) && w == 60000);
  }
  // The radio likewise only on a screen with one: preferred, then the exchanges; and waited for only as long as it
  // says (the worker gives a radio with no stick a short Retry-After of its own).
  {
    providers::Providers own;
    own.set_order(RADIO, true, false, true);
    auto o = own.order();
    CHECK(o.size == 3 && o.list[0] == RADIO && o.list[1] == ADSB_LOL && o.list[2] == ADSB_FI);
    Feed got;
    uint32_t w;
    CHECK(own.choose(0, got, w) && got == RADIO);
    CHECK(own.failed(RADIO, 0, {0, 5, 0, "", "no radio"}) == 5000);
    CHECK(own.choose(10, got, w) && got == ADSB_LOL);
    CHECK(own.choose(5000, got, w) && got == RADIO);
    own.set_order(RADIO, true, false, false);  // no radio: the exchanges
    o = own.order();
    CHECK(o.size == 2 && o.list[0] == ADSB_LOL);
    own.set_order(ADSB_FI, true, true, true);  // an exchange preferred: the receiver, then the radio, after the others
    o = own.order();
    CHECK(o.size == 4 && o.list[0] == ADSB_FI && o.list[2] == RECEIVER && o.list[3] == RADIO);
  }
  // The menu steps past the radio on a screen without one, and onto it on one with.
  {
    const menu::Row *feed_row = nullptr;
    for (const auto &r : menu::rows())
      if (std::string(r.key) == "feed") feed_row = &r;
    CHECK(feed_row != nullptr);
    if (feed_row) {
      Settings st;
      st.feed = RECEIVER;
      st.receiver_url = "http://receiver.local/data/aircraft.json";
      radio_fitted = false;
      CHECK(menu::stepped(*feed_row, st, 1) && st.feed == ADSB_FI);
      st.feed = RECEIVER;
      radio_fitted = true;
      CHECK(menu::stepped(*feed_row, st, 1) && st.feed == RADIO);
      radio_fitted = false;
    }
  }
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

static void test_menu() {
  // Every value of every row is one the settings take, and a row shows it as its label.
  for (const auto &row : menu::rows()) {
    for (const auto &c : row.choices) {
      Settings st;
      JsonDocument change;
      if (row.kind == menu::NUMBER) change[row.key] = std::atoi(c.value);
      else change[row.key] = c.value;
      std::string error;
      const bool ok = apply(change.as<JsonObjectConst>(), st, error);
      if (!ok) std::printf("menu row %s refuses %s\n", row.key, c.value);
      CHECK(ok);
      if (ok) CHECK(menu::shown(row, st) == c.label || std::string(row.key) == "map");
    }
  }
  auto row = [](const char *key) -> const menu::Row & {
    for (const auto &r : menu::rows())
      if (std::string(r.key) == key) return r;
    return menu::rows()[0];
  };
  Settings st;
  // Numbers stop at their ends; one between two choices steps to the next its way.
  st.range_nm = 6;
  CHECK(menu::stepped(row("range_nm"), st, 1) && st.range_nm == 7);
  st.range_nm = 6;
  CHECK(menu::stepped(row("range_nm"), st, -1) && st.range_nm == 5);
  st.range_nm = 250;
  CHECK(!menu::stepped(row("range_nm"), st, 1) && st.range_nm == 250);
  // Words go round; the map skips the custom server when none is set.
  st.map = IFR_HIGH;
  CHECK(menu::stepped(row("map"), st, 1) && st.map == RADAR);
  st.custom_url = "https://t.example/{z}/{x}/{y}.png";
  st.map = IFR_HIGH;
  CHECK(menu::stepped(row("map"), st, 1) && st.map == CUSTOM);
  CHECK(menu::shown(row("map"), st) == "Custom");
  // Flags turn over.
  st.rings = true;
  CHECK(menu::stepped(row("rings"), st, 1) && !st.rings && menu::shown(row("rings"), st) == "Off");
  CHECK(menu::shown(row("metric"), st) == "ft, kt");
  // Every group starts at a row there is, in the rows' order, the first row starting the first group.
  {
    size_t at = 0, found = 0;
    CHECK(menu::group_starting(menu::rows()[0]) != nullptr);
    for (const auto &g : menu::groups())
      for (size_t i = at; i < menu::rows().size(); ++i)
        if (std::string(menu::rows()[i].key) == g.first_key) { at = i; ++found; break; }
    CHECK(found == menu::groups().size());
  }
  // Rows follow the map: the phosphor on the radar only, place names on streets and imagery.
  st.map = RADAR;
  CHECK(menu::applies(row("tint"), st) && !menu::applies(row("place_names"), st) && !menu::applies(row("theme"), st));
  st.map = SATELLITE;
  CHECK(!menu::applies(row("tint"), st) && menu::applies(row("place_names"), st) && menu::applies(row("tile_scale"), st));
  st.map = SECTIONAL;
  CHECK(!menu::applies(row("tile_scale"), st) && menu::applies(row("map_brightness"), st));
  // Bedtime's times show with bedtime on; the time it comes on again only with that on too.
  st.bedtime = false;
  CHECK(!menu::applies(row("bed_off"), st) && !menu::applies(row("timezone"), st));
  st.bedtime = true;
  st.bed_wake = false;
  CHECK(menu::applies(row("bed_off"), st) && !menu::applies(row("bed_on"), st));
  st.bed_wake = true;
  CHECK(menu::applies(row("bed_on"), st));
  // Every row is stepped, typed, or both; a row with nothing to step has no arrows to step it with.
  for (const auto &r : menu::rows()) {
    CHECK(menu::steps(r) || menu::typed(r));
    Settings any;
    if (!menu::steps(r)) CHECK(!menu::stepped(r, any, 1));
  }

  // Typed: a number between the choices shows with its unit, and steps on from there.
  Settings t;
  CHECK(menu::typed_in(row("range_nm"), t, " 12 ") && t.range_nm == 12 && menu::shown(row("range_nm"), t) == "12 NM");
  CHECK(menu::stepped(row("range_nm"), t, 1) && t.range_nm == 15);
  CHECK(!menu::typed_in(row("range_nm"), t, "300") && t.range_nm == 15);
  CHECK(!menu::typed_in(row("range_nm"), t, "12a") && !menu::typed_in(row("range_nm"), t, ""));
  CHECK(menu::text_of(row("range_nm"), t) == "15");
  // Coordinates: the place's name goes with a coordinate typed, and there is a place from then on.
  t.place = "KJFK";
  CHECK(menu::shown(row("lat"), t) == "Not set" && menu::shown(row("place"), t) == "KJFK");
  CHECK(menu::typed_in(row("lat"), t, "-33.94610") && t.located && near(t.lat, -33.9461, 1e-9) && t.place.empty());
  CHECK(menu::typed_in(row("lon"), t, "151.17722") && menu::text_of(row("lon"), t) == "151.17722");
  CHECK(menu::shown(row("place"), t) == "-33.946, 151.177");
  CHECK(!menu::typed_in(row("lat"), t, "91") && !menu::typed_in(row("lat"), t, "1.2.3") && near(t.lat, -33.9461, 1e-9));
  // Words, URLs and times, through the settings' own checks.
  CHECK(menu::shown(row("custom_url"), t) == "None");
  CHECK(!menu::typed_in(row("custom_url"), t, "ftp://x/{z}") && t.custom_url.empty());
  CHECK(menu::typed_in(row("custom_url"), t, "https://t.example/{z}/{x}/{y}.png") && !t.custom_url.empty());
  CHECK(menu::typed_in(row("bed_off"), t, "23:30") && t.bed_off == "23:30");
  CHECK(!menu::typed_in(row("bed_off"), t, "25:00") && t.bed_off == "23:30");
  // A time zone typed that is none of the list shows as typed, and the arrows go on into the list from it.
  CHECK(menu::typed_in(row("timezone"), t, "WET0WEST,M3.5.0/1,M10.5.0") && menu::shown(row("timezone"), t) == "WET0WEST,M3.5.0/1,M10.5.0");
  CHECK(menu::stepped(row("timezone"), t, 1) && t.timezone == "UTC0");
  CHECK(menu::typed_in(row("timezone"), t, "CET-1CEST,M3.5.0,M10.5.0/3") && menu::shown(row("timezone"), t) == "Central Europe");
  // The watched aircraft, with a comma between, red where no colour is given; written back the same way.
  CHECK(menu::typed_in(row("watch"), t, "n12345 Blue, BAW123,") && t.watch.size() == 2 && t.watch[0].id == "N12345" &&
        t.watch[0].colour == 4 && t.watch[1].colour == 0);
  CHECK(menu::text_of(row("watch"), t) == "N12345 blue, BAW123 red" && menu::shown(row("watch"), t) == "2 aircraft");
  CHECK(!menu::typed_in(row("watch"), t, "N1 pink") && t.watch.size() == 2);
  CHECK(menu::typed_in(row("watch"), t, "") && t.watch.empty() && menu::shown(row("watch"), t) == "None");
  // Rotation steps through the ways up the settings take.
  CHECK(menu::shown(row("rotation"), t) == "Default" && menu::stepped(row("rotation"), t, 1) && t.rotation == 0);
  // Every hint of a typed row, and every typed row has one.
  for (const auto &r : menu::rows())
    if (menu::typed(r)) CHECK(*r.hint);

  // Wi-Fi: a name of 1 to 32 bytes; no password, 8 to 63 characters, or 64 hexadecimal digits.
  CHECK(!menu::network_refused("Home", "") && !menu::network_refused("Home", "12345678"));
  CHECK(menu::network_refused("", "12345678") && menu::network_refused(std::string(33, 'n'), ""));
  CHECK(menu::network_refused("Home", "1234567") && menu::network_refused("Home", std::string(64, 'z')));
  CHECK(!menu::network_refused("Home", std::string(64, 'a')) && !menu::network_refused("Home", std::string(63, 'z')));
}

static void test_keyboard() {
  using namespace keyboard;
  // Every printable character is on a page of the words' keyboard, and every row fills its width or less.
  std::string all;
  for (Page page : {LETTERS, NUMBERS, SYMBOLS})
    for (const auto &row : layout(WORDS, page)) {
      int width = 0;
      for (const auto &k : row) {
        width += k.width;
        if (k.action == TYPE) all += k.text;
      }
      CHECK(width <= ROW_WIDTH);
    }
  for (char c = ' '; c < 127; ++c) {
    const bool typed = all.find(c) != std::string::npos || (c >= 'A' && c <= 'Z');  // capitals: a letter, shifted
    if (!typed) std::printf("no key types '%c'\n", c);
    CHECK(typed);
  }
  for (Pad pad : {WHOLE, DECIMAL, CLOCK})
    for (const auto &row : layout(pad, DIGITS)) {
      int width = 0;
      for (const auto &k : row) width += k.width;
      CHECK(width == ROW_WIDTH);
    }
  // A board as a finger uses it: capitals with the shift, a page of numbers, the cursor moved and a character put in.
  auto key = [](const Board &b, const char *text) {
    for (const auto &row : b.keys())
      for (const auto &k : row)
        if (!std::strcmp(k.text, text)) return k;
    return Key{"?", TYPE, 2};
  };
  Board b;
  b.start(WORDS, "", 8);
  CHECK(b.page == LETTERS && !b.caps);
  CHECK(b.press(key(b, "Aa")) && b.caps && b.face(key(b, "k")) == "K");
  b.press(key(b, "k"));
  b.press(key(b, "Aa"));
  b.press(key(b, "j"));
  b.press(key(b, " "));
  CHECK(b.press(key(b, "?123")) && b.page == NUMBERS);
  b.press(key(b, "1"));
  CHECK(b.editor.text == "Kj 1");
  b.press(key(b, "<"));
  b.press(key(b, "<"));
  b.press(key(b, "-"));
  CHECK(b.editor.text == "Kj- 1" && b.editor.cursor == 3);
  b.press(key(b, "Del"));
  CHECK(b.editor.text == "Kj 1" && b.editor.cursor == 2);
  // No more than it takes.
  for (int i = 0; i < 10; ++i) b.press(key(b, "9"));
  CHECK(b.editor.text.size() == 8);
  // Digits: the decimal pad has a sign and a point, the clock a colon, the whole numbers neither.
  b.start(DECIMAL, "51.5", 12);
  CHECK(b.page == DIGITS && b.editor.cursor == 4 && key(b, "-").action == TYPE && key(b, ".").action == TYPE);
  b.start(CLOCK, "", 5);
  CHECK(!std::strcmp(key(b, ":").text, ":") && !std::strcmp(key(b, ".").text, "?"));
  // A character of two bytes goes, and is passed, whole.
  Editor e;
  e.start("Z\xC3\xBCrich", 40);
  e.cursor = 3;
  CHECK(e.erase() && e.text == "Zrich" && e.cursor == 1);
  e.start("\xC3\xBC", 40);
  CHECK(e.left() && e.cursor == 0 && e.right() && e.cursor == 2 && !e.right());
  CHECK(chars_in("Z\xC3\xBCrich", 3) == 2);
  CHECK(clipped("ab\xC3\xBC", 3) == "ab" && clipped("abc", 5) == "abc");
}

static void test_places() {
  using namespace places;
  // An airport code goes to the FAA; a search that names an airport does too, by its other words; any other does not.
  CHECK(code_url(" kjfk ").find("where=ICAO_ID%3D%27KJFK%27%20OR%20IDENT%3D%27KJFK%27") != std::string::npos);
  CHECK(code_url("Teterboro Airport").empty() && code_url("K").empty() && code_url("KJF-K").empty());
  const std::string named = named_url("Boeing Field, Seattle");
  CHECK(named.find("BOEING") != std::string::npos && named.find("FLD") != std::string::npos &&
        named.find("SEATTLE") != std::string::npos && named.find("FIELD%25") != std::string::npos);
  CHECK(named_url("Paris").empty() && named_url("Airport").empty());
  CHECK(named_url("O'Hare Airport'; DROP").find("%27%3B") == std::string::npos);
  CHECK(nominatim_url("Zürich HB").find("q=Z%C3%BCrich%20HB") != std::string::npos);

  JsonDocument doc;
  std::vector<Found> found;
  std::string error;
  const std::string faa = R"({"features":[{"attributes":{"IDENT":"JFK","ICAO_ID":"KJFK","NAME":"JOHN F KENNEDY INTL",
      "SERVCITY":"NEW YORK","STATE":"NY"},"geometry":{"x":-73.7789,"y":40.6398}},{"attributes":{"IDENT":"X"}}]})";
  CHECK(read_faa(faa.c_str(), faa.size(), doc, found, error) && found.size() == 1);
  CHECK(found[0].name == "KJFK" && found[0].airport && near(found[0].lat, 40.6398, 1e-5) &&
        found[0].detail == "KJFK JOHN F KENNEDY INTL, NEW YORK, NY");
  const std::string refused = R"({"error":{"code":400}})";
  CHECK(!read_faa(refused.c_str(), refused.size(), doc, found, error) && !error.empty());

  const std::string osm = R"([{"lat":"47.4581","lon":"8.5555","display_name":"Zurich Airport, Kloten, Switzerland",
      "class":"aeroway","type":"aerodrome","extratags":{"iata":"ZRH","icao":"LSZH"}},
      {"lat":"47.3769","lon":"8.5417","display_name":"Zürich, Bezirk Zürich, Switzerland","class":"boundary",
       "type":"administrative","extratags":{}},{"lat":"x","lon":"8"}])";
  std::vector<Found> places;
  CHECK(read_nominatim(osm.c_str(), osm.size(), "zurich", doc, places, error) && places.size() == 2);
  CHECK(places[0].name == "LSZH" && places[0].airport && places[1].name == "Zürich" && !places[1].airport);
  places.clear();
  CHECK(read_nominatim(osm.c_str(), osm.size(), "zrh", doc, places, error) && places[0].name == "ZRH");
  CHECK(!read_nominatim("{}", 2, "x", doc, places, error));
  // The FAA's first, then the airports, then the rest.
  std::reverse(places.begin(), places.end());
  const auto all = in_order(found, places);
  CHECK(all.size() == 3 && all[0].name == "KJFK" && all[1].name == "ZRH" && all[2].name == "Zürich");
}

static void test_pinch() {
  using namespace pinch;
  Settings st;
  st.lat = 40.64;
  st.lon = -73.78;
  st.range_nm = 20;
  st.map = RADAR;  // a view of any zoom, not only whole ones
  const geo::View from = tiles::plan(st, 600, 450, 1).view;
  CHECK(range_nm(from) == 20);
  Fingers p;
  p.start({200, 200}, {300, 200}, from);
  // Still fingers: the same view.
  geo::View v = p.moved({200, 200}, {300, 200});
  CHECK(near(v.zoom, from.zoom, 1e-9) && near(v.lat, from.lat, 1e-9) && near(v.lon, from.lon, 1e-9));
  // Twice as far apart: one zoom closer, half the range, and the place between them stays between them.
  v = p.moved({150, 200}, {350, 200});
  CHECK(near(v.zoom, from.zoom + 1, 1e-9) && range_nm(v) == 10);
  const geo::Place under = from.place(250, 200), now = v.place(250, 200);
  CHECK(near(under.lat, now.lat, 1e-6) && near(under.lon, now.lon, 1e-6));
  // Moved together: the map moves with them, the middle's place now 100 pixels right of the middle.
  v = p.moved({300, 200}, {400, 200});
  const geo::Pixel was_middle = v.point(from.lat, from.lon);
  CHECK(near(was_middle.x, 400, 1e-6) && near(was_middle.y, 225, 1e-6) && range_nm(v) == 20);
  // One finger (two the panel takes for one): it pans, and never zooms.
  Fingers one;
  one.start({300, 200}, {300, 200}, from);
  v = one.moved({250, 260}, {250, 260});
  const geo::Pixel middle_now = v.point(from.lat, from.lon);
  CHECK(near(v.zoom, from.zoom, 1e-9) && near(middle_now.x, 250, 1e-6) && near(middle_now.y, 285, 1e-6));
  // As far as the range goes, either way.
  CHECK(range_nm(p.moved({0, 200}, {599, 200 + 1})) >= MIN_RANGE_NM);
  CHECK(range_nm(p.moved({249, 200}, {250, 200})) >= MAX_RANGE_NM - 2);  // measured where the middle moved to
  CHECK(range_nm(p.moved({-50000, 200}, {50000, 200})) == MIN_RANGE_NM);
  // The frame drawn for one view, drawn again for another: every place lands where the other view has it.
  v = p.moved({120, 150}, {380, 260});
  const Transform t = transform(from, v);
  for (const geo::Place &q : {geo::Place{40.70, -73.90}, geo::Place{40.55, -73.60}, geo::Place{st.lat, st.lon}}) {
    const geo::Pixel a = from.point(q.lat, q.lon), b = v.point(q.lat, q.lon);
    CHECK(near(a.x * t.scale + t.dx, b.x, 1e-6) && near(a.y * t.scale + t.dy, b.y, 1e-6));
  }
  CHECK(transform(from, from).identity() && !t.identity());
  // Across the date line, the short way.
  Settings fiji = st;
  fiji.lat = -17;
  fiji.lon = 179.9;
  const geo::View east = tiles::plan(fiji, 600, 450, 1).view;
  fiji.lon = -179.9;
  const geo::View west = tiles::plan(fiji, 600, 450, 1).view;
  CHECK(std::fabs(transform(east, west).dx) < 600);

  // Stretched: as it was, shifted with the ground where nothing was, and twice as large about the corner.
  const int w = 8, h = 6;
  std::vector<uint16_t> frame(w * h), out(w * h);
  for (int i = 0; i < w * h; ++i) frame[i] = (uint16_t) (i + 1);
  std::vector<int> columns;
  stretch(frame.data(), out.data(), w, h, Transform{}, 0, columns);
  CHECK(out == frame);
  stretch(frame.data(), out.data(), w, h, Transform{1, 2, -1}, 0, columns);
  CHECK(out[0] == 0 && out[1] == 0 && out[2] == frame[1 * w + 0] && out[(h - 1) * w + 3] == 0);
  stretch(frame.data(), out.data(), w, h, Transform{2, 0, 0}, 0, columns);
  CHECK(out[0] == frame[0] && out[1] == frame[0] && out[2] == frame[1] && out[w * 2 + 3] == frame[w + 1]);
}

static void test_swipe() {
  using namespace swipe;
  // The strip along the bottom: its last `strip_px` rows, nothing above.
  CHECK(in_strip(449, 450, 73) && in_strip(377, 450, 73) && !in_strip(376, 450, 73) && !in_strip(450, 450, 73));
  // Sideways far enough, and more sideways than not: to the left the next map, to the right the one before.
  CHECK(direction(300, 420, 180, 430, 98) == 1 && direction(180, 420, 300, 410, 98) == -1);
  CHECK(direction(300, 420, 250, 420, 98) == 0);   // too short
  CHECK(direction(300, 440, 180, 370, 98) == 0);   // not twice as far across as up
  // The maps in the menu's order, the custom server only where there is one; a swipe steps as the menu's arrows do.
  Settings s;
  auto all = maps(s);
  CHECK(all.size() == 8 && all.front() == RADAR && all.back() == IFR_HIGH);
  s.custom_url = "https://t.example/{z}/{x}/{y}.png";
  all = maps(s);
  CHECK(all.size() == 9 && all.back() == CUSTOM);
  s.map = RADAR;
  CHECK(menu::stepped(map_row(), s, 1) && s.map == STREETS);
  CHECK(menu::stepped(map_row(), s, -1) && menu::stepped(map_row(), s, -1) && s.map == CUSTOM);
}

static void test_wifi_list() {
  using namespace wifi_list;
  List l;
  // The first network ESPHome starts with joins the list; known as it is, nothing changes.
  CHECK(l.merge("Home", "secret12") && l.networks.size() == 1);
  CHECK(!l.merge("Home", "secret12") && !l.merge("", "x"));
  // Networks joined go first; one joined again goes first with its new password, the rest keep their order.
  CHECK(l.add_first("Cabin", "pinecone") && l.add_first("Office", "work1234"));
  CHECK(l.networks[0].name == "Office" && l.networks[1].name == "Cabin" && l.networks[2].name == "Home");
  CHECK(l.add_first("Home", "newsecret") && l.networks[0].name == "Home" && l.networks[0].password == "newsecret" &&
        l.networks[1].name == "Office" && l.networks.size() == 3);
  CHECK(!l.add_first("Home", "newsecret"));  // already first, as it is
  // ESPHome given a known network with another password (its setup page): first, with it.
  CHECK(l.merge("Cabin", "pinecone2") && l.networks[0].name == "Cabin" && l.networks[0].password == "pinecone2");
  // Moved, within the list only.
  CHECK(l.move(0, 1) && l.networks[1].name == "Cabin" && !l.move(0, -1) && !l.move(2, 1) && !l.move(9, 1));
  // Earlier is higher to ESPHome.
  CHECK(l.priority(0) > l.priority(1) && l.priority(1) > l.priority(2) && l.priority(2) > 0);
  // Forgotten, but never the last.
  CHECK(l.forget(2) && l.networks.size() == 2 && l.forget(0) && !l.forget(0) && l.networks.size() == 1);
  // No more than MOST: the last choice goes.
  for (int i = 0; i < 10; ++i) l.add_first("Net" + std::to_string(i), "password");
  CHECK(l.networks.size() == MOST && l.networks[0].name == "Net9" && l.find("Net1") < 0);
  // Names and passwords as long as ESPHome takes them, and no longer.
  CHECK(!l.add_first(std::string(33, 'n'), "") && !l.add_first("x", std::string(65, 'p')));
  CHECK(l.add_first(std::string(32, 'n'), std::string(64, 'p')));
  // Stored and read back the same; bytes of something else read as no networks.
  const List back = from(stored(l));
  CHECK(back.networks.size() == l.networks.size());
  for (size_t i = 0; i < l.networks.size(); ++i)
    CHECK(back.networks[i].name == l.networks[i].name && back.networks[i].password == l.networks[i].password);
  Stored junk = stored(l);
  junk.check = 0;
  CHECK(from(junk).networks.empty());
  junk = stored(l);
  junk.count = 200;
  CHECK(from(junk).networks.size() == MOST);
}

static void test_weather() {
  using namespace weather;
  JsonDocument doc;
  std::string error;
  CHECK(station_id(" kjfk ") == "KJFK" && station_id("JFK") == "JFK" && station_id("K1") == "" && station_id("KJ-K") == "");
  CHECK(metar_url("KJFK").find("ids=KJFK") != std::string::npos && mos_url("KJFK").find("station=KJFK") != std::string::npos);
  CHECK(nearby_url(40.64, -73.78).find("bbox=40.14,-74.") != std::string::npos);
  // METARs as the Aviation Weather Center gives them (2026-10-30), the nearest to a place, and in words.
  const std::string metars = R"([{"icaoId": "KJFK", "obsTime": 1793346660, "temp": 13.9, "dewp": 12.2, "wdir": 240,
    "wspd": 12, "visib": "10+", "altim": 1017.4, "rawOb": "METAR KJFK 300751Z 24012KT 10SM FEW130 SCT250 14/12 A3004",
    "lat": 40.6392, "lon": -73.7639, "name": "New York/JF Kennedy Intl, NY, US", "clouds": [{"cover": "FEW", "base":
    13000}, {"cover": "BKN", "base": 2500}], "fltCat": "MVFR"}, {"icaoId": "KTEB", "obsTime": 1793346660, "temp": 12.8,
    "dewp": 11.1, "wdir": "VRB", "wspd": 3, "wgst": null, "visib": 1.5, "altim": 1016.3, "rawOb": "METAR KTEB", "lat":
    40.859, "lon": -74.0562, "clouds": [], "fltCat": "IFR"}])";
  std::vector<Metar> m;
  CHECK(read_metars(metars.c_str(), metars.size(), doc, m, error) && m.size() == 2);
  CHECK(m[0].id == "KJFK" && m[0].category == MVFR && m[0].wind_dir == 240 && m[0].wind_kt == 12 && !m[0].variable);
  CHECK(m[1].variable && m[1].wind_kt == 3 && m[1].visibility == "1.5" && m[1].category == IFR);
  CHECK(nearest(m, 40.85, -74.05) == 1 && nearest(m, 40.6, -73.7) == 0 && nearest({}, 0, 0) == -1);
  CHECK(ceiling_ft(m[0]) == 2500 && ceiling_ft(m[1]) == -1);
  const auto words = decoded(m[0], false);
  CHECK(words.size() == 6 && words[0] == "Wind 240\xC2\xB0 at 12 kt" && words[1] == "Visibility 10 SM or more");
  CHECK(words[3] == "Ceiling 2500 ft" && words[4].find("57\xC2\xB0" "F") != std::string::npos &&
        words[5].find("30.04 inHg") != std::string::npos);
  CHECK(decoded(m[1], true)[0] == "Wind variable, 3 kt" && decoded(m[1], true)[2].find("13\xC2\xB0" "C") != std::string::npos);
  CHECK(when(1793346660, 1793346660 + 23 * 60) == "0751Z, 23 min ago" && when(1793346660, 0) == "0751Z");
  CHECK(read_metars("", 0, doc, m, error) && m.empty());  // no stations near
  CHECK(!read_metars("<html>", 6, doc, m, error));
  // A TAF a line a change; PROB30 TEMPO is one.
  const auto lines = taf_lines("TAF KJFK 300522Z 3006/3112 26008KT P6SM SKC FM301500 31015G22KT P6SM SKC "
                               "TEMPO 3016/3018 BKN020 PROB30 TEMPO 3020/3022 TSRA BECMG 3100/3102 33012KT");
  CHECK(lines.size() == 5 && lines[1].rfind("FM301500", 0) == 0 && lines[2].rfind("TEMPO", 0) == 0 &&
        lines[3].rfind("PROB30 TEMPO", 0) == 0 && lines[4].rfind("BECMG", 0) == 0);
  // The GFS MOS as the Iowa Environmental Mesonet gives it: each hour, and the flight category its ceiling and
  // visibility categories make.
  const std::string mos = R"({"schema": {}, "data": [{"ftime": "2026-10-30 06:00", "tmp": 58, "dpt": 55, "cld": "FW",
    "wdr": 230, "wsp": 9, "p06": null, "cig": 8, "vis": 7}, {"ftime": "2026-10-30 12:00", "tmp": 59, "dpt": 56, "cld":
    "BK", "wdr": 260, "wsp": 9, "p06": 3.0, "cig": 4, "vis": 7}, {"ftime": "2026-10-30 15:00", "tmp": null}]})";
  std::vector<MosRow> rows;
  CHECK(read_mos(mos.c_str(), mos.size(), doc, rows, error) && rows.size() == 2);
  CHECK(rows[0].at == 1793340000 && rows[1].pop6 == 3 && rows[0].pop6 == -1);
  CHECK(utc_seconds("2026-10-30T06:00:00.000Z") == 1793340000 && utc_seconds("soon") == 0);
  const std::string taf = R"([{"icaoId": "KJFK", "issueTime": "2026-10-30T05:22:00.000Z", "rawTAF": "TAF KJFK"}])";
  Taf t;
  CHECK(read_taf(taf.c_str(), taf.size(), doc, t, error) && t.issued == 1793340000 - 38 * 60 && t.raw == "TAF KJFK");
  CHECK(category_of(rows[0]) == VFR && category_of(rows[1]) == MVFR);
  CHECK(mos_line(rows[1]) == "12Z  59/56\xC2\xB0" "F  260/9 kt  BK  MVFR  3% precip");
  MosRow low = rows[0];
  low.cig = 2;
  CHECK(category_of(low) == LIFR);
  low.cig = 8;
  low.vis = 4;
  CHECK(category_of(low) == IFR);
  // Advisories over the place only: a SIGMET round it, another far away; G-AIRMETs of the hour valid now, each
  // hazard once.
  const std::string sigmets = R"([{"airSigmetType": "SIGMET", "hazard": "CONVECTIVE", "altitudeHi1": 40000,
    "altitudeLow1": null, "validTimeTo": 1793354100, "rawAirSigmet": "CONVECTIVE SIGMET 25E", "coords": [{"lat": 41.5,
    "lon": -75}, {"lat": 41.5, "lon": -72}, {"lat": 39.5, "lon": -72}, {"lat": 39.5, "lon": -75}]}, {"airSigmetType":
    "SIGMET", "hazard": "CONVECTIVE", "coords": [{"lon": -93.461, "lat": 42.347}, {"lon": -93.559, "lat": 38.894},
    {"lon": -95.477, "lat": 38.577}]}])";
  std::vector<Advisory> advisories;
  CHECK(read_sigmets(sigmets.c_str(), sigmets.size(), doc, 40.64, -73.78, 20, advisories, error) && advisories.size() == 1);
  CHECK(advisory_line(advisories[0]) == "SIGMET CONVECTIVE, to FL400, until 0955Z");
  const std::string gairmets = R"([{"forecastHour": 3, "hazard": "ICE", "base": "120", "top": "240", "severity": "MOD",
    "expireTime": 1793350800, "coords": [{"lat": "40.5", "lon": "-74.5"}, {"lat": "41", "lon": "-73"}, {"lat": "40",
    "lon": "-73"}]}, {"forecastHour": 3, "hazard": "ICE", "coords": [{"lat": "40.6", "lon": "-73.8"}]},
    {"forecastHour": 6, "hazard": "TURB-HI", "coords": [{"lat": "40.6", "lon": "-73.8"}]}, {"forecastHour": 3,
    "hazard": "IFR", "due_to": "CIG BLW 010", "coords": [{"lat": "30", "lon": "-90"}, {"lat": "31", "lon": "-90"},
    {"lat": "30", "lon": "-89"}]}])";
  CHECK(read_gairmets(gairmets.c_str(), gairmets.size(), doc, 40.64, -73.78, 20, advisories, error) &&
        advisories.size() == 2 && advisories[1].hazard == "ICE");
  CHECK(advisory_line(advisories[1]) == "G-AIRMET ICE, 12000 ft to FL240, MOD, until 0900Z");
  CHECK(near({{0, 0}, {0, 2}, {2, 2}, {2, 0}}, 1, 1, 1) && !near({{0, 0}, {0, 2}, {2, 2}, {2, 0}}, 5, 5, 10));
  // Pilot reports, the latest first; none is an empty body.
  const std::string pireps = R"([{"rawOb": "JFK UA /OV JFK/TM 0730/FL050/TP B738/SK OVC040", "acType": "B738",
    "fltLvl": 50, "lat": 40.6, "lon": -73.8, "obsTime": 1793345400}, {"rawOb": "LGA UA /OV LGA", "obsTime": 1793346000}])";
  std::vector<Pirep> reports;
  CHECK(read_pireps(pireps.c_str(), pireps.size(), doc, reports, error) && reports.size() == 2 &&
        reports[0].raw == "LGA UA /OV LGA" && reports[1].level_ft == 5000);
  CHECK(read_pireps("", 0, doc, reports, error) && reports.empty());
}

static void test_radar() {
  using namespace radar;
  JsonDocument doc;
  // IEM's note of the latest scan, its time as IEM names it, and an hour of them for a loop.
  const std::string latest = R"({"meta": {"vcp": null, "product": "N0Q", "valid": "2026-10-30T08:10:00Z"}})";
  const uint32_t at = read_latest(latest.c_str(), latest.size(), doc);
  CHECK(at == 1793347800 / 60 && stamp(at) == "202610300810" && clock(at) == "0810Z");
  CHECK(read_latest("{}", 2, doc) == 0 && read_latest("x", 1, doc) == 0);
  CHECK(stamp(0) == "197001010000" && stamp(26092800) == "201908120000");
  const auto one = frames(at, false), hour = frames(at, true);
  CHECK(one.size() == 1 && one[0] == at && hour.size() == 12 && hour.back() == at && hour.front() == at - 55);
  CHECK(frames(0, true).empty());
  // A radar tile for each square of the map, not of its place names; asked for by its scan.
  Settings s;
  s.lat = 40.64;
  s.lon = -73.78;
  s.map = STREETS;
  const tiles::Plan p = tiles::plan(s, 600, 450, 2);
  const auto keys = radar::keys(p, at);
  size_t maps = 0;
  for (const auto &k : p.keys) maps += k.layer == 0;
  CHECK(!keys.empty() && keys.size() == maps && keys[0].layer == LAYER && keys[0].time == at);
  CHECK(tile_url(keys[0]).find("ridge::USCOMP-N0Q-202610300810/" + std::to_string(keys[0].z) + "/") != std::string::npos);
  // Another scan is another tile.
  tiles::Key later = keys[0];
  later.time += STEP_MINUTES;
  CHECK(!(later == keys[0]));
  CHECK(filter().opacity == OPACITY && filter().opacity < 255);
  // What a scan changed, kept as runs and put back exactly; past the most pixels, not kept at all.
  std::vector<uint16_t> clean(100, 7), with = clean;
  for (int i : {0, 1, 2, 40, 41, 99}) with[i] = (uint16_t) (100 + i);
  Overlay o;
  CHECK(difference(with.data(), clean.data(), 100, 10, o) && o.starts.size() == 3 && o.pixels.size() == 6);
  std::vector<uint16_t> back = clean;
  apply(o, back.data());
  CHECK(back == with);
  CHECK(!difference(with.data(), clean.data(), 100, 5, o) && o.pixels.empty() && o.starts.empty());
  CHECK(difference(clean.data(), clean.data(), 100, 0, o) && o.starts.empty());
}

static void test_schedule() {
  using namespace schedule;
  Zone z;
  CHECK(parse("PST8PDT,M3.2.0,M11.1.0", z) && z.std_offset_seconds == 8 * 3600 && z.dst_offset_seconds == 7 * 3600);
  CHECK(z.dst_start.type == MONTH_WEEK_DAY && z.dst_start.month == 3 && z.dst_start.week == 2 && z.dst_start.day_of_week == 0 &&
        z.dst_start.time_seconds == 7200 && z.dst_end.month == 11 && z.dst_end.week == 1);
  CHECK(parse("CET-1CEST,M3.5.0,M10.5.0/3", z) && z.std_offset_seconds == -3600 && z.dst_offset_seconds == -7200 &&
        z.dst_end.time_seconds == 3 * 3600);
  CHECK(parse("IST-5:30", z) && z.std_offset_seconds == -(5 * 3600 + 1800) && z.dst_start.type == NONE);
  CHECK(parse("<+0545>-5:45", z) && z.std_offset_seconds == -(5 * 3600 + 45 * 60));
  CHECK(parse("EST5EDT4,M3.2.0/2:00:00,M11.1.0/2:00:00", z) && z.dst_offset_seconds == 4 * 3600);
  CHECK(parse("AEST-10AEDT,M10.1.0,M4.1.0/3", z) && z.dst_start.month == 10 && z.dst_end.month == 4);
  CHECK(parse("UTC0", z) && z.std_offset_seconds == 0 && z.dst_start.type == NONE);
  CHECK(parse("XXX3YYY,J60/-1,300/26", z) && z.dst_start.type == JULIAN_NO_LEAP && z.dst_start.day == 60 &&
        z.dst_start.time_seconds == -3600 && z.dst_end.type == DAY_OF_YEAR && z.dst_end.time_seconds == 26 * 3600);
  for (const char *bad : {"", "PST", "P8", "PST8PDT,M13.2.0,M11.1.0", "PST8PDT,M3.2.0", "PST8PDT,M3.2.0,M11.1.0x", "PST99"})
    CHECK(!parse(bad, z));
  CHECK(minutes("22:30") == 22 * 60 + 30 && minutes("7:05") == 7 * 60 + 5 && minutes("24:00") == -1 && minutes("1230") == -1);
  // Over midnight: dark from 22:00 to 07:00.
  CHECK(within(23 * 60, 22 * 60, 7 * 60) && within(3 * 60, 22 * 60, 7 * 60) && !within(7 * 60, 22 * 60, 7 * 60) &&
        !within(12 * 60, 22 * 60, 7 * 60));
  // Within a day: dark from 13:00 to 14:00.
  CHECK(within(13 * 60, 13 * 60, 14 * 60) && !within(14 * 60, 13 * 60, 14 * 60) && !within(5, 13 * 60, 14 * 60));
  CHECK(!within(600, 600, 600));
  Settings st;
  JsonDocument in;
  std::string error;
  deserializeJson(in, R"({"timezone":"bogus","bed_off":"23:00"})");
  CHECK(!apply(in.as<JsonObjectConst>(), st, error) && error == "timezone" && st.bed_off == "22:00");
  deserializeJson(in, R"({"timezone":"CET-1CEST,M3.5.0,M10.5.0/3","bed_off":"23:15","bed_on":"6:45","bedtime":true})");
  CHECK(apply(in.as<JsonObjectConst>(), st, error) && st.bedtime && st.bed_off == "23:15" && st.bed_on == "6:45");
  deserializeJson(in, R"({"bed_on":"25:00"})");
  CHECK(!apply(in.as<JsonObjectConst>(), st, error) && error == "bed_on");
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

  // On round glass a plane near the circle's edge has its label on the glass, not in the corner beyond it.
  std::vector<plan::Label> rim(1);
  rim[0] = {{620, 620}, 6, 60, 20, false};
  const auto square = plan::place_labels(rim, 720, 720, 4, 20);
  CHECK(square[0].shown && square[0].spot == 0);  // beside it, to the right, in the corner
  const auto round = plan::place_labels(rim, 720, 720, 4, 20, {}, true);
  const glass::Shape circle{720, 720, true};
  CHECK(round[0].shown && round[0].spot != 0);
  const plan::Box &b = round[0].box;
  CHECK(glass::holds_box(circle, b.x, b.y, b.w, b.h));
}

// A message from its hex, as the examples print them.
static std::vector<uint8_t> hex_bytes(const char *hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out.push_back((uint8_t) std::strtoul(std::string(hex + i, 2).c_str(), nullptr, 16));
  return out;
}

// Samples of `msgs` one after another, through the demodulator in blocks of `block` pairs: the messages it accepted.
static std::vector<modes::Message> demodulate(const std::vector<std::vector<uint8_t>> &msgs, size_t block,
                                              int low = 2, int high = 60) {
  std::vector<uint8_t> iq;
  for (const auto &m : msgs) {
    const auto one = modes::samples_of(m.data(), (int) m.size() * 8, low, high);
    iq.insert(iq.end(), one.begin(), one.end());
  }
  // Quiet after, so the last one is read whole.
  iq.insert(iq.end(), 2 * 600, 128);
  static const modes::Powers powers;
  modes::Demodulator demod;
  std::vector<modes::Message> found;
  std::vector<uint16_t> p(block);
  for (size_t at = 0; at < iq.size() / 2; at += block) {
    const size_t n = std::min(block, iq.size() / 2 - at);
    powers.convert(iq.data() + 2 * at, n, p.data());
    demod.feed(p.data(), n, [&](const modes::Message &m) {
      found.push_back(m);
      return true;
    });
  }
  return found;
}

static void test_modes() {
  const modes::Crc crc;
  // The examples' parity holds; one bit flipped is found and put right.
  const char *examples[] = {"8D4840D6202CC371C32CE0576098", "8D40621D58C382D690C8AC2863A7",
                            "8D40621D58C386435CC412692AD6", "8D485020994409940838175B284F",
                            "8DA05F219B06B6AF189400CBC33F"};
  for (const char *e : examples) CHECK(crc.residue(hex_bytes(e).data(), 112) == 0);
  auto bad = hex_bytes(examples[0]);
  bad[6] ^= 0x10;  // bit 51
  CHECK(crc.single_bit(crc.residue(bad.data(), 112)) == 51);

  // Each example through the samples a radio would give, in blocks small and large, comes back as it went.
  std::vector<std::vector<uint8_t>> msgs;
  for (const char *e : examples) msgs.push_back(hex_bytes(e));
  for (size_t block : {(size_t) 97, (size_t) 4096, (size_t) 100000}) {
    const auto found = demodulate(msgs, block);
    CHECK(found.size() == msgs.size());
    for (size_t i = 0; i < found.size() && i < msgs.size(); ++i) {
      CHECK(found[i].df == 17 && found[i].bits == 112 && !found[i].overlaid && !found[i].mended);
      CHECK(std::memcmp(found[i].data, msgs[i].data(), 14) == 0);
    }
  }
  const auto first = demodulate(msgs, 4096);
  CHECK(!first.empty() && first[0].address == 0x4840D6);
  // One bit wrong in the air: mended.
  const auto mended = demodulate({bad}, 4096);
  CHECK(mended.size() == 1 && mended[0].mended && std::memcmp(mended[0].data, msgs[0].data(), 14) == 0);
  // Two bits wrong: let go.
  auto worse = bad;
  worse[9] ^= 0x01;
  CHECK(demodulate({worse}, 4096).empty());
  // A weak one, its pulses a little over the noise: read; one under it: not.
  CHECK(demodulate(msgs, 4096, 8, 40).size() == msgs.size());
  CHECK(demodulate(msgs, 4096, 20, 22).empty());

  // An all-call reply (DF11) with its parity, and a surveillance reply (DF4) whose parity carries its address.
  uint8_t df11[7] = {0x5D, 0x48, 0x40, 0xD6, 0, 0, 0};
  const uint32_t p11 = crc.residue(df11, 56);
  df11[4] = (uint8_t) (p11 >> 16);
  df11[5] = (uint8_t) (p11 >> 8);
  df11[6] = (uint8_t) p11;
  CHECK(crc.residue(df11, 56) == 0);
  const int feet = 38000, n = (feet + 1000) / 25;
  const uint32_t ac = ((n & 0x7E0) << 2) | ((n & 0x10) << 1) | (n & 0xF) | 0x10;
  uint8_t df4[7] = {0x20, 0x00, (uint8_t) (ac >> 8), (uint8_t) ac, 0, 0, 0};
  const uint32_t p4 = crc.residue(df4, 56) ^ 0x4840D6;
  df4[4] = (uint8_t) (p4 >> 16);
  df4[5] = (uint8_t) (p4 >> 8);
  df4[6] = (uint8_t) p4;
  const auto replies = demodulate({std::vector<uint8_t>(df11, df11 + 7), std::vector<uint8_t>(df4, df4 + 7)}, 4096);
  CHECK(replies.size() == 2);
  if (replies.size() == 2) {
    CHECK(replies[0].df == 11 && replies[0].address == 0x4840D6 && !replies[0].overlaid);
    CHECK(replies[1].df == 4 && replies[1].address == 0x4840D6 && replies[1].overlaid);
  }
}

static void test_adsb() {
  // Fields.
  CHECK(adsb::callsign(hex_bytes("8D4840D6202CC371C32CE0576098").data()) == "KLM1023");
  int feet = 0;
  CHECK(adsb::altitude12(adsb::me(hex_bytes("8D40621D58C382D690C8AC2863A7").data(), 9, 12), feet) && feet == 38000);
  const int n = (38000 + 1000) / 25;
  CHECK(adsb::altitude13(((n & 0x7E0) << 2) | ((n & 0x10) << 1) | (n & 0xF) | 0x10, feet) && feet == 38000);
  CHECK(!adsb::altitude13(0x0040 | 0x10, feet));  // metres
  CHECK(adsb::squawk13((1 << 11) | (1 << 9) | (1 << 7) | (1 << 5) | (1 << 3) | (1 << 1)) == "7700");
  CHECK(adsb::squawk13(1 << 0) == "0004" && adsb::squawk13(1 << 12) == "0010" && adsb::squawk13(1 << 5) == "0100");
  CHECK(adsb::movement(1) == 0 && adsb::movement(13) == 2 && adsb::movement(39) == 15 && adsb::movement(0) < 0);
  CHECK(adsb::zones(0) == 59 && adsb::zones(52.2572) == 36 && adsb::zones(87) == 2 && adsb::zones(88) == 1);

  // A place from an even and an odd report (the even one newer), and from one report and a place near it.
  adsb::Tracker t;
  modes::Message m;
  auto msg = [&](const char *hex) {
    m = modes::Message{};
    const auto b = hex_bytes(hex);
    std::memcpy(m.data, b.data(), b.size());
    m.bits = 112;
    m.df = modes::df_of(m.data);
    m.address = modes::address_field(m.data);
    return m;
  };
  CHECK(t.add(msg("8D40621D58C386435CC412692AD6"), 1000));  // odd
  CHECK(t.flights(1000).empty());
  CHECK(t.add(msg("8D40621D58C382D690C8AC2863A7"), 2000));  // even
  auto seen = t.flights(2000);
  CHECK(seen.size() == 1);
  if (!seen.empty()) {
    CHECK(seen[0].hex == "40621d" && seen[0].has_altitude && seen[0].altitude == 38000 && !seen[0].on_ground);
    CHECK(near(seen[0].lat, 52.2572, 1e-3) && near(seen[0].lon, 3.91937, 1e-3));
  }
  adsb::Cpr even;
  const auto e = hex_bytes("8D40621D58C382D690C8AC2863A7");
  even.lat = adsb::me(e.data(), 23, 17);
  even.lon = adsb::me(e.data(), 40, 17);
  double lat, lon;
  CHECK(adsb::local(even, 52.258, 3.918, lat, lon) && near(lat, 52.2572, 1e-3) && near(lon, 3.91937, 1e-3));
  // Reports too far apart in time make no place.
  adsb::Tracker slow;
  slow.add(msg("8D40621D58C386435CC412692AD6"), 1000);
  slow.add(msg("8D40621D58C382D690C8AC2863A7"), 1000 + adsb::PAIR_MS + 1);
  CHECK(slow.flights(1000 + adsb::PAIR_MS + 1).empty());
  // A place farther from the receiver than any radio hears is thrown away.
  adsb::Tracker far;
  far.set_reference(40.64, -73.78);
  far.add(msg("8D40621D58C386435CC412692AD6"), 1000);
  far.add(msg("8D40621D58C382D690C8AC2863A7"), 2000);
  CHECK(far.flights(2000).empty() && far.counts().bad_positions == 1);

  // Identity and velocity on the same plane.
  t.add(msg("8D4840D6202CC371C32CE0576098"), 2000);
  const auto &klm = t.planes().at(0x4840D6);
  CHECK(klm.callsign == "KLM1023");
  t.add(msg("8D485020994409940838175B284F"), 2000);
  const auto &v = t.planes().at(0x485020);
  CHECK(v.has_speed && near(v.speed, 159.2, 0.1) && v.has_heading && near(v.heading, 182.88, 0.01));
  CHECK(v.has_climb && v.climb == -832);
  t.add(msg("8DA05F219B06B6AF189400CBC33F"), 2000);
  const auto &as = t.planes().at(0xA05F21);
  CHECK(as.has_speed && as.speed == 375 && as.has_heading && near(as.heading, 243.98, 0.01) && as.climb == -2304);

  // An overlaid message counts only for a plane heard by its own address lately.
  modes::Message overlaid;
  overlaid.df = 5;
  overlaid.bits = 56;
  overlaid.overlaid = true;
  overlaid.address = 0x123456;
  CHECK(!t.add(overlaid, 2000) && t.counts().unknown == 1);
  overlaid.address = 0x40621D;
  CHECK(t.add(overlaid, 2000));
  CHECK(!t.known(0x40621D, 2000 + adsb::FORGET_MS));
  // A minute without a word: forgotten.
  t.expire(2000 + adsb::FORGET_MS);
  CHECK(t.size() == 0 && t.flights(2000 + adsb::FORGET_MS).empty());
}

static void test_glass() {
  const glass::Shape rect{600, 450, false}, circle{720, 720, true};
  // A rectangle holds its corners; a circle does not.
  CHECK(glass::holds(rect, 0, 0) && glass::holds(rect, 600, 450) && !glass::holds(rect, 601, 0));
  CHECK(glass::holds(circle, 360, 0) && glass::holds(circle, 0, 360) && !glass::holds(circle, 0, 0));
  CHECK(!glass::holds(circle, 40, 40) && glass::holds(circle, 40, 40, 400));
  CHECK(glass::holds_box(rect, 0, 0, 600, 450) && !glass::holds_box(rect, 1, 0, 600, 450));
  CHECK(glass::holds_box(circle, 260, 260, 200, 200) && !glass::holds_box(circle, 0, 300, 100, 20));

  // Nothing moves in from the edge of a rectangle; on a circle, the wider the box the further in.
  CHECK(glass::edge_inset(rect, 500) == 0);
  CHECK(glass::edge_inset(circle, 0) == 0);
  const int narrow = glass::edge_inset(circle, 200), wide = glass::edge_inset(circle, 500);
  CHECK(narrow > 0 && wide > narrow && glass::edge_inset(circle, 800) == 360);
  // A box that wide, that far in, has its corners on the glass, and one pixel less would not.
  CHECK(glass::holds_box(circle, 360 - 250, wide, 500, 30) && glass::holds_box(circle, 360 - 250, 720 - wide - 30, 500, 30));
  CHECK(!glass::holds_box(circle, 360 - 250, wide - 2, 500, 30));

  // A page: padded alike on a rectangle; on a circle its content is the largest square inside it.
  const glass::Insets r = glass::page(rect, 6);
  CHECK(r.left == 6 && r.top == 6 && r.right == 6 && r.bottom == 6 && glass::page_width(rect, 6) == 588);
  const glass::Insets c = glass::page(circle, 0);
  const int side = glass::page_width(circle, 0);
  CHECK(near(side, 720 / std::sqrt(2.0), 2) && c.left == c.top && c.right == c.bottom);
  CHECK(glass::holds_box(circle, c.left, c.top, side, 720 - c.top - c.bottom));
  CHECK(glass::page_width(circle, 10) == side - 20);
}

static void test_pixels() {
  using namespace pixels;
  static constexpr uint16_t WHITE = 0xFFFF, BLACK = 0, RED = 0xF800, BLUE = 0x001F;
  // Opaque replaces, clear keeps, half and half is half.
  CHECK(blend(BLACK, RED, 255) == RED && blend(BLUE, RED, 0) == BLUE);
  const uint16_t grey = blend(BLACK, WHITE, 128);
  CHECK((grey >> 11) >= 15 && (grey >> 11) <= 16 && (grey >> 5 & 0x3F) >= 31 && (grey >> 5 & 0x3F) <= 32);
  CHECK(mix4(RED, BLUE, BLUE, BLUE, 256, 0, 0, 0) == RED);
  const uint16_t mixed = mix4(WHITE, BLACK, WHITE, BLACK, 64, 64, 64, 64);
  CHECK((mixed >> 11) == 15 && (mixed & 0x1F) == 15);

  // A 4 x 4 tile (in a row of 6) of red, its last column clear.
  uint16_t colour[6 * 4];
  uint8_t alpha[6 * 4];
  for (int i = 0; i < 6 * 4; ++i) {
    colour[i] = RED;
    alpha[i] = (i % 6) == 3 ? 0 : 255;
  }
  const Tile tile{colour, alpha, 4, 4, 6};
  auto fresh = [](uint16_t *f, int n) { std::fill(f, f + n, BLUE); };
  // At its own pixels at (2, 2) on an 8 x 8 frame: the tile where it lies, its clear column and the rest untouched.
  uint16_t frame[8 * 8];
  fresh(frame, 64);
  lay(frame, 8, 8, 2, 2, 1.0, tile);
  CHECK(frame[2 * 8 + 2] == RED && frame[5 * 8 + 4] == RED);
  CHECK(frame[2 * 8 + 5] == BLUE && frame[1 * 8 + 2] == BLUE && frame[6 * 8 + 2] == BLUE);
  // Partly off the frame (above and left): clipped, nothing written outside it (the sanitizer watches).
  fresh(frame, 64);
  lay(frame, 8, 8, -2, -3, 1.0, tile);
  CHECK(frame[0] == RED && frame[1] == BLUE && frame[1 * 8 + 0] == BLUE);
  // Wholly off the frame: nothing.
  fresh(frame, 64);
  lay(frame, 8, 8, 20, 20, 1.0, tile);
  lay(frame, 8, 8, -10, 0, 2.0, tile);
  CHECK(std::all_of(frame, frame + 64, [](uint16_t p) { return p == BLUE; }));
  // Half size: the 4 x 4 tile in 2 x 2, its clear column falling under the right-hand pixels' middles.
  fresh(frame, 64);
  lay(frame, 8, 8, 0, 0, 0.5, tile);
  CHECK(frame[0] == RED && frame[8] == RED && frame[1] == BLUE && frame[2] == BLUE);
  // Twice the size: a 2 x 2 tile, left red and right white, in 4 x 4. The outer columns are their own colour, the
  // inner ones between the two, nearer their own side's.
  uint16_t two[4] = {RED, WHITE, RED, WHITE};
  uint8_t solid[4] = {255, 255, 255, 255};
  fresh(frame, 64);
  lay(frame, 8, 8, 0, 0, 2.0, Tile{two, solid, 2, 2, 2});
  CHECK(frame[0] == RED && frame[3] == WHITE);
  CHECK(frame[1] != RED && frame[1] != WHITE && (frame[1] & 0x1F) < (frame[2] & 0x1F));
  CHECK(frame[8 * 3 + 0] == RED && frame[4] == BLUE);  // its fourth row, and nothing past its width
  // A colour enlarged is the same colour.
  uint16_t one[4] = {RED, RED, RED, RED};
  fresh(frame, 64);
  lay(frame, 8, 8, 1, 1, 3.0, Tile{one, solid, 2, 2, 2});
  CHECK(frame[0] == BLUE && std::all_of(frame + 9, frame + 15, [](uint16_t p) { return p == RED; }));
}

static void test_touch() {
  using namespace touch;
  // A tap: down where it is for a moment, then up there.
  Player p;
  CHECK(!p.at(0).pressed);
  p.play(tap(10, 20));
  CHECK(p.busy() && p.at(1000).pressed && p.at(1000).x == 10 && p.at(1050).y == 20);
  CHECK(!p.at(1100).pressed && p.busy());  // up after 80 ms, for 60
  CHECK(!p.at(1200).pressed && p.at(1200).x == 10 && !p.busy());
  // On a loop slower than a tap, not lost: it starts at the first read, however late, and is up at the next.
  p.play(tap(3, 3));
  CHECK(p.busy() && p.at(500).pressed && !p.at(900).pressed && !p.busy());
  // A hold of a second and a half is down all that time.
  p.play(hold(5, 5, 1500));
  CHECK(p.at(0).pressed && p.at(1499).pressed && !p.at(1500).pressed);
  CHECK(gesture("hold", 1, 1, -1, -1, 0)[0].ms == 1200);  // a long press unless it says
  // A drag goes from its start to its end, down all the way, each point further on, and lifts at the end.
  const Script d = drag(100, 400, 100, 100, 600);
  CHECK(d.front().pressed && d.front().y == 400 && d.back().y == 100 && !d.back().pressed);
  int last = 401;
  bool down = true, nearer = true;
  for (size_t i = 0; i + 1 < d.size(); ++i) {
    down = down && d[i].pressed;
    nearer = nearer && d[i].y < last;
    last = d[i].y;
  }
  CHECK(down && nearer && last == 100);
  CHECK(length(d) >= 600 && length(d) < 800);
  // Across millis() wrapping.
  p.play(tap(1, 1));
  CHECK(p.at(0xFFFFFFF0u).pressed && p.at(0x10).pressed && !p.at(0x40).pressed && !p.at(0x80).pressed && !p.busy());
  CHECK(gesture("swipe", 0, 0, 0, 0, 0).empty());
}

static void test_firmware() {
  using namespace firmware;
  // Nothing to install: no start, nothing said.
  Tracker t;
  CHECK(!t.start(NO_UPDATE) && !t.updating);
  CHECK(!t.seen(NO_UPDATE, false) && t.error.empty());
  // A check that fails is said, and unsaid by one that works.
  t.seen(UNKNOWN, true);
  CHECK(t.error == SERVER_FAILED);
  t.seen(AVAILABLE, false);
  CHECK(t.error.empty());
  // Asked to install: updating, and the entity still saying AVAILABLE before it flashes is no failure.
  CHECK(t.start(AVAILABLE) && t.updating);
  CHECK(!t.start(AVAILABLE));  // one at a time
  CHECK(!t.seen(AVAILABLE, false) && t.updating && t.error.empty());
  CHECK(!t.seen(INSTALLING, false) && t.updating);
  // Flashing, then AVAILABLE again with the entity's error: the install failed, and the page says so, not "server".
  CHECK(t.seen(AVAILABLE, true) && !t.updating);
  CHECK(t.error == INSTALL_FAILED);
  // The word stays through later checks, failed or not, until the next install starts.
  CHECK(!t.seen(AVAILABLE, true) && t.error == INSTALL_FAILED);
  CHECK(!t.seen(AVAILABLE, false) && t.error == INSTALL_FAILED);
  CHECK(t.start(AVAILABLE) && t.error.empty());
  // A second failure is a failure too.
  t.seen(INSTALLING, false);
  CHECK(t.seen(NO_UPDATE, false) && t.error == INSTALL_FAILED);
  // The menu's line: installing over everything, then a check under way, an error, a release, the latest.
  CHECK(menu_line(true, true, true, true, "1.0", "x").text == "Installing 1.0...");
  CHECK(menu_line(true, true, true, false, "1.0", "x").text == "Checking...");
  CHECK(menu_line(false, false, false, false, "", SERVER_FAILED).tone == BAD_NEWS);
  CHECK(menu_line(false, true, true, false, "1.0", "").text == "1.0 is available");
  CHECK(menu_line(false, true, true, false, "1.0", "").tone == GOOD_NEWS);
  CHECK(menu_line(false, true, false, false, "1.0", "").text == "This is the latest release");
  CHECK(menu_line(false, false, false, false, "", "").text == "Not checked yet");
  // A check that finds nothing to install after a failed one: the failure no longer matters. (One that failed while
  // the entity said NO_UPDATE stays, as above.)
  Tracker w;
  w.start(AVAILABLE);
  w.seen(INSTALLING, false);
  CHECK(w.seen(AVAILABLE, true) && w.error == INSTALL_FAILED);
  CHECK(!w.seen(AVAILABLE, true) && w.error == INSTALL_FAILED);
  CHECK(!w.seen(NO_UPDATE, false) && w.error.empty());
  CHECK(!t.seen(NO_UPDATE, false) && t.error == INSTALL_FAILED);
  // A start a check failed before: the server's word goes with it.
  Tracker u;
  u.seen(AVAILABLE, true);
  CHECK(u.start(AVAILABLE) && u.error.empty());
}

int main(int argc, char **argv) {
  test_geo();
  test_settings();
  test_offline();
  test_card();
  test_scope();
  test_airspace();
  test_feed();
  test_tiles();
  test_frame_key();
  test_overlay();
  test_labels();
  test_glass();
  test_modes();
  test_adsb();
  test_sprites();
  test_shapes();
  test_beam();
  test_providers();
  test_menu();
  test_keyboard();
  test_places();
  test_pinch();
  test_swipe();
  test_wifi_list();
  test_weather();
  test_radar();
  test_schedule();
  test_firmware();
  test_touch();
  test_pixels();
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
