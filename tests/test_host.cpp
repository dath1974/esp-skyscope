// The parts of Skyscope that need no screen or network, on a PC: settings, the feed's JSON, the tile plan, which planes
// a frame shows and where, and the label placement. tests/run.sh builds and runs it; a reply saved from a feed can be
// given as an argument to read it too.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../components/skyscope/adsb.h"
#include "../components/skyscope/airspace.h"
#include "../components/skyscope/beam.h"
#include "../components/skyscope/card.h"
#include "../components/skyscope/emblem.h"
#include "../components/skyscope/ticks.h"
#include "../components/skyscope/feed.h"
#include "../components/skyscope/firmware.h"
#include "../components/skyscope/follow.h"
#include "../components/skyscope/geo.h"
#include "../components/skyscope/glass.h"
#include "../components/skyscope/http.h"
#include "../components/skyscope/pinch.h"
#include "../components/skyscope/keyboard.h"
#include "../components/skyscope/locate.h"
#include "../components/skyscope/menu_rows.h"
#include "../components/skyscope/numbers.h"
#include "../components/skyscope/modes.h"
#include "../components/skyscope/offline.h"
#include "../components/skyscope/overlay.h"
#include "../components/skyscope/pixels.h"
#include "../components/skyscope/photos.h"
#include "../components/skyscope/places.h"
#include "../components/skyscope/plan.h"
#include "../components/skyscope/providers.h"
#include "../components/skyscope/radar.h"
#include "../components/skyscope/schedule.h"
#include "../components/skyscope/sightings.h"
#include "../components/skyscope/sound.h"
#include "../components/skyscope/settings.h"
#include "../components/skyscope/sprites.h"
#include "../components/skyscope/swipe.h"
#include "../components/skyscope/tiles.h"
#include "../components/skyscope/today.h"
#include "../components/skyscope/touch.h"
#include "../components/skyscope/tour.h"
#include "../components/skyscope/weather.h"
#include "../components/skyscope/wifi_list.h"
#include "../components/skyscope/pngrow.h"

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
  CHECK(near(20 * geo::NM / r.meters_per_px(), 225, 0.5));
  CHECK(near(geo::meters_between(51.47, -0.4543, 40.64, -73.78) / 1000, 5540, 15));
  // place() is point() the other way.
  const geo::Place back = r.place(17, 431);
  const geo::Pixel there = r.point(back.lat, back.lon);
  CHECK(near(there.x, 17, 1e-6) && near(there.y, 431, 1e-6));
  CHECK(near(r.place(300, 225).lat, 40.64, 1e-9) && near(r.place(300, 225).lon, -73.78, 1e-9));
}

static void test_scope() {
  // The radar's scope is on unless switched off, and its beam turns only around it, on the radar. A screen starts on
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
  CHECK(near(geo::meters_between(r.a.lat, r.a.lon, r.b.lat, r.b.lon) / 0.3048, 10083, 60));
  const double mid_lat = (r.a.lat + r.b.lat) / 2, mid_lon = (r.a.lon + r.b.lon) / 2;
  CHECK(near(mid_lat, 42.364465, 1e-4) && near(mid_lon, -71.00474, 1e-4));
  CHECK(std::min(r.a.lat, r.b.lat) < 42.3548 && std::max(r.a.lat, r.b.lat) > 42.3741);
  CHECK(!map.runways[1].towered && !airspace::lengthened(map.runways[1]));
  CHECK(near(geo::meters_between(map.runways[1].a.lat, map.runways[1].a.lon, map.runways[1].b.lat, map.runways[1].b.lon),
             1112, 5));

  // The server's own word for a bad query; a reply that is no JSON, or cut short.
  const std::string refused = R"({"error":{"code":400,"message":"Cannot perform query. Invalid query parameters.","details":[]}})";
  airspace::Map none;
  CHECK(!airspace::read_airspace(refused.c_str(), refused.size(), doc, none, airports, error));
  CHECK(error == "ADDS: Cannot perform query. Invalid query parameters.");
  // Too busy, for everyone who asks: its own word, which waits longer (worker.cpp).
  const std::string busy = R"({"error":{"code":429,"message":"Unable to perform query. Too many requests.","details":[]}})";
  CHECK(!airspace::read_airspace(busy.c_str(), busy.size(), doc, none, airports, error) && error == airspace::BUSY);

  // Kept in flash for the next start: the same map back, with what it answered and when; anything else refused.
  {
    const std::vector<uint8_t> bytes = airspace::pack(map, ask.airspace + ask.runways, 1700000000);
    airspace::Map back;
    std::string key;
    uint32_t unix_s = 0;
    CHECK(airspace::unpack(bytes.data(), bytes.size(), back, key, unix_s));
    CHECK(key == ask.airspace + ask.runways && unix_s == 1700000000 && back.cut == map.cut);
    CHECK(back.points.size() == map.points.size() && back.world.size() == map.world.size());
    CHECK(back.areas.size() == map.areas.size() && back.runways.size() == map.runways.size());
    bool same = true;
    for (size_t i = 0; i < map.points.size(); ++i)
      same &= back.points[i].lat == map.points[i].lat && back.points[i].lon == map.points[i].lon &&
              back.world[i].x == map.world[i].x && back.world[i].y == map.world[i].y;
    for (size_t i = 0; i < map.areas.size(); ++i)
      same &= back.areas[i].cls == map.areas[i].cls && back.areas[i].first == map.areas[i].first &&
              back.areas[i].count == map.areas[i].count;
    for (size_t i = 0; i < map.runways.size(); ++i)
      same &= back.runways[i].length_ft == map.runways[i].length_ft &&
              back.runways[i].towered == map.runways[i].towered && back.runways[i].a.lat == map.runways[i].a.lat &&
              back.runways[i].b.lon == map.runways[i].b.lon && back.runways[i].world_b.x == map.runways[i].world_b.x;
    CHECK(same);
    for (size_t cut : {(size_t) 0, (size_t) 3, (size_t) 20, bytes.size() - 1})
      CHECK(!airspace::unpack(bytes.data(), cut, back, key, unix_s) && back.points.empty());
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    CHECK(!airspace::unpack(longer.data(), longer.size(), back, key, unix_s));
    std::vector<uint8_t> other = bytes;
    other[3] = '2';  // another version's
    CHECK(!airspace::unpack(other.data(), other.size(), back, key, unix_s));
    // An area past the end of the points.
    airspace::Map wrong = map;
    wrong.areas[0].count = (uint32_t) map.points.size() + 1;
    const std::vector<uint8_t> broken = airspace::pack(wrong, "k", 1);
    CHECK(!airspace::unpack(broken.data(), broken.size(), back, key, unix_s) && back.areas.empty());
  }
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
  // The map drawn: the Class B's edge in its color, the D's in dashes, the long towered runway and its centerline
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
    const double len = std::hypot(b.x - a.x, b.y - a.y), nm = geo::NM / v.meters_per_px();
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
  deserializeJson(in, R"({"lat":51.47,"lon":-0.45,"range_nm":30,"map":"sectional","watch":[{"id":"g-ezab","color":"blue"},{"id":"GEZAB"}]})");
  std::string error;
  CHECK(apply(in.as<JsonObjectConst>(), s, error));
  CHECK(s.located && s.range_nm == 30 && s.map == SECTIONAL);
  CHECK(s.watch.size() == 1 && s.watch[0].id == "GEZAB" && s.watch[0].color == 4);
  // The British key that older firmware stored is still read.
  JsonDocument old;
  deserializeJson(old, R"({"watch":[{"id":"N12345","colour":"blue"}]})");
  CHECK(apply(old.as<JsonObjectConst>(), s, error) && s.watch.size() == 1 && s.watch[0].color == 4);
  deserializeJson(in, R"({"watch":[{"id":"g-ezab","color":"blue"}]})");
  CHECK(apply(in.as<JsonObjectConst>(), s, error));
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
  // And every other text at its longest, the password among them as the stored copy keeps it.
  s.place = std::string(40, 'p');
  s.follow = std::string(10, 'F');
  s.follow_hex = "abcdef";
  s.password = std::string(MOST_PASSWORD, 'x');
  s.timezone = std::string(60, 'T');
  s.weather_station = "KJFK";
  JsonDocument full;
  to_json(s, full.to<JsonObject>(), true);
  CHECK(measureJson(full) < MOST_STORED - 300);
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
    // millis() wrapping around counts as time going on.
    offline::Tracker t;
    CHECK(!t.update(true, false, UINT32_MAX - 100));
    CHECK(t.update(true, false, AFTER_MS));
  }
  {
    // Off a network for longer than the clock's 49.7 days: still offline as it comes around to where it was lost,
    // not online for another AFTER_MS.
    offline::Tracker t;
    CHECK(!t.update(true, false, 5000));
    CHECK(t.update(true, false, 5000 + AFTER_MS));
    CHECK(t.update(true, false, 0x80000000u) && t.update(true, false, 4999) && t.update(true, false, 5001));
    CHECK(t.update(true, false, 5000 + AFTER_MS / 2) && t.offline());
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

// The map card as the menu and the page tell of it: what it is, its space in four, and what the screen keeps on it.
static void test_card_words() {
  using namespace tiles;
  card::About a;
  a.name = "SD32G";
  a.maker = 0x03;
  a.made = (23 << 4) | 4;
  a.high_capacity = true;
  a.bytes = 31914983424ULL;
  a.khz = 40000;
  a.lines = 4;
  CHECK(card::about_words(a) == "SanDisk SD32G, SDHC, made April 2023");
  CHECK(card::bus_words(a) == "40 MHz on 4 lines");
  a.bytes = 64ULL * 1000 * 1000 * 1000;
  a.maker = 0xEE;
  a.made = 0;
  a.lines = 1;
  CHECK(card::about_words(a) == "SD32G, SDXC");
  CHECK(card::bus_words(a) == "40 MHz on 1 line");
  a.high_capacity = false;
  a.name.clear();
  CHECK(card::about_words(a) == "SDSC");
  CHECK(card::made_words((10 << 4) | 13) == "2010");

  CHECK(card::size_words(40 * 1024) == "40 KB" && card::size_words(52ULL << 20) == "52 MB");
  CHECK(card::size_words(31914983424ULL) == "29.7 GB");
  CHECK(card::count_words(0) == "0" && card::count_words(999) == "999" && card::count_words(12345) == "12,345");
  CHECK(card::count_words(1234567) == "1,234,567");

  // Folders under tiles: two digits of map, its layer.
  uint8_t style = 0;
  CHECK(card::style_folder("06_0", style) && style == SRC_SECTIONAL);
  CHECK(card::style_folder("04_1", style) && style == SRC_SATELLITE);
  CHECK(!card::style_folder("job.txt", style) && !card::style_folder("6_0", style) && !card::style_folder("99_0", style));
  CHECK(card::path(Key{SRC_TOPO, 9, 1, 2}).find("/05_0/") != std::string::npos);
  CHECK(card::on_card(1, 32768) == 32768 && card::on_card(32768, 32768) == 32768 && card::on_card(0, 32768) == 0);
  CHECK(card::on_card(40000, 0) == 65536);  // a cluster not known yet: the usual one

  // Each map's share: the streets' light and dark one map, the largest first.
  card::Tally t;
  CHECK(card::kept_words(t) == "Not counted yet");
  t.counting = true;
  CHECK(card::kept_words(t) == "Counting...");
  t.counted = true;
  CHECK(card::kept_words(t) == "No tiles yet");
  t.tiles[SRC_GRAY_LIGHT] = 10;
  t.bytes[SRC_GRAY_LIGHT] = 10 << 15;
  t.tiles[SRC_GRAY_DARK] = 1;
  t.bytes[SRC_GRAY_DARK] = 1 << 15;
  t.tiles[SRC_SECTIONAL] = 12345;
  t.bytes[SRC_SECTIONAL] = 480ULL << 20;
  t.logs = 1;
  t.log_bytes = 32768;
  const auto maps = card::by_map(t);
  CHECK(maps.size() == 2 && std::string(maps[0].map) == "sectional" && std::string(maps[1].label) == "Streets");
  CHECK(maps[1].tiles == 11 && maps[1].bytes == 11 << 15);
  CHECK(t.tile_count() == 12356 && t.tile_bytes() == (480ULL << 20) + (11 << 15));
  CHECK(card::kept_words(t) == "Sectional: 12,345 tiles, 480 MB\nStreets: 11 tiles, 352 KB\nLog: 1 day, 32 KB");

  // The space in four, which together are the card; a count older than files deleted since is held to what is used.
  const uint64_t GB = 1ULL << 30;
  card::Space sp = card::space_of(30 * GB, 28 * GB, t);
  CHECK(sp.tiles == t.tile_bytes() && sp.log == 32768 && sp.free == 28 * GB);
  CHECK(sp.tiles + sp.log + sp.other + sp.free == 30 * GB);
  sp = card::space_of(1000, 900, t);
  CHECK(sp.tiles == 100 && sp.log == 0 && sp.other == 0 && sp.free == 900);
  CHECK(card::space_words(30 * GB, 28 * GB) == "2.0 GB used of 30.0 GB");

  // A bar's widths: in proportion, a sliver still seen, and together the whole bar.
  std::vector<int> w = card::widths({50, 0, 1, 949}, 200);
  CHECK(w[1] == 0 && w[2] == 2 && w[0] == 10 && w[0] + w[1] + w[2] + w[3] == 200);
  w = card::widths({0, 0, 0, 0}, 200);
  CHECK(w[0] == 0 && w[3] == 0);

  CHECK(card::since_words(12, 340, 0) == "12 read, 340 written");
  CHECK(card::since_words(0, 1500, 2) == "0 read, 1,500 written, 2 failed");
  CHECK(card::download_words(SRC_SECTIONAL, 215, 860) == "Sectional: 215 of 860 tiles (25%)");
  CHECK(card::download_words(SRC_TOPO, 0, 0) == "Topo: 0 of 0 tiles (0%)");
  CHECK(card::note_words(false, "no card in the slot", false, false, 0) == "No map card: no card in the slot.");
  CHECK(card::note_words(true, "", false, false, 0).empty());
  CHECK(card::note_words(true, "", true, false, 0) == "Formatting the card (some seconds)...");
  CHECK(card::note_words(true, "", false, true, 4800) == "Deleting the old tiles: 4,800 files so far.");
  CHECK(card::note_words(true, "the tiles could not be cleared", false, false, 0) == "The tiles could not be cleared.");
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
  // A test's mock in place of every server: the server and path kept as the path; the mock's own address as it is.
  const std::string mock = "http://192.168.1.5:8095";
  CHECK(http::diverted("https://tile.openstreetmap.org/9/150/192.png", mock) ==
        "http://192.168.1.5:8095/tile.openstreetmap.org/9/150/192.png");
  CHECK(http::diverted("https://aviationweather.gov/api/data/metar?format=json&ids=KJFK", mock) ==
        "http://192.168.1.5:8095/aviationweather.gov/api/data/metar?format=json&ids=KJFK");
  CHECK(http::diverted(mock + "/data/aircraft.json", mock) == mock + "/data/aircraft.json");
  CHECK(http::diverted("http://nowhere.invalid/data/aircraft.json", mock) == "http://nowhere.invalid/data/aircraft.json");
  CHECK(http::diverted("https://opendata.adsb.fi/api/v2/x", "") == "https://opendata.adsb.fi/api/v2/x");
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
  // No more planes than there is room for: the first of the list, in room taken once.
  std::vector<feed::Flight> two;
  CHECK(feed::parse(fi.c_str(), fi.size(), doc, two, error, 2) && two.size() == 2 && two.capacity() == 2);
  CHECK(feed::parse(fi.c_str(), fi.size(), doc, flights, error, 0) && flights.empty());
  CHECK(!feed::parse("{\"msg\":\"bad\"}", 13, doc, flights, error) && error == "bad");
  // A reply that calls its planes made up is credited as such; any other is not.
  bool simulated = true;
  CHECK(feed::parse(fi.c_str(), fi.size(), doc, flights, error, SIZE_MAX, &simulated) && !simulated);
  std::string made_up = fi;
  made_up.insert(1, "\"simulated\": true, ");
  CHECK(feed::parse(made_up.c_str(), made_up.size(), doc, flights, error, SIZE_MAX, &simulated) && simulated);
  made_up.replace(made_up.find("true"), 4, "false");
  CHECK(feed::parse(made_up.c_str(), made_up.size(), doc, flights, error, SIZE_MAX, &simulated) && !simulated);
  CHECK(std::string(feed::credit(RECEIVER, true)) == "simulated" && std::string(feed::credit(RECEIVER)) == "receiver");
  CHECK(std::string(feed::credit(ADSB_FI, true)) == "adsb.fi");
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
  CHECK(p.keys.empty() && near(20 * geo::NM / p.view.meters_per_px(), 225, 0.5));
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
  const tiles::Filter colors{tiles::NIGHT_COLORS, 256};
  CHECK(tiles::paint(colors, 255, 255, 255) == tiles::rgb565(tiles::NIGHT_LOW, tiles::NIGHT_LOW, tiles::NIGHT_LOW));
  const tiles::Rgb blue = tiles::night_colors(0, 60, 160);
  CHECK(blue.b > blue.g && blue.g > blue.r && blue.b >= tiles::COLOR_VALUE);
  // Dimmed to 60 %, and the map brightness after any rule.
  CHECK(tiles::paint({tiles::NIGHT_DIM, 256}, 200, 200, 200) == tiles::rgb565(120, 120, 120));
  CHECK(tiles::paint({tiles::NIGHT_NONE, 128}, 200, 100, 50) == tiles::rgb565(100, 50, 25));
  // Which rule each map takes in the dark, and none in the light or on a server of one's own.
  Settings n;
  for (const auto &[map, rule] : {std::pair{SECTIONAL, tiles::NIGHT_DIM}, {IFR_LOW, tiles::NIGHT_COLORS},
                                  {IFR_HIGH, tiles::NIGHT_COLORS}, {TOPO, tiles::NIGHT_INVERT}, {OSM, tiles::NIGHT_INVERT},
                                  {SATELLITE, tiles::NIGHT_NONE}, {STREETS, tiles::NIGHT_NONE}, {CUSTOM, tiles::NIGHT_NONE}}) {
    n.map = map;
    n.theme = DARK;
    CHECK(tiles::filter_of(n).night == rule);
    n.theme = LIGHT;
    CHECK(tiles::filter_of(n).night == tiles::NIGHT_NONE);
  }
  // The filter as a table: every RGB565 color what paint makes of it, for each rule; a plain filter needs none.
  std::vector<uint16_t> table(tiles::PAINT_TABLE);
  for (const tiles::Filter f : {invert, colors, tiles::Filter{tiles::NIGHT_DIM, 200}, tiles::Filter{tiles::NIGHT_NONE, 128}}) {
    tiles::paint_table(f, table.data());
    size_t wrong = 0;
    for (size_t c = 0; c < tiles::PAINT_TABLE; ++c) {
      const uint16_t k = (uint16_t) c;
      if (table[c] != tiles::paint(f, tiles::red_of(k), tiles::green_of(k), tiles::blue_of(k))) ++wrong;
    }
    CHECK(wrong == 0 && !tiles::plain(f));
  }
  CHECK(tiles::plain(tiles::Filter{}) && tiles::red_of(0xF800) == 255 && tiles::green_of(0x07E0) == 255 &&
        tiles::blue_of(0x001F) == 255 && tiles::red_of(0x07FF) == 0);
  // A PNG's colors through the table are within a step of RGB565 of painting them whole.
  tiles::paint_table(invert, table.data());
  int worst = 0;
  for (int r = 0; r < 256; r += 17)
    for (int g = 0; g < 256; g += 15)
      for (int b = 0; b < 256; b += 13) {
        const uint16_t whole = tiles::paint(invert, r, g, b), quick = table[tiles::rgb565(r, g, b)];
        worst = std::max({worst, std::abs(tiles::red_of(whole) - tiles::red_of(quick)),
                          std::abs(tiles::green_of(whole) - tiles::green_of(quick)),
                          std::abs(tiles::blue_of(whole) - tiles::blue_of(quick))});
      }
  CHECK(worst <= 16);
  // Time spent: counted, in all, the longest.
  tiles::Spent spent;
  CHECK(spent.mean_ms() == 0);
  spent.add(2000);
  spent.add(4000);
  spent.add(-5);
  CHECK(spent.count == 3 && spent.total_us == 6000 && spent.most_us == 4000 && near(spent.mean_ms(), 2.0, 1e-9));
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
  CHECK(cache.bytes() == 3 * 33 * 33 + 34 * 34 + 4 * sizeof(sprites::Cache<int>::Entry));  // one is 31 long
  {
    // Past its budget the masks drawn longest ago go as a frame starts, until a quarter of it is free again.
    const size_t one = 33 * 33 + sizeof(sprites::Cache<int>::Entry);
    sprites::Cache<int> kept(3 * one);
    kept.get(30, 0, 1);
    kept.get(30, 90, 1);
    auto *late = kept.get(30, 180, 5);
    kept.get(30, 270, 5);
    CHECK(kept.size() == 4);  // over, but nothing goes until the next frame starts
    CHECK(kept.get(30, 180, 6) == late && kept.size() == 2 && kept.bytes() == 2 * one);  // frame 1's two went
    CHECK(kept.get(30, 270, 6) != nullptr && kept.size() == 2);
    kept.get(30, 0, 6);
    kept.get(30, 90, 6);
    CHECK(kept.size() == 4);
    // The frame before's masks are never let go, even over the budget: it would only make them again.
    kept.get(30, 0, 7);
    CHECK(kept.size() == 4);
  }
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

// The emblem (emblem.h): a frame drawn after another is the same picture as that frame drawn alone, and every pixel
// that differs from the frame before lies in a box it says changed, so the glass can send only those.
static void test_emblem() {
  using namespace emblem;
  const int n = 240;
  Emblem stepped, alone;
  stepped.prepare(n, n / 2.0f - 2);
  alone.prepare(n, n / 2.0f - 2);
  CHECK(stepped.ready() && stepped.size() == n);
  std::vector<uint16_t> now((size_t) n * n), before((size_t) n * n), fresh((size_t) n * n);
  std::vector<Box> changed;
  size_t sent = 0, frames = 0;
  for (float t = 0; t < SPIN_S + 0.3f; t += 0.09f) {
    stepped.draw(now.data(), n, bearing_at(t), turned_at(t), 1, &changed);
    // Into other pixels each time, so this one is drawn whole.
    std::vector<uint16_t> other((size_t) n * n);
    alone.draw(other.data(), n, bearing_at(t), turned_at(t));
    CHECK(now == other);
    size_t outside = 0;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        if (now[(size_t) y * n + x] == before[(size_t) y * n + x]) continue;
        bool in = false;
        for (const Box &b : changed) in = in || (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
        outside += !in;
      }
    CHECK(outside == 0);
    for (const Box &b : changed) {
      CHECK(b.x >= 0 && b.y >= 0 && b.w >= 0 && b.h >= 0 && b.x + b.w <= n && b.y + b.h <= n);
      if (t > 0) sent += (size_t) b.w * b.h;
    }
    if (t > 0) ++frames;
    // Few enough boxes for LVGL to keep apart (past 32 it draws the whole glass again).
    CHECK(changed.size() <= 20);
    before = now;
  }
  // A frame sends well under the whole picture, on average.
  CHECK(sent / frames < (size_t) n * n * 7 / 10);
  // It comes to rest where it is printed, having passed every blip, and stays there.
  CHECK(near(bearing_at(SPIN_S), REST_DEGREES, 0.01) && near(bearing_at(SPIN_S + 5), REST_DEGREES, 0.01));
  CHECK(near(turned_at(0), 0, 0.001) && near(turned_at(SPIN_S), TURN_DEGREES, 0.001));
  for (const Blip &b : BLIPS) {
    const float after_start = std::fmod(b.bearing - bearing_at(0) + 720.0f, 360.0f);
    CHECK(after_start <= TURN_DEGREES);
  }
  // The rim is lit, the ground outside the scope is not, and the hub is the propeller's, not the scope's.
  alone.draw(fresh.data(), n, REST_DEGREES, TURN_DEGREES);
  CHECK(fresh[0] == rgb565(Colors{}.ground));
  CHECK(fresh[(size_t) (n / 2) * n + (int) (n / 2 + RIM * (n / 2.0f - 2))] == rgb565(Colors{}.scope));
  const uint16_t hub = fresh[(size_t) (n / 2) * n + n / 2 + (int) (HUB * (n / 2.0f - 2) * 0.3f)];
  CHECK((hub >> 11) > 12 && (hub >> 11) + 4 > (hub & 0x1F));
  // The bearing a compass gives, and the arc tangent it comes from.
  CHECK(near(bearing_of(0, -10), 0, 0.02) && near(bearing_of(10, 0), 90, 0.02) && near(bearing_of(0, 10), 180, 0.02) &&
        near(bearing_of(-10, 0), 270, 0.02) && near(bearing_of(10, -10), 45, 0.02));
  for (float t = 0; t < 40; t += 0.37f) CHECK(near(atan_degrees(t), std::atan(t) * 180 / 3.14159265, 0.02));
  // The blade: slim at the root, broadest past its middle, nothing at its tip; its middle leans the way it turns
  // (+y), which is what tells the eye which way that is; lit most along the leading edge.
  CHECK(blade_half_wide(0.64f) > 2 * blade_half_wide(0) && near(blade_half_wide(1), 0, 1e-4));
  CHECK(blade_middle(0.5f) > 0 && near(blade_middle(1), 0, 1e-6));
  const float mid = BLADE_ROOT + BLADE_LONG * 0.6f;
  CHECK(blade_holds(mid, blade_middle(0.6f)) && !blade_holds(BLADE_TIP + 0.01f, 0) && !blade_holds(mid, 0.2f));
  CHECK(blade_lit(mid, blade_lead(0.6f)) > blade_lit(mid, blade_trail(0.6f)) + 0.5f);
  // A plane the beam finds: its nose and its wingtips are in it, the air beside its nose is not.
  CHECK(plane_distance(0, -0.9f) < 0 && plane_distance(0.9f, 0.4f) < 0 && plane_distance(-0.9f, 0.4f) < 0);
  CHECK(plane_distance(0.6f, -0.8f) > 0.3f);
  // What it takes is said before it is taken, and given back.
  CHECK(Emblem::bytes(720, 358) < 2 * 1024 * 1024);
  stepped.release();
  CHECK(!stepped.ready());
}

// Times on the 32-bit clock (ticks.h): one left alone for weeks reads as yet to come, unless it was kept behind the
// clock on every pass.
static void test_ticks() {
  using namespace ticks;
  CHECK(past(1000, 1000) && past(1000, 5000) && !past(5000, 1000));
  CHECK(past(0xFFFFFF00u, 0x00000100u));  // over the clock's turn
  // 25 days on, a time set at the start reads as yet to come.
  const uint32_t day = 24u * 3600 * 1000;
  CHECK(!past(0, 25 * day));
  // Kept behind the clock a pass at a time (here an hour), it is past all along, and over the clock's turn.
  uint32_t due = 0;
  for (uint32_t now = 0, pass = 0; pass < 60 * 24; ++pass, now += 3600 * 1000) {
    keep_past(due, now);
    CHECK(past(due, now));
  }
  // A time yet to come is left alone, and so is one that has only just passed.
  due = 9000;
  keep_past(due, 1000);
  CHECK(due == 9000);
  keep_past(due, 9000 + BEHIND_MS);
  CHECK(due == 9000);
  keep_past(due, 9001 + BEHIND_MS);
  CHECK(due == 9001);
  // One where 0 says "none": none again once it has come, and not before.
  uint32_t until = 5000;
  expire(until, 4999);
  CHECK(until == 5000);
  expire(until, 5000);
  CHECK(until == 0);
  expire(until, 123);
  CHECK(until == 0);
}

static void test_beam() {
  // A 101 x 101 scope around its middle, a gray ground; the beam at 90 degrees (east), having come from 85.
  const int n = 101;
  std::vector<uint16_t> canvas(n * n, 0x1234), ground(n * n, 0x1234);
  beam::Painter p;
  p.canvas = canvas.data();
  p.background = ground.data();
  p.width = p.height = n;
  p.cx = p.cy = n / 2.0f;
  p.reach = 48;
  p.beam_width = 3;
  p.set_tint(colors::tint(0));
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

// Following a plane (follow.h): what is asked for it, how it goes when out of reach, and the view moving with it.
static void test_follow() {
  using namespace follow;
  // Flight numbers turned into callsigns; anything else is none.
  CHECK(callsign_of("BA286") == "BAW286" && callsign_of("BA0286") == "BAW286" && callsign_of("U21234") == "EZY1234");
  CHECK(callsign_of("AS1A") == "ASA1A" && callsign_of("6E2131") == "IGO2131" && callsign_of("BA0") == "BAW0");
  CHECK(callsign_of("N12345").empty() && callsign_of("BAW286").empty() && callsign_of("ZZ123").empty() &&
        callsign_of("BA12345").empty() && callsign_of("12345").empty());
  // What is asked: a known hex alone; else the callsigns, the registration (with its dash added) and the hex.
  auto asked = asks("BA286", "");
  CHECK(asked.size() >= 3 && asked[0] == (Ask{BY_CALLSIGN, "BAW286"}) && asked[1] == (Ask{BY_CALLSIGN, "BA286"}));
  asked = asks("N7209W", "");
  CHECK(std::find(asked.begin(), asked.end(), Ask{BY_REGISTRATION, "N7209W"}) != asked.end() &&
        std::none_of(asked.begin(), asked.end(), [](const Ask &a) { return a.id.find('-') != std::string::npos; }));
  asked = asks("GPOLH", "");
  CHECK(std::find(asked.begin(), asked.end(), Ask{BY_REGISTRATION, "G-POLH"}) != asked.end() &&
        std::find(asked.begin(), asked.end(), Ask{BY_REGISTRATION, "GP-OLH"}) != asked.end());
  asked = asks("G-POLH", "");
  CHECK(std::find(asked.begin(), asked.end(), Ask{BY_REGISTRATION, "G-POLH"}) != asked.end() &&
        std::find(asked.begin(), asked.end(), Ask{BY_CALLSIGN, "GPOLH"}) != asked.end());
  asked = asks("A9A734", "");
  CHECK(asked.back() == (Ask{BY_HEX, "a9a734"}));
  asked = asks("BAW286", "4ca123");
  CHECK(asked.size() == 1 && asked[0] == (Ask{BY_HEX, "4ca123"}));
  CHECK(url(ADSB_LOL, {BY_REGISTRATION, "G-POLH"}) == "https://api.adsb.lol/v2/reg/G-POLH");
  CHECK(url(ADSB_FI, {BY_CALLSIGN, "BAW286"}) == "https://opendata.adsb.fi/api/v2/callsign/BAW286");
  CHECK(url(ADSB_FI, {BY_HEX, "a9a734"}) == "https://opendata.adsb.fi/api/v2/hex/a9a734");
  CHECK(url(ADSB_LOL, {BY_CALLSIGN, "A/../B"}) == "https://api.adsb.lol/v2/callsign/AB");
  CHECK(lookup_feeds(RECEIVER)[0] == ADSB_LOL && lookup_feeds(ADSB_FI)[0] == ADSB_FI);

  // The settings keep what was typed in one form, and a new plane forgets the old one's hex.
  Settings s;
  JsonDocument change;
  std::string error;
  change["follow"] = " ba 286 ";
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.follow == "BA286" && s.follow_hex.empty());
  change.clear();
  change["follow_hex"] = "4CA123";
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.follow_hex == "4ca123");
  change.clear();
  change["follow"] = "BA286";
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.follow_hex == "4ca123");
  change["follow"] = "g-polh";
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.follow == "G-POLH" && s.follow_hex.empty());
  change.clear();
  change["follow_hex"] = "xyz";
  CHECK(!apply(change.as<JsonObjectConst>(), s, error) && error == "follow_hex");
  change.clear();
  change["follow"] = "ABCDEFGHIJK";
  CHECK(!apply(change.as<JsonObjectConst>(), s, error) && error == "follow");
  change["follow"] = "";
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.follow.empty() && s.follow_hex.empty());
  change.clear();
  change["range_nm"] = 5000;
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && s.range_nm == 5000);
  change["range_nm"] = 5001;
  CHECK(!apply(change.as<JsonObjectConst>(), s, error));

  // A flight seen, out of reach, carried on along its great circle, found again; then landing stops it.
  feed::Flight f;
  f.hex = "a9a734";
  f.callsign = "AMF1985";
  f.lat = 45.59;
  f.lon = -122.60;
  f.has_altitude = true;
  f.altitude = 35000;
  f.has_heading = true;
  f.heading = 45;
  f.has_speed = true;
  f.speed = 480;
  Follower fl;
  CHECK(fl.phase(1000) == LOOKING && status(fl, "AMF1985", 1000) == "Looking for AMF1985");
  fl.seen(f, 1000);
  CHECK(fl.phase(30000) == LIVE && status(fl, "N7209W", 30000) == "Following AMF1985");
  const uint32_t hour = 3600u * 1000;
  CHECK(fl.phase(1000 + hour) == LOST && fl.estimating(1000 + hour));
  const geo::Place p = fl.where(1000 + hour);
  CHECK(near(geo::meters_between(f.lat, f.lon, p.lat, p.lon), 480 * geo::NM, 100) && p.lat > f.lat && p.lon > f.lon);
  CHECK(status(fl, "N7209W", 1000 + 42 * 60000 + LOST_MS) == "AMF1985 out of reach 43 min, estimated");
  CHECK(fl.shown(1000 + hour).lat == p.lat);
  // An estimate goes no further than MOST_ESTIMATE_MS, and half a day lost gives up.
  const geo::Place far = fl.where(1000 + 11 * hour), most = fl.where(1000 + MOST_ESTIMATE_MS);
  CHECK(far.lat == most.lat && far.lon == most.lon);
  CHECK(fl.phase(1000 + GIVE_UP_MS) == GAVE_UP);
  // On the ground after flying: landed once it has sat there a while.
  f.on_ground = true;
  fl.seen(f, 2 * hour);
  CHECK(fl.phase(2 * hour + 1000) == LIVE && !fl.estimating(2 * hour + LOST_MS + 1000));
  CHECK(fl.phase(2 * hour + LANDED_MS) == LANDED);
  // Found at the gate before it flies: never landed, however long it waits there.
  Follower gate;
  gate.seen(f, 1000);
  CHECK(gate.phase(1000 + LANDED_MS + 5000) == LOST && !gate.estimating(1000 + hour));
  // Lost low and coming down after flying: landing, held where it was, landed after a while.
  Follower low;
  f.on_ground = false;
  low.seen(f, 1000);
  f.altitude = 1200;
  f.has_climb = true;
  f.climb = -700;
  low.seen(f, 2000);
  CHECK(low.phase(2000 + LOST_MS + 1) == LOST && !low.estimating(2000 + LOST_MS + 1) &&
        low.where(2000 + hour).lat == f.lat);
  CHECK(low.phase(2000 + LANDED_MS) == LANDED);
  // The answer that found it: the one in the air before one on the ground.
  std::vector<feed::Flight> found(2, f);
  found[0].on_ground = true;
  found[0].hex = "aaaaaa";
  CHECK(pick(found) == &found[1] && pick({}) == nullptr);

  // The view: kept while the plane is well inside, moved once it is out past KEEP, with the plane behind the middle.
  const geo::View v(45.0, -122.0, 7, 480, 640);
  CHECK(!off_center(v, 45.0, -122.0));
  const geo::Place edge = v.place(480 * 0.95, 320);
  CHECK(off_center(v, edge.lat, edge.lon));
  const geo::Place c = center_for(v, edge.lat, edge.lon, true, 90);
  const geo::View next(c.lat, c.lon, v.zoom, v.width, v.height);
  const geo::Pixel at = next.point(edge.lat, edge.lon);
  CHECK(near(at.x, 240 - LEAD * 240, 3) && near(at.y, 320, 3) && !off_center(next, edge.lat, edge.lon));
  geo::View round = v;
  round.round = true;
  const geo::Place corner = v.place(480 * 0.8, 640 * 0.8);
  CHECK(off_center(round, corner.lat, corner.lon));

  // Its own range: from where a step puts it, the plane flies about a minute (a whole zoom either way of it: 40 to 85 s)
  // before the next step, slow or fast, any way it heads, on a long glass or a round one.
  struct Glass { int w, h; bool round; };
  for (const Glass g : {Glass{480, 640, false}, Glass{640, 480, false}, Glass{466, 466, true}}) {
    for (const float kt : {60.0f, 140.0f, 250.0f, 480.0f, 560.0f}) {
      for (const float heading : {0.0f, 45.0f, 90.0f, 200.0f, 300.0f}) {
        feed::Flight f;
        f.has_speed = f.has_heading = true;
        f.speed = kt;
        f.heading = heading;
        const int nm = plane_range_nm(f, 51.5, g.w, g.h, g.round);
        geo::View view = view_for(51.5, -0.1, nm, g.w, g.h);
        view.round = g.round;
        const geo::Place mid = center_for(view, 51.5, -0.1, true, heading);
        geo::View stepped(mid.lat, mid.lon, view.zoom, g.w, g.h);
        stepped.round = g.round;
        int s = 0;
        geo::Place p{51.5, -0.1};
        while (s < 600 && !off_center(stepped, p.lat, p.lon)) {
          p = geo::destination(51.5, -0.1, heading, kt * geo::NM * ++s / 3600.0);
        }
        CHECK(s >= 40 && s <= 85);
        if (s < 40 || s > 85) std::printf("  %d x %d, %.0f kt heading %.0f: %d NM, %d s\n", g.w, g.h, kt, heading, nm, s);
      }
    }
  }
  // The next step, drawn ahead: a plane flying straight, reported every few seconds, steps each time into the view
  // planned for it a step before, and is where a step would put it there.
  for (const Glass g : {Glass{480, 640, false}, Glass{466, 466, true}}) {
    for (const float heading : {10.0f, 75.0f, 135.0f, 260.0f}) {
      const float kt = 450, report_s = 5;
      geo::View shown(52.0, 4.0, 11, g.w, g.h);
      shown.round = g.round;
      geo::Place plane{52.0, 4.0};
      int steps = 0, taken = 0;
      for (int t = 0; t < 600 && steps < 6; t += (int) report_s) {
        geo::Place ahead;
        CHECK(next_center(shown, plane.lat, plane.lon, true, heading, ahead));
        geo::View planned(ahead.lat, ahead.lon, shown.zoom, g.w, g.h);
        planned.round = g.round;
        plane = geo::destination(plane.lat, plane.lon, heading, kt * geo::NM * report_s / 3600.0);
        if (!off_center(shown, plane.lat, plane.lon)) continue;
        ++steps;
        if (suits(planned, plane.lat, plane.lon, heading)) {
          ++taken;
          shown = planned;
        } else {
          const geo::Place c = center_for(shown, plane.lat, plane.lon, true, heading);
          shown = geo::View(c.lat, c.lon, shown.zoom, g.w, g.h);
          shown.round = g.round;
        }
        CHECK(!off_center(shown, plane.lat, plane.lon));
      }
      CHECK(steps >= 3 && taken == steps);
    }
  }
  {
    // A plane turned around is nowhere near the spot of the view planned for it, and none is planned without a heading.
    const geo::View shown(52.0, 4.0, 11, 480, 640);
    geo::Place ahead, at;
    CHECK(next_center(shown, 52.0, 4.0, true, 90, ahead) && !next_center(shown, 52.0, 4.0, false, 90, ahead));
    const geo::View planned(ahead.lat, ahead.lon, 11, 480, 640);
    CHECK(crossing(shown, 52.0, 4.0, 90, at) && suits(planned, at.lat, at.lon, 90) && !suits(planned, at.lat, at.lon, 270));
    CHECK(!suits(planned, 52.0, 4.0, 90));
  }
  feed::Flight unheard;
  CHECK(plane_range_nm(unheard, 51.5, 480, 640, false) == PLANE_RANGE_NM);
  feed::Flight parked;
  parked.has_speed = true;
  CHECK(plane_range_nm(parked, 51.5, 480, 640, false) == MIN_RANGE_NM);
  feed::Flight jet;
  jet.has_speed = true;
  jet.speed = 480;
  // An airliner's minute is 8 NM: on a 480 pixel side, a range of some 5 to 11 NM (a whole zoom).
  const int jet_nm = plane_range_nm(jet, 51.5, 480, 640, false);
  CHECK(jet_nm >= 5 && jet_nm <= 11);

  // Shown while following: the middle, a world map outside the United States, no rings, the plane watched.
  Settings home;
  home.map = SECTIONAL;
  home.lat = 40.6;
  home.lon = -73.8;
  Center over{true, 51.5, -0.4, "a9a734"};
  Settings shown_s = shown(home, over);
  CHECK(shown_s.lat == 51.5 && shown_s.map == STREETS && !shown_s.rings && shown_s.located);
  CHECK(shown_s.watch.size() == 1 && shown_s.watch[0].id == "A9A734");
  over.lat = 45.6;
  over.lon = -122.6;
  CHECK(shown(home, over).map == SECTIONAL);
  CHECK(shown(home, Center{}).lat == 40.6 && shown(home, Center{}).rings);
  CHECK(in_united_states(61.2, -149.9) && in_united_states(21.3, -157.9) && !in_united_states(51.5, -0.4));
  // The planes around it: out to the view's far corner from it, at most the exchanges' reach.
  CHECK(reach_nm(v, 45.0, -122.0) < FEED_REACH_NM);
  const geo::View wide(45.0, -122.0, 2, 480, 640);
  CHECK(reach_nm(wide, 45.0, -122.0) == FEED_REACH_NM);
  // A great circle: west from London, and over the date line.
  const geo::Place west = geo::destination(51.5, -0.4, 270, 1000 * 1000);
  CHECK(west.lon < -10 && west.lat < 51.5 && west.lat > 50);  // due west at first, a great circle bends south
  CHECK(geo::destination(0, 179.5, 90, 200 * 1000).lon < -178);
  // The followed plane's own label: a data block among planes labeled by callsign, and back to theirs with `auto`.
  Settings labels;
  labels.label = LABEL_CALLSIGN;
  labels.lat = f.lat;
  labels.lon = f.lon;
  CHECK(labels.follow_label == LABEL_SCOPE);
  feed::Flight other = f;
  other.hex = "bbbbbb";
  other.callsign = "OTHER1";
  f.on_ground = false;
  f.altitude = 35000;
  f.has_climb = false;
  const std::vector<feed::Flight> two{f, other};
  const geo::View here(f.lat, f.lon, 9, 480, 640);
  overlay::Tracks none;
  plan::Overlay o = overlay::build(two, shown(labels, Center{true, f.lat, f.lon, f.hex}), here, none, 0, 8, false);
  auto label_for = [&](const char *id) {
    for (const auto &p : o.planes) if (p.id == id) return p.label;
    return std::string("?");
  };
  CHECK(label_for("a9a734") == "AMF1985\n350 48" && label_for("bbbbbb") == "OTHER1");
  labels.follow_label = LABEL_AUTO;
  o = overlay::build(two, shown(labels, Center{true, f.lat, f.lon, f.hex}), here, none, 0, 8, false);
  CHECK(label_for("a9a734") == "AMF1985");
  JsonDocument pick_label;
  pick_label["follow_label"] = "full";
  CHECK(apply(pick_label.as<JsonObjectConst>(), labels, error) && labels.follow_label == LABEL_FULL);
  pick_label["follow_label"] = "big";
  CHECK(!apply(pick_label.as<JsonObjectConst>(), labels, error) && error == "follow_label");
  // The trail: a place each MIN_STEP_M, thinned at its old end past MOST_POINTS, as many minutes of it as asked.
  Trail trail;
  trail.clear("a9a734");
  for (int i = 0; i < 3000; ++i) trail.add(40.0 + i * 0.01, -100.0, (uint32_t) i * 1000);
  trail.add(40.0 + 2999 * 0.01 + 0.001, -100.0, 3000000);  // 111 m on: not kept
  CHECK(trail.hex() == "a9a734" && trail.size() <= Trail::MOST_POINTS && trail.size() > Trail::MOST_POINTS / 2);
  const auto all = trail.since(3000000, 0);
  CHECK(near(all.front().lat, 40.0, 1e-4) && near(all.back().lat, 40.0 + 2999 * 0.01, 1e-3));
  const auto lately = trail.since(2999 * 1000, 1);  // the last minute: one place a second
  CHECK(lately.size() == 61 && near(lately.back().lat, all.back().lat, 1e-6));
  // On the frame: thinned to the most asked, the plane's end kept, the far old end cut off.
  const geo::View near_end(all.back().lat, -100.0, 12, 480, 640);
  const auto px = track_on(all, near_end, 50);
  CHECK(!px.empty() && px.size() <= 100 && near(px[px.size() - 1], 320, 1) && near(px[px.size() - 2], 240, 1));
  CHECK(std::all_of(px.begin(), px.end(), [](int16_t v) { return std::abs(v) <= FAR_PX; }));
  const geo::View world(all.back().lat, -100.0, 3, 480, 640);
  const auto whole = track_on(all, world, 50);
  CHECK(whole.size() == 100 && near(world.place(whole[0], whole[1]).lat, 40.0, 0.1));
  CHECK(track_on({}, world, 50).empty());
  JsonDocument minutes;
  minutes["follow_track_min"] = 480;
  CHECK(apply(minutes.as<JsonObjectConst>(), labels, error) && labels.follow_track_min == 480);
  minutes["follow_track_min"] = 2000;
  CHECK(!apply(minutes.as<JsonObjectConst>(), labels, error) && error == "follow_track_min");
  CHECK(Settings{}.follow_track_min == 0);
  // The range following changed from: none until it has, at most the farthest range.
  CHECK(Settings{}.own_range_nm == 0);
  JsonDocument own;
  own["own_range_nm"] = 40;
  CHECK(apply(own.as<JsonObjectConst>(), labels, error) && labels.own_range_nm == 40);
  own["own_range_nm"] = MAX_RANGE_NM + 1;
  CHECK(!apply(own.as<JsonObjectConst>(), labels, error) && error == "own_range_nm");
  // The route: adsb.lol's routeset asked and read, the leg the plane is on, its great circle, the line on the frame,
  // and a view that holds it all.
  JsonDocument doc;
  Route route;
  RouteAsk over_canada{"baw 266", 58.0, -60.0, true, 60};
  CHECK(routeset_body(over_canada) == R"({"planes":[{"callsign":"BAW266","lat":58,"lng":-60}]})");
  const std::string answer = R"([{"callsign":"BAW266","airport_codes":"KPDX-EGLL","plausible":true,
      "_airports":[{"icao":"KPDX","iata":"PDX","lat":45.5887,"lon":-122.5975},{"icao":"EGLL","iata":"LHR","lat":51.4706,
      "lon":-0.461941}]}])";
  CHECK(read_routeset(answer.c_str(), answer.size(), doc, over_canada, route, error) && route.known &&
        route.from == "KPDX" && route.to == "EGLL" && near(route.to_lon, -0.461941, 1e-6) &&
        route.callsign == "baw 266");
  // The same callsign over Spain is on no leg of it: the flight number has moved on.
  Route moved;
  RouteAsk over_spain{"BAW266", 40.4, -3.7};
  CHECK(read_routeset(answer.c_str(), answer.size(), doc, over_spain, moved, error) && !moved.known);
  // Over Canada but flying back west: no leg of it either.
  RouteAsk back{"BAW266", 58.0, -60.0, true, 250};
  CHECK(read_routeset(answer.c_str(), answer.size(), doc, back, moved, error) && !moved.known);
  // adsb.lol's own check said no, an unknown callsign, and a reply that is no list.
  const std::string implausible = R"([{"callsign":"BAW266","plausible":false,"_airports":[{"icao":"KPDX","lat":45.6,
      "lon":-122.6},{"icao":"EGLL","lat":51.5,"lon":-0.46}]}])";
  CHECK(read_routeset(implausible.c_str(), implausible.size(), doc, over_canada, moved, error) && !moved.known);
  Route nothing;
  const std::string unknown = R"([{"callsign":"BAW266","airport_codes":"unknown","_airports":[]}])";
  CHECK(read_routeset(unknown.c_str(), unknown.size(), doc, over_canada, nothing, error) && !nothing.known);
  CHECK(!read_routeset("{", 1, doc, over_canada, nothing, error));
  CHECK(!read_routeset("{}", 2, doc, over_canada, nothing, error));
  // A route of two legs: the second for a plane between its middle airport and its last.
  const std::string two_legs = R"([{"callsign":"UAE2","plausible":true,"_airports":[
      {"icao":"EGLL","lat":51.47,"lon":-0.46},{"icao":"OMDB","lat":25.25,"lon":55.36},{"icao":"YSSY","lat":-33.95,"lon":151.18}]}])";
  Route second;
  RouteAsk indian_ocean{"UAE2", -5.0, 95.0};
  CHECK(read_routeset(two_legs.c_str(), two_legs.size(), doc, indian_ocean, second, error) && second.known &&
        second.from == "OMDB" && second.to == "YSSY");
  CHECK(route_leg({{51.47, -0.46}, {25.25, 55.36}}, 40, 25) == 0);
  // hexdb.io's answer: its airports in the order flown, the plane on the leg; none known is no error.
  CHECK(hexdb_url("baw 266") == "https://hexdb.io/api/v2/routes/BAW266");
  const std::string hexdb = R"({"callsign":"BAW266","origin":{"icao":"KPDX"},"destination":{"icao":"EGLL"},
      "airports":[{"icao":"KPDX","iata":"PDX","latitude":45.5887,"longitude":-122.5975},
      {"icao":"EGLL","iata":"LHR","latitude":51.4706,"longitude":-0.461941}],"updated_at":"2014-04-20T11:02:19Z"})";
  Route from_hexdb;
  CHECK(read_hexdb(hexdb.c_str(), hexdb.size(), doc, over_canada, from_hexdb, error) && from_hexdb.known &&
        from_hexdb.from == "KPDX" && from_hexdb.to == "EGLL");
  CHECK(read_hexdb(hexdb.c_str(), hexdb.size(), doc, over_spain, from_hexdb, error) && !from_hexdb.known);
  const std::string no_route = R"({"error":{"status":404,"code":"route_not_found","message":"No route."}})";
  CHECK(read_hexdb(no_route.c_str(), no_route.size(), doc, over_canada, from_hexdb, error) && !from_hexdb.known);
  const std::string busy = R"({"error":{"status":500,"code":"oops","message":"Busy"}})";
  CHECK(!read_hexdb(busy.c_str(), busy.size(), doc, over_canada, from_hexdb, error) && error == "hexdb.io: Busy");
  CHECK(!read_hexdb("{", 1, doc, over_canada, from_hexdb, error));
  CHECK(route_leg({{51.47, -0.46}}, 40, 25) == -1);
  const auto circle = route_places(route, 32);
  CHECK(circle.size() == 32 && near(circle.front().lat, 45.5887, 1e-6) && near(circle.back().lon, -0.461941, 1e-6));
  // Portland to London goes up past Hudson Bay and southern Greenland: far north of both ends.
  CHECK(std::any_of(circle.begin(), circle.end(), [](const geo::Place &p) { return p.lat > 60; }));
  const Fit all_of_it = fit(circle, 480, 640, false);
  CHECK(all_of_it.range_nm > 1000 && all_of_it.range_nm < MAX_RANGE_NM);
  const geo::View fitted = view_for(all_of_it.center.lat, all_of_it.center.lon, all_of_it.range_nm, 480, 640);
  CHECK(std::all_of(circle.begin(), circle.end(), [&](const geo::Place &p) {
    const geo::Pixel q = fitted.point(p.lat, p.lon);
    return q.x >= 0 && q.x < 480 && q.y >= 0 && q.y < 640;
  }));
  const auto line = route_on(route, fitted);
  CHECK(line.size() == 128 && std::none_of(line.begin(), line.end(), [](int16_t v) { return v == ROUTE_BREAK; }));
  // Close in at Portland: the far end leaves the frame by far and is cut off, with no break left dangling.
  const auto close = route_on(route, view_for(45.5887, -122.5975, 20, 480, 640));
  CHECK(close.size() >= 4 && close.size() < 128 && close.back() != ROUTE_BREAK && near(close[0], 240, 2));
  // Over the date line: Tokyo to San Francisco is one piece on a view that holds it, the fit the short way around.
  Route pacific{true, "JAL2", "RJTT", "KSFO", 35.55, 139.78, 37.62, -122.38};
  const Fit ocean = fit(route_places(pacific, 24), 480, 640, false);
  CHECK(ocean.center.lon > 150 || ocean.center.lon < -150);
  CHECK(fit({{45, -122}, {45.1, -122.1}}, 480, 640, false).range_nm < 20);
  CHECK(fit({{45, -122}}, 480, 640, true).range_nm == MIN_RANGE_NM);
  const geo::Place mid = geo::between(0, 0, 0, 90, 0.5);
  CHECK(near(mid.lat, 0, 1e-9) && near(mid.lon, 45, 1e-9));
  CHECK(age_text(40000) == "40 s" && age_text(12 * 60000) == "12 min" && age_text(3 * hour + 5 * 60000) == "3 h 05 min");
}

// A plane's card says how fast it climbs or descends, roughly.
// A tap takes the nearest plane in reach, but the plane followed before any other within its own reach.
static void test_pick() {
  plan::Overlay o;
  o.glide = false;
  auto add = [&](const char *id, int x, int y) {
    plan::Plane p;
    p.id = id;
    p.x = x;
    p.y = y;
    o.planes.push_back(p);
  };
  add("near", 102, 100);
  add("followed", 112, 100);
  add("far", 200, 100);
  CHECK(plan::pick(o, 100, 100, 20, 0) == "near");
  o.followed = "followed";
  CHECK(plan::pick(o, 100, 100, 20, 0) == "followed");
  // Its reach is a quarter more than the others', no more.
  CHECK(plan::pick(o, 88, 100, 20, 0) == "followed" && plan::pick(o, 85, 100, 20, 0) == "near");
  CHECK(plan::pick(o, 200, 104, 20, 0) == "far" && plan::pick(o, 150, 150, 20, 0).empty());
}

static void test_climb_text() {
  feed::Flight f;
  CHECK(overlay::climb_way(f) == 0 && overlay::climb_rate(f, false).empty());
  f.has_climb = true;
  f.climb = 150;
  CHECK(overlay::climb_way(f) == 0 && overlay::climb_rate(f, false).empty());
  f.climb = 1830;
  CHECK(overlay::climb_way(f) == 1 && overlay::climb_rate(f, false) == "1,800 ft/min");
  f.climb = -640;
  CHECK(overlay::climb_way(f) == -1 && overlay::climb_rate(f, false) == "600 ft/min" &&
        overlay::climb_rate(f, true) == "3 m/s");
  f.climb = -12345;
  CHECK(overlay::climb_rate(f, false) == "12,300 ft/min");
  f.climb = 250;
  CHECK(overlay::climb_rate(f, true) == "1 m/s");
  f.on_ground = true;
  CHECK(overlay::climb_way(f) == 0);
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
  st.range_nm = 5000;
  CHECK(!menu::stepped(row("range_nm"), st, 1) && st.range_nm == 5000);
  // Words go around; the map skips the custom server when none is set.
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
    // Every row has its place under one heading: the rows a setting needs come after it in its own group.
    auto group_of = [](const char *key) {
      const char *name = "";
      for (const auto &r : menu::rows()) {
        if (const menu::Group *g = menu::group_starting(r)) name = g->name;
        if (std::string(r.key) == key) return std::string(name);
      }
      return std::string();
    };
    CHECK(group_of("range_nm") == "Place" && group_of("sweep") == "Map" && group_of("radar_loop") == "Over the map");
    CHECK(group_of("photos") == "Aircraft" && group_of("watch") == "Which aircraft");
    CHECK(group_of("follow_track_min") == "Follow a plane" && group_of("refresh_s") == "Source");
    CHECK(group_of("rotation") == "Screen" && group_of("password") == "Network");
    CHECK(group_of("overhead_ft") == "Alerts" && group_of("distress_always") == "Sound");
    for (const auto &r : menu::rows())
      for (const char *need : r.needs) CHECK(group_of(need) == group_of(r.key));
  }
  // A flag that is on or off is a switch; Units is neither, and keeps its arrows. A switch still steps.
  CHECK(menu::switched(row("rings")) && menu::switched(row("bedtime")) && !menu::switched(row("metric")));
  CHECK(!menu::switched(row("map")) && !menu::switched(row("range_nm")));
  for (const auto &r : menu::rows())
    if (menu::switched(r)) CHECK(menu::steps(r) && !menu::typed(r));
  // Rows follow the map: the phosphor on the radar only, place names on streets and imagery.
  st.map = RADAR;
  CHECK(menu::applies(row("tint"), st) && !menu::applies(row("place_names"), st) && !menu::applies(row("theme"), st));
  st.map = SATELLITE;
  CHECK(!menu::applies(row("tint"), st) && menu::applies(row("place_names"), st) && menu::applies(row("tile_scale"), st));
  st.map = SECTIONAL;
  CHECK(!menu::applies(row("tile_scale"), st) && menu::applies(row("map_brightness"), st));
  // The weather radar goes over a map of tiles, not the scope; its loop shows only with it on.
  st.map = RADAR;
  CHECK(!menu::applies(row("radar_overlay"), st));
  st.map = STREETS;
  st.radar_overlay = false;
  CHECK(menu::applies(row("radar_overlay"), st) && !menu::applies(row("radar_loop"), st));
  st.radar_overlay = true;
  CHECK(menu::applies(row("radar_loop"), st));
  // Bedtime's times show with bedtime on; the time it comes on again only with that on too. The time zone always
  // shows: the day's tally and the weather go by it too.
  st.bedtime = false;
  CHECK(!menu::applies(row("bed_off"), st) && menu::applies(row("timezone"), st));
  st.bedtime = true;
  st.bed_wake = false;
  CHECK(menu::applies(row("bed_off"), st) && !menu::applies(row("bed_on"), st));
  st.bed_wake = true;
  CHECK(menu::applies(row("bed_on"), st));
  // The followed plane's label and track show only while a plane is named to follow.
  st.follow = "";
  CHECK(!menu::applies(row("follow_label"), st) && !menu::applies(row("follow_track_min"), st));
  st.follow = "BAW286";
  CHECK(menu::applies(row("follow_label"), st) && menu::applies(row("follow_track_min"), st));
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
  CHECK(!menu::typed_in(row("range_nm"), t, "6000") && t.range_nm == 15);
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
  // The watched aircraft, with a comma between, red where no color is given; written back the same way.
  CHECK(menu::typed_in(row("watch"), t, "n12345 Blue, BAW123,") && t.watch.size() == 2 && t.watch[0].id == "N12345" &&
        t.watch[0].color == 4 && t.watch[1].color == 0);
  CHECK(menu::text_of(row("watch"), t) == "N12345 blue, BAW123 red" && menu::shown(row("watch"), t) == "2 aircraft");
  CHECK(!menu::typed_in(row("watch"), t, "N1 pink") && t.watch.size() == 2);
  CHECK(menu::typed_in(row("watch"), t, "") && t.watch.empty() && menu::shown(row("watch"), t) == "None");
  // Rotation steps through the ways up the settings take.
  CHECK(menu::shown(row("rotation"), t) == "0\xC2\xB0" && menu::stepped(row("rotation"), t, 1) && t.rotation == 90);
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
  // Names asked for in English, and one the fonts cannot write left out: they hold Latin letters only.
  CHECK(nominatim_url("pdx").find("accept-language=en") != std::string::npos);
  CHECK(latin("Zürich") && latin("Łódź") && latin("St. Mary’s – East") && latin("Portland"));
  CHECK(!latin("နေပြည်တော်") && !latin("東京") &&
        !latin("Москва") && !latin("Plane 🛩"));

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
  const std::string burmese = R"([{"lat":"19.7","lon":"96.1","display_name":"နေပြည်, Myanmar",
      "class":"place","type":"city"},{"lat":"45.5","lon":"-122.6","display_name":"Portland, Oregon","class":"place",
      "type":"city"}])";
  std::vector<Found> written;
  CHECK(read_nominatim(burmese.c_str(), burmese.size(), "pdx", doc, written, error) && written.size() == 1 &&
        written[0].name == "Portland");
  // The FAA's first, then the airports, then the rest.
  std::reverse(places.begin(), places.end());
  const auto all = in_order(found, places);
  CHECK(all.size() == 3 && all[0].name == "KJFK" && all[1].name == "ZRH" && all[2].name == "Zürich");
}

// Where the screen is from the planes its radio hears (locate.h), and from its internet address (places.h).
static void test_locate() {
  using locate::Locator;
  // Too few planes: no place.
  Locator few;
  for (uint32_t i = 0; i < locate::FEWEST_PLANES - 1; ++i) few.add(i + 1, 40 + i * 0.1, -74, 10000, 1000, 1000);
  CHECK(!few.fix().found && few.size() == locate::FEWEST_PLANES - 1);
  // A plane heard again keeps its loudest place; a fainter one later does not move it.
  Locator one;
  one.add(7, 40, -74, 10000, 500, 1000);
  one.add(7, 41, -74, 10000, 900, 2000);
  one.add(7, 42, -74, 10000, 100, 3000);
  CHECK(one.size() == 1);
  // Six planes around a place on the date line, all as loud: their middle, not the far side of the world.
  Locator line;
  for (int i = 0; i < 6; ++i) {
    const geo::Place p = geo::destination(10, 180, i * 60, 30 * locate::NM);
    line.add(i + 1, p.lat, p.lon, 20000, 1000, 1000);
  }
  locate::Fix f = line.fix();
  CHECK(f.found && f.planes == 6 && geo::meters_between(f.lat, f.lon, 10, 180) < 100 && f.within_nm >= 1);
  // One far off (a bad position, or one from another receiver's ocean) is left out.
  line.add(99, 10, 160, 20000, 1000, 1000);
  f = line.fix();
  CHECK(f.found && f.planes == 6 && geo::meters_between(f.lat, f.lon, 10, 180) < 100);
  // Let go when not heard for KEEP_MS, also across millis() turning over.
  Locator turn;
  turn.add(1, 40, -74, 10000, 1000, 0xFFFFF000u);
  turn.expire(0xFFFFF000u + locate::KEEP_MS - 1);
  CHECK(turn.size() == 1);
  turn.expire(0xFFFFF000u + locate::KEEP_MS);
  CHECK(turn.size() == 0);
  // Full: a louder plane takes the faintest's place, a fainter one is not taken.
  Locator full;
  for (uint32_t i = 0; i < locate::MOST_PLANES; ++i) full.add(i + 1, 40, -74, 10000, 100 + i, 1000);
  full.add(1000, 40, -74, 10000, 50, 1000);
  CHECK(full.size() == locate::MOST_PLANES);
  full.add(1001, 40, -74, 10000, 5000, 1000);
  CHECK(full.size() == locate::MOST_PLANES);

  // Simulated traffic: planes flying straight past a receiver at random heights, distances and headings, each heard
  // every 10 seconds while within its horizon, louder by the square of how near (with a few decibels of fading), and
  // a few bad positions far off. The place found should be within the distance it says, nearly every time.
  uint64_t seed = 12345;
  auto random = [&]() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (seed >> 11) / 9007199254740992.0;
  };
  const int trials = 300;
  int within = 0, found = 0;
  double worst = 0;
  for (int trial = 0; trial < trials; ++trial) {
    const double lat = -60 + 120 * random(), lon = -180 + 360 * random();
    Locator loc;
    const int planes = 8 + (int) (40 * random());
    uint32_t now = 1000;
    for (int i = 0; i < planes; ++i) {
      const int feet = 2000 + (int) (38000 * random());
      const double reach = locate::horizon_nm(feet) - 10;
      // Where it passes nearest, and which way it flies.
      const geo::Place nearest = geo::destination(lat, lon, 360 * random(), reach * std::sqrt(random()) * locate::NM);
      const double heading = 360 * random(), knots = 150 + 350 * random();
      for (double t = -1800; t <= 1800; t += 10) {
        const geo::Place p = geo::destination(nearest.lat, nearest.lon, t < 0 ? heading + 180 : heading,
                                              std::fabs(t) / 3600 * knots * locate::NM);
        const double d = geo::meters_between(lat, lon, p.lat, p.lon) / locate::NM;
        if (d > reach) continue;
        const double fading = std::pow(10, (random() - 0.5) * 0.6);
        const double power = 4e8 / std::max(d * d + (feet / 6076.0) * (feet / 6076.0), 0.01) * fading;
        loc.add(i + 1, p.lat, p.lon, feet, (uint32_t) std::min(power, 4e9), now + (uint32_t) (t + 1800) * 1000 / 60);
      }
    }
    for (int i = 0; i < 2; ++i) loc.add(5000 + i, lat + 20 * (random() - 0.5), lon + 20, 30000, 3000, now);
    const locate::Fix fix = loc.fix();
    if (!fix.found) continue;
    ++found;
    const double off = geo::meters_between(lat, lon, fix.lat, fix.lon) / locate::NM;
    worst = std::max(worst, off);
    if (off <= fix.within_nm) ++within;
  }
  std::printf("  locate: %d of %d found, %d within what they said, the worst %.1f NM off\n", found, trials, within,
              worst);
  CHECK(found >= trials * 95 / 100 && within >= found * 95 / 100);

  // As a place to pick, and why there is none yet.
  f.planes = 12;
  f.within_nm = 4.2;
  const places::Found here = places::radio_place(f, false);
  CHECK(here.name.empty() && here.title == "Radio-based location" && here.source == "this screen's radio" &&
        here.detail.find("from 12 planes heard, within about 5 NM") != std::string::npos);
  CHECK(places::radio_place(f, true).detail.find("within about 8 km") != std::string::npos);
  CHECK(places::radio_waiting(2) == "The radio has heard 2 of the 6 planes it needs to say where the screen is");

  // freeipapi.com's answers: a town, one with only its country, one it cannot place, and one that is not JSON.
  JsonDocument doc;
  std::string error;
  places::Found town;
  const std::string sydney = R"({"latitude":-33.8688,"longitude":151.209,"cityName":"Sydney",
      "regionName":"New South Wales","countryName":"Australia","ipAddress":"1.1.1.1"})";
  CHECK(places::read_ip_place(sydney.c_str(), sydney.size(), doc, town, error) && town.name == "Sydney" &&
        town.detail.rfind("Sydney, New South Wales, Australia, from the internet address", 0) == 0 &&
        town.title == "Network-based location" && town.source == "freeipapi.com" && !town.airport);
  const std::string country = R"({"latitude":51.5,"longitude":10.5,"cityName":"-","regionName":"",
      "countryName":"Germany"})";
  CHECK(places::read_ip_place(country.c_str(), country.size(), doc, town, error) && town.name == "Germany");
  const std::string nowhere = R"({"latitude":0,"longitude":0,"cityName":"-","countryName":"-"})";
  CHECK(!places::read_ip_place(nowhere.c_str(), nowhere.size(), doc, town, error) && !error.empty());
  CHECK(!places::read_ip_place("<html>", 6, doc, town, error) && !error.empty());
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
  CHECK(range_nm(p.moved({249, 200}, {250, 200})) > 1000);  // measured where the middle moved to
  // Out to the whole range on any glass, a plane followed over an ocean in view: 5000 NM fits at the farthest zoom.
  CHECK(range_nm(geo::View(45.6, -122.6, geo::MIN_ZOOM, 480, 640)) == MAX_RANGE_NM);
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
  // The strip along the top, and a swipe down from it: far enough, and more down than across.
  CHECK(in_top_strip(0, 73) && in_top_strip(72, 73) && !in_top_strip(73, 73) && !in_top_strip(-1, 73));
  CHECK(downward(200, 20, 210, 130, 98) && !downward(200, 20, 210, 100, 98) && !downward(200, 20, 300, 130, 98) &&
        !downward(200, 130, 200, 20, 98));
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
  // Advisories over the place only: a SIGMET around it, another far away; G-AIRMETs of the hour valid now, each
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
  CHECK(!adsb::altitude13(0x0040 | 0x10, feet));  // meters
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
  const uint16_t gray = blend(BLACK, WHITE, 128);
  CHECK((gray >> 11) >= 15 && (gray >> 11) <= 16 && (gray >> 5 & 0x3F) >= 31 && (gray >> 5 & 0x3F) <= 32);
  CHECK(mix4(RED, BLUE, BLUE, BLUE, 256, 0, 0, 0) == RED);
  const uint16_t mixed = mix4(WHITE, BLACK, WHITE, BLACK, 64, 64, 64, 64);
  CHECK((mixed >> 11) == 15 && (mixed & 0x1F) == 15);

  // A 4 x 4 tile (in a row of 6) of red, its last column clear.
  uint16_t color[6 * 4];
  uint8_t alpha[6 * 4];
  for (int i = 0; i < 6 * 4; ++i) {
    color[i] = RED;
    alpha[i] = (i % 6) == 3 ? 0 : 255;
  }
  const Tile tile{color, alpha, 4, 4, 6};
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
  // Twice the size: a 2 x 2 tile, left red and right white, in 4 x 4. The outer columns are their own color, the
  // inner ones between the two, nearer their own side's.
  uint16_t two[4] = {RED, WHITE, RED, WHITE};
  uint8_t solid[4] = {255, 255, 255, 255};
  fresh(frame, 64);
  lay(frame, 8, 8, 0, 0, 2.0, Tile{two, solid, 2, 2, 2});
  CHECK(frame[0] == RED && frame[3] == WHITE);
  CHECK(frame[1] != RED && frame[1] != WHITE && (frame[1] & 0x1F) < (frame[2] & 0x1F));
  CHECK(frame[8 * 3 + 0] == RED && frame[4] == BLUE);  // its fourth row, and nothing past its width
  // A color enlarged is the same color.
  uint16_t one[4] = {RED, RED, RED, RED};
  fresh(frame, 64);
  lay(frame, 8, 8, 1, 1, 3.0, Tile{one, solid, 2, 2, 2});
  CHECK(frame[0] == BLUE && std::all_of(frame + 9, frame + 15, [](uint16_t p) { return p == RED; }));
  // A tile known to be opaque is laid just as one whose alphas say so, at its own size, smaller and larger.
  uint16_t varied[5 * 5];
  uint8_t full[5 * 5];
  for (int i = 0; i < 25; ++i) {
    varied[i] = (uint16_t) (i * 2741 + 77);
    full[i] = 255;
  }
  for (const double scale : {1.0, 0.5, 0.7, 1.6, 2.0, 3.3}) {
    for (const int at : {-3, 0, 2}) {
      uint16_t slow[16 * 16], quick[16 * 16];
      fresh(slow, 256);
      fresh(quick, 256);
      lay(slow, 16, 16, at, at + 1, scale, Tile{varied, full, 5, 5, 5, false});
      lay(quick, 16, 16, at, at + 1, scale, Tile{varied, full, 5, 5, 5, true});
      CHECK(std::equal(slow, slow + 256, quick));
    }
  }
}

static void test_pngrow() {
  using namespace pngrow;
  const tiles::Filter plain{};
  uint16_t color[8];
  uint8_t alpha[8];
  // A palette of four, the last see-through; rows packed 1, 2, 4 and 8 bits to a pixel.
  uint8_t palette[1024] = {};
  const uint8_t entries[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 3; ++c) palette[3 * i + c] = entries[i][c];
  for (int i = 0; i < 256; ++i) palette[768 + i] = i == 3 ? 0 : 255;
  Palette painted;
  paint_palette(palette, true, plain, painted);
  CHECK(painted.color[0] == 0xF800 && painted.color[2] == 0x001F && painted.alpha[3] == 0 && painted.alpha[1] == 255);
  const uint8_t one_bit[] = {0b10100000};
  convert({one_bit, 4, INDEXED, 1, true, 0}, plain, nullptr, &painted, color, alpha);
  CHECK(color[0] == 0x07E0 && color[1] == 0xF800 && color[2] == 0x07E0 && color[3] == 0xF800);
  const uint8_t two_bits[] = {0b00011011};
  convert({two_bits, 4, INDEXED, 2, true, 0}, plain, nullptr, &painted, color, alpha);
  CHECK(color[0] == 0xF800 && color[1] == 0x07E0 && color[2] == 0x001F && alpha[3] == 0 && alpha[2] == 255);
  const uint8_t four_bits[] = {0x21, 0x30};
  convert({four_bits, 3, INDEXED, 4, true, 0}, plain, nullptr, &painted, color, alpha);
  CHECK(color[0] == 0x001F && color[1] == 0x07E0 && alpha[2] == 0);
  const uint8_t eight_bits[] = {2, 0, 3};
  convert({eight_bits, 3, INDEXED, 8, true, 0}, plain, nullptr, &painted, color, alpha);
  CHECK(color[0] == 0x001F && color[1] == 0xF800 && alpha[2] == 0 && alpha[1] == 255);
  // Without a tRNS chunk every entry is whole.
  paint_palette(palette, false, plain, painted);
  CHECK(painted.alpha[3] == 255);
  // Gray, packed and whole, its tRNS gray see-through.
  const uint8_t gray2[] = {0b11000100};
  convert({gray2, 3, GRAY, 2, true, 1}, plain, nullptr, nullptr, color, alpha);
  CHECK(color[0] == 0xFFFF && color[1] == 0 && alpha[2] == 0 && alpha[0] == 255);
  const uint8_t gray_alpha[] = {255, 128, 0, 255};
  convert({gray_alpha, 2, GRAY_ALPHA, 8, false, 0}, plain, nullptr, nullptr, color, alpha);
  CHECK(color[0] == 0xFFFF && alpha[0] == 128 && color[1] == 0 && alpha[1] == 255);
  // Color, with its tRNS color see-through, and with alpha (a place name's edge).
  const uint8_t rgb[] = {255, 0, 0, 1, 2, 3, 0, 0, 255};
  convert({rgb, 3, RGB, 8, true, 0x010203}, plain, nullptr, nullptr, color, alpha);
  CHECK(color[0] == 0xF800 && alpha[0] == 255 && alpha[1] == 0 && color[2] == 0x001F);
  const uint8_t rgba[] = {0, 255, 0, 200, 9, 9, 9, 0};
  convert({rgba, 2, RGBA, 8, false, 0}, plain, nullptr, nullptr, color, alpha);
  CHECK(color[0] == 0x07E0 && alpha[0] == 200 && alpha[1] == 0);
  // Through a filter: the palette painted by it, the colors by its table just as by painting them, and the radar's
  // opacity on every alpha.
  const tiles::Filter turned{tiles::NIGHT_INVERT, 256, 128};
  paint_palette(palette, true, turned, painted);
  CHECK(painted.color[3] == tiles::paint(turned, 255, 255, 255) && painted.alpha[0] == 128 && painted.alpha[3] == 0);
  std::vector<uint16_t> table(tiles::PAINT_TABLE);
  tiles::paint_table(turned, table.data());
  const uint8_t white[] = {255, 255, 255, 0, 0, 0};
  convert({white, 2, RGB, 8, false, 0}, turned, table.data(), nullptr, color, alpha);
  CHECK(color[0] == tiles::paint(turned, 255, 255, 255) && color[1] == tiles::paint(turned, 0, 0, 0) && alpha[0] == 128);
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

static void test_tour() {
  using tour::Guide;
  using tour::Seen;
  Seen seen;
  seen.planes = true;
  Guide g;
  g.start();
  // The welcome waits for its key, whatever the glass shows.
  CHECK(g.step() == tour::WELCOME && !g.look(seen, 1000) && !g.look(seen, 60000) && g.step() == tour::WELCOME);
  CHECK(g.next(seen) && g.step() == tour::PLANE && !g.done());
  CHECK(std::string(tour::words(tour::PLANE, seen).ask) == "Tap a plane.");
  // A step waits for its touch as long as it takes.
  CHECK(!g.look(seen, 70000) && !g.look(seen, 700000) && g.step() == tour::PLANE);
  // The touch made: said at once, and the step stays while what it opened is open.
  seen.card = true;
  CHECK(g.look(seen, 701000) && g.done() && g.step() == tour::PLANE);
  CHECK(!g.look(seen, 800000) && g.step() == tour::PLANE);
  // Put away: the words stay a moment, then the next step.
  seen.card = false;
  CHECK(!g.look(seen, 800100) && !g.look(seen, 800100 + Guide::HOLD_MS - 1) && g.step() == tour::PLANE);
  CHECK(g.look(seen, 800100 + Guide::HOLD_MS) && g.step() == tour::ADDRESS && !g.done());
  // The address goes by itself and is in nobody's way: the next step does not wait for it.
  seen.address = true;
  CHECK(g.look(seen, 900000) && g.done());
  CHECK(!g.look(seen, 900100) && g.look(seen, 900100 + Guide::HOLD_MS) && g.step() == tour::BRIGHTNESS);
  seen.address = false;
  // Opened again before the wait is over, the wait starts again.
  seen.level = true;
  CHECK(g.look(seen, 1000000));
  seen.level = false;
  CHECK(!g.look(seen, 1000100));
  seen.level = true;
  CHECK(!g.look(seen, 1001000));
  seen.level = false;
  CHECK(!g.look(seen, 1002000) && !g.look(seen, 1002000 + Guide::HOLD_MS - 1) && g.step() == tour::BRIGHTNESS);
  CHECK(g.look(seen, 1002000 + Guide::HOLD_MS) && g.step() == tour::ZOOM);
  // The key goes on without the touch, through every step to the goodbye and out.
  const tour::Step rest[] = {tour::MAPS, tour::WEATHER, tour::TALLY, tour::MENU, tour::FINISH};
  for (tour::Step next : rest) CHECK(g.next(seen) && g.step() == next && !g.over());
  CHECK(!g.look(seen, 2000000) && g.step() == tour::FINISH);
  CHECK(g.next(seen) && g.over() && !g.next(seen) && !g.look(seen, 2000001));
  // The menu is the last touch: the goodbye comes once it is closed.
  g.start();
  while (g.step() != tour::MENU) g.next(seen);
  seen.menu = true;
  CHECK(g.look(seen, 5000) && !g.look(seen, 500000) && g.step() == tour::MENU);
  seen.menu = false;
  CHECK(!g.look(seen, 500100) && g.look(seen, 500100 + Guide::HOLD_MS) && g.step() == tour::FINISH);
  // A wait that spans the clock's turn (ticks.h) is still that long.
  g.start();
  g.next(seen);
  seen.card = true;
  CHECK(g.look(seen, 0xFFFFFF00u));
  seen.card = false;
  CHECK(!g.look(seen, 0xFFFFFFF0u) && !g.look(seen, 100) && g.look(seen, 0xFFFFFFF0u + Guide::HOLD_MS));
  CHECK(g.step() == tour::ADDRESS);

  // A screen with only its radar has no map to swipe to: that step is left out, and its dot.
  Seen bare;
  bare.can_swipe = false;
  CHECK(tour::touches(seen) == 8 && tour::touches(bare) == 7);
  CHECK(tour::after(tour::ZOOM, seen) == tour::MAPS && tour::after(tour::ZOOM, bare) == tour::WEATHER);
  CHECK(tour::touch_index(tour::WELCOME, seen) == -1 && tour::touch_index(tour::PLANE, seen) == 0);
  CHECK(tour::touch_index(tour::WEATHER, seen) == 5 && tour::touch_index(tour::WEATHER, bare) == 4);
  CHECK(tour::touch_index(tour::FINISH, seen) == 8 && tour::after(tour::FINISH, seen) == tour::STEPS);
  // With no plane on the glass the first step says so instead of asking for a tap that cannot be made.
  CHECK(std::string(tour::words(tour::PLANE, bare).ask).find("No plane") == 0);
  // Every step has a name, a heading and something to do; only the steps with a touch can be made.
  for (int s = 0; s < tour::STEPS; ++s) {
    const tour::Words w = tour::words((tour::Step) s, seen);
    CHECK(*tour::NAMES[s] && *w.title && *w.ask);
  }
  Seen all;
  all.card = all.address = all.level = all.zooming = all.swiping = all.weather = all.tally = all.menu = true;
  CHECK(!tour::made(tour::WELCOME, all) && !tour::made(tour::FINISH, all) && tour::made(tour::TALLY, all));
  CHECK(!tour::open(tour::ADDRESS, all) && tour::open(tour::WEATHER, all));

  // The setting: a new screen is shown the tour, and it is a setting like any other.
  Settings s;
  CHECK(s.tour);
  JsonDocument change;
  change["tour"] = false;
  std::string error;
  CHECK(apply(change.as<JsonObjectConst>(), s, error) && !s.tour);
  change["tour"] = "yes";
  CHECK(!apply(change.as<JsonObjectConst>(), s, error) && error == "tour" && !s.tour);
  JsonDocument out;
  to_json(s, out.to<JsonObject>());
  CHECK(out["tour"].is<bool>() && out["tour"] == false);
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
  // Firmware sent from a computer: busy from its start, once, and no longer once it fails; the page's own install
  // stays as it was through it.
  Tracker c;
  CHECK(!c.busy() && c.upload_started() && c.busy());
  CHECK(!c.upload_started());
  c.upload_ended();
  CHECK(!c.busy() && c.error.empty());
  // The screen's own install says it is updating already: its own download is no second start.
  CHECK(u.busy() && !u.upload_started() && !u.uploading);
  u.upload_ended();
  CHECK(u.busy() && u.updating);
}

// What tests/mock_services.py answers in place of each public service (tests/run.sh writes it to $MOCK_ANSWERS), read
// with the screen's own parsers: what a test against the mock expects is what the screen makes of it.
// Which way to look for a plane, and when it passes near.
static void test_sight() {
  CHECK(near(geo::bearing(0, 0, 1, 0), 0, 1e-9) && near(geo::bearing(0, 0, 0, 1), 90, 1e-9));
  CHECK(near(geo::bearing(0, 0, -1, 0), 180, 1e-9) && near(geo::bearing(0, 0, 0, -1), 270, 1e-9));
  CHECK(std::string(geo::compass_point(0)) == "N" && std::string(geo::compass_point(44)) == "NE" &&
        std::string(geo::compass_point(350)) == "N" && std::string(geo::compass_point(225)) == "SW");
  // 10 NM east at 10,000 ft: about 9.4 degrees up (curve and all).
  const double lat = 40.0, lon = -74.0;
  const geo::Place east = geo::destination(lat, lon, 90, 10 * geo::NM);
  geo::Sight s = geo::sight(lat, lon, east.lat, east.lon, 10000 * 0.3048);
  CHECK(near(s.bearing, 90, 0.1) && near(s.meters, 10 * geo::NM, 1) && near(s.elevation, 9.3, 0.2) && !s.nearing);
  // 35,000 ft 250 NM off is below the horizon.
  const geo::Place far = geo::destination(lat, lon, 0, 250 * geo::NM);
  CHECK(geo::sight(lat, lon, far.lat, far.lon, 35000 * 0.3048).elevation < 0);
  // Flying west at 240 kt, 1 NM north of the line: it passes 1 NM off in 2.5 minutes; flying east, it goes away.
  const geo::Place off = geo::destination(east.lat, east.lon, 0, geo::NM);
  s = geo::sight(lat, lon, off.lat, off.lon, 3000, true, 240 * geo::NM / 3600, 270);
  CHECK(s.nearing && near(s.closest_meters, geo::NM, 30) && near(s.closest_s, 150, 3));
  CHECK(!geo::sight(lat, lon, off.lat, off.lon, 3000, true, 240 * geo::NM / 3600, 90).nearing);
  // The card's words.
  feed::Flight f;
  f.lat = off.lat;
  f.lon = off.lon;
  f.has_altitude = true;
  f.altitude = 3000;
  f.has_speed = f.has_heading = true;
  f.speed = 200;  // 10 NM in 3 minutes
  f.heading = 270;
  auto said = [&](bool metric) { return overlay::sight_text(overlay::sight_of(f, lat, lon), metric); };
  CHECK(said(false) == "E, 3\xC2\xB0 up, 10 NM, passes 1.0 NM off in 3 min");
  CHECK(said(true) == "E, 3\xC2\xB0 up, 19 km, passes 1.9 km off in 3 min");
  f.lat = east.lat;
  f.lon = east.lon;
  CHECK(overlay::sight_text(overlay::sight_of(f, lat, lon), false) == "E, 3\xC2\xB0 up, 10 NM, overhead in 3 min");
  f.heading = 90;
  CHECK(overlay::sight_text(overlay::sight_of(f, lat, lon), false) == "E, 3\xC2\xB0 up, 10 NM");
  f.lat = lat + 0.001;
  f.lon = lon;
  CHECK(overlay::sight_text(overlay::sight_of(f, lat, lon), false) == "Overhead");
  CHECK(overlay::distance_text(12.4 * geo::NM, false) == "12 NM" && overlay::distance_text(950, true) == "0.9 km");
}

// A plane's photo from Planespotters.net: one that can be shown as its terms ask, or none.
static void test_photos() {
  CHECK(photos::url_for("a1b2c3") == "https://api.planespotters.net/pub/photos/hex/A1B2C3");
  JsonDocument doc;
  std::string error;
  photos::Photo p;
  const std::string one = R"({"photos":[{"id":"000001","thumbnail":{"src":"https://cdn.planespotters.net/1_t.jpg",
      "size":{"width":200,"height":133}},"thumbnail_large":{"src":"https://cdn.planespotters.net/1_280.jpg",
      "size":{"width":420,"height":280}},"link":"https://www.planespotters.net/photo/000001/b-hoy",
      "photographer":"Thomas Noack"}]})";
  CHECK(photos::read(one.c_str(), one.size(), doc, p, error) && p.known &&
        p.src == "https://cdn.planespotters.net/1_t.jpg" &&
        p.link == "https://www.planespotters.net/photo/000001/b-hoy" && p.width == 200 && p.height == 133);
  CHECK(photos::credit(p) == "Photo: Thomas Noack, planespotters.net");
  // None, and one without its photographer or its page (it could not be credited or linked): no photo, no error.
  const std::string none = R"({"photos":[]})";
  CHECK(photos::read(none.c_str(), none.size(), doc, p, error) && !p.known);
  const std::string uncredited = R"({"photos":[{"thumbnail":{"src":"https://cdn.planespotters.net/1_t.jpg"},
      "link":"https://www.planespotters.net/photo/1"}]})";
  CHECK(photos::read(uncredited.c_str(), uncredited.size(), doc, p, error) && !p.known);
  // An error, and an answer that is none.
  const std::string refused = R"({"error":"Too many requests"})";
  CHECK(!photos::read(refused.c_str(), refused.size(), doc, p, error) && error == "Planespotters: Too many requests");
  CHECK(!photos::read("{}", 2, doc, p, error) && !photos::read("<html>", 6, doc, p, error));
}

// The speaker's sounds: when each is heard, how loud, and the samples themselves.
static void test_sound() {
  using namespace sound;
  // The volume: silent at 0, then the media player's range (volume_min to volume_max in the board's YAML).
  CHECK(codec_volume(0) == 0 && codec_volume(-5) == 0);
  CHECK(near(codec_volume(1), CODEC_LOW + 0.003, 0.001) && near(codec_volume(100), CODEC_HIGH, 1e-6) &&
        near(codec_volume(150), CODEC_HIGH, 1e-6));
  CHECK(near(codec_volume(50), (CODEC_LOW + CODEC_HIGH) / 2, 1e-6));
  for (int p = 0; p <= 100; ++p) CHECK(percent_of(p / 100.0f) == p);
  CHECK(percent_of(-1) == 0 && percent_of(2) == 100 && percent_of(NAN) == 0);
  CHECK(cue_named("watched") == WATCHED && cue_named("level") == LEVEL && cue_named("nope") < 0 && cue_named(nullptr) < 0);
  CHECK(rank(LOST_RADIO) > rank(WATCHED) && rank(WATCHED) > rank(OVERHEAD) && rank(OVERHEAD) > rank(CLICK));
  // An emergency by its squawk: a klaxon for 7500, the falling pair for 7600, the ELT for 7700 and any other.
  CHECK(emergency_cue("7500") == HIJACK && emergency_cue("7600") == LOST_RADIO && emergency_cue("7700") == DISTRESS &&
        emergency_cue("") == DISTRESS && emergency_cue("1200") == DISTRESS);
  CHECK(rank(HIJACK) > rank(LOST_RADIO) && rank(DISTRESS) > rank(LOST_RADIO) && cue_named("distress") == DISTRESS);

  // When each sounds.
  Settings s;
  const bool fitted = speaker_fitted;
  speaker_fitted = false;
  CHECK(!sounds(WATCHED, s, 600, false));  // no speaker, no sound
  speaker_fitted = true;
  CHECK(sounds(WATCHED, s, 600, false) && sounds(LOST_RADIO, s, 600, false) && sounds(OVERHEAD, s, 600, false));
  CHECK(!sounds(CLICK, s, 600, false));  // clicks are off to start with
  s.clicks = true;
  CHECK(sounds(CLICK, s, 600, false));
  s.sound_watched = false;
  CHECK(!sounds(WATCHED, s, 600, false) && sounds(LEVEL, s, 600, false));
  s.volume = 0;
  CHECK(!sounds(LOST_RADIO, s, 600, false) && !sounds(LEVEL, s, 600, false));
  s = Settings{};
  // Bedtime from 22:00 to 07:00: quiet then, but for a hijack or distress where asked (never a lost radio); not quiet
  // with bedtime off.
  s.bedtime = true;
  CHECK(quiet(s, 23 * 60, false) && quiet(s, 3 * 60, false) && !quiet(s, 7 * 60, false) && !quiet(s, 12 * 60, true));
  CHECK(!sounds(WATCHED, s, 23 * 60, false) && !sounds(LOST_RADIO, s, 23 * 60, false) && sounds(WATCHED, s, 12 * 60, false));
  CHECK(!sounds(HIJACK, s, 23 * 60, false) && !sounds(DISTRESS, s, 23 * 60, false));
  s.distress_always = true;
  CHECK(!sounds(LOST_RADIO, s, 23 * 60, false) && sounds(HIJACK, s, 23 * 60, false) &&
        sounds(DISTRESS, s, 23 * 60, false) && !sounds(OVERHEAD, s, 23 * 60, false));
  CHECK(sounds(LOST_RADIO, s, 12 * 60, false));
  s.sound_emergency = false;
  CHECK(!sounds(HIJACK, s, 12 * 60, false) && !sounds(DISTRESS, s, 12 * 60, false));
  s.sound_emergency = true;
  s.quiet_bedtime = false;
  CHECK(!quiet(s, 23 * 60, true) && sounds(WATCHED, s, 23 * 60, false));
  s.quiet_bedtime = true;
  // With no time to come on again, quiet while bedtime keeps the screen dark; before the clock is set, the same.
  s.bed_wake = false;
  CHECK(quiet(s, 12 * 60, true) && !quiet(s, 23 * 60, false));
  s.bed_wake = true;
  CHECK(quiet(s, -1, true) && !quiet(s, -1, false));
  s.bedtime = false;
  CHECK(!quiet(s, 23 * 60, true));
  speaker_fitted = fitted;

  // The samples: as long as their notes ring, never past half of full scale, rising from silence and fading to it.
  for (int c = 0; c < CUES; ++c) {
    const Samples a = render((Cue) c);
    int end_ms = 0;
    for (const Note &n : notes((Cue) c)) end_ms = std::max(end_ms, n.at_ms + n.ring_ms);
    CHECK(!a.empty() && a.size() == (size_t) end_ms * RATE / 1000);
    int peak = 0;
    for (int16_t v : a) peak = std::max(peak, std::abs((int) v));
    CHECK(peak > 3000 && peak <= 16384);
    CHECK(std::abs((int) a.front()) < 100 && std::abs((int) a.back()) < 200);
    CHECK(render((Cue) c, 16000).size() == (size_t) end_ms * 16);
  }
  // A click is short; an emergency the longest.
  CHECK(render(CLICK).size() < (size_t) RATE / 50 && render(LOST_RADIO).size() > render(WATCHED).size());
  // The ELT sweeps down three times a second, and starts again from the top; the klaxon climbs and holds.
  {
    const Note elt = notes(DISTRESS)[0];
    CHECK(elt.voice == TONE && elt.repeat && elt.hz - elt.to_hz >= 700 && elt.hz <= 1600 && elt.to_hz >= 300);
    CHECK(near(pitch_at(elt, 0), 1600, 0.01) && near(pitch_at(elt, 166.5f), 1000, 1) && pitch_at(elt, 330) < 420 &&
          near(pitch_at(elt, 333.5f), 1598, 2));
    const int per_s = 1000 / elt.glide_ms;
    CHECK(per_s >= 2 && per_s <= 4 && elt.ring_ms >= 2000);
    // Its pitch heard in the samples: crossings of zero in the first and last 50 ms of a sweep.
    const Samples a = render(DISTRESS);
    auto crossings = [&](size_t from, size_t n) {
      int count = 0;
      for (size_t i = from + 1; i < from + n; ++i) count += (a[i - 1] < 0) != (a[i] < 0);
      return count;
    };
    const size_t sweep = (size_t) elt.glide_ms * RATE / 1000, part = RATE / 20;
    CHECK(crossings(sweep, part) > 2 * crossings(2 * sweep - part, part));
    const Note horn = notes(HIJACK)[0];
    CHECK(horn.voice == HORN && notes(HIJACK).size() == 3 && pitch_at(horn, 0) == horn.hz &&
          pitch_at(horn, 600) == horn.to_hz && horn.to_hz > horn.hz);
  }
}

// What the screen has seen in range: visits, the day's tally, the log and the alerts.
static void test_sightings() {
  using namespace sightings;
  Settings s;
  s.located = true;
  s.lat = 40.0;
  s.lon = -74.0;
  s.range_nm = 20;
  s.watch = {{"N12345", 4}};
  auto plane = [&](const char *hex, double north_nm, const char *callsign = "", const char *type = "B738") {
    feed::Flight f;
    f.hex = hex;
    f.callsign = callsign;
    f.type = type;
    f.operator_name = "Test Air";
    const geo::Place p = geo::destination(s.lat, s.lon, 0, north_nm * geo::NM);
    f.lat = p.lat;
    f.lon = p.lon;
    f.has_altitude = true;
    f.altitude = 12000;
    f.has_speed = true;
    f.speed = 300;
    return f;
  };
  Book book;
  std::vector<Alert> alerts;
  std::vector<Visit> ended;
  const int64_t t0 = 1793347800;
  // Two in range, one beyond it, and a vehicle on the ground (not counted).
  feed::Flight truck = plane("e00009", 1, "", "");
  truck.category = "C1";
  std::vector<feed::Flight> report = {plane("a00001", 5, "UAL1"), plane("a00002", 12, "N12345", "C172"),
                                      plane("a00003", 40, "DAL3"), truck};
  book.see(report, s, t0, 20261110, 14, alerts, ended);
  CHECK(book.today().planes == 2 && book.today().hours[14] == 2 && book.in_range() == 2 && book.today().busiest == 2);
  CHECK(alerts.size() == 1 && alerts[0].kind == WATCHED && alerts[0].name == "N12345" && alerts[0].color == 5 &&
        alerts[0].text == "N12345 is in range, 12 NM N");
  CHECK(book.today().closest.set && std::string(book.today().closest.name) == "UAL1" &&
        near(book.today().closest.value, 5 * geo::NM, 1));
  // The same planes again: no new count, no new alert; one squawks 7700.
  alerts.clear();
  report[0].squawk = "7700";
  report[0].emergency = true;
  report[0].lat = geo::destination(s.lat, s.lon, 0, 2 * geo::NM).lat;
  book.see(report, s, t0 + 10, 20261110, 14, alerts, ended);
  CHECK(book.today().planes == 2 && alerts.size() == 1 && alerts[0].kind == EMERGENCY &&
        alerts[0].text == "UAL1 squawks 7700, 2.0 NM N" && book.today().emergencies == 1);
  alerts.clear();
  book.see(report, s, t0 + 20, 20261110, 14, alerts, ended);
  CHECK(alerts.empty());
  // A plane passing close by, where asked for: within 3 NM and below 13000 ft, once a visit; not the one 12 NM off,
  // nor one too high.
  {
    Book near_book;
    Settings c = s;
    c.overhead = true;
    c.overhead_nm = 3;
    c.overhead_ft = 13000;
    feed::Flight high = plane("a00005", 1, "JBU5");
    high.altitude = 35000;
    std::vector<Alert> close;
    near_book.see({plane("a00004", 2.5, "SWA4"), report[1], high}, c, t0, 20261110, 14, close, ended);
    CHECK(close.size() == 2 && close[0].kind == OVERHEAD && close[0].name == "SWA4" && close[1].kind == WATCHED &&
          close[0].text == "SWA4 is close by, 2.5 NM N, 12000 ft");
    close.clear();
    near_book.see({plane("a00004", 1, "SWA4")}, c, t0 + 10, 20261110, 14, close, ended);
    CHECK(close.empty());
    // Off, none.
    Book off_book;
    near_book.close_all(ended);
    off_book.see({plane("a00006", 1, "SWA6")}, s, t0, 20261110, 14, close, ended);
    CHECK(close.empty());
    ended.clear();
  }
  // Both gone for longer than a visit's gap: their visits end, into the log; then one comes back, counted once still.
  book.see({}, s, t0 + 20 + VISIT_GAP_S + 1, 20261110, 15, alerts, ended);
  CHECK(ended.size() == 2 && book.open_visits() == 0 && book.log().size() == 2);
  CHECK(near(ended[0].closest_m, 2 * geo::NM, 1) && ended[0].emergency && std::string(ended[0].squawk) == "7700" &&
        ended[0].lowest_ft == 12000 && ended[1].watched);
  ended.clear();
  book.see({report[1]}, s, t0 + 2000, 20261110, 15, alerts, ended);
  CHECK(book.today().planes == 2 && book.today().visits == 3 && alerts.size() == 1 && alerts[0].kind == WATCHED);
  const auto types = book.top_types();
  CHECK(types.size() == 2 && types[0].n == 1);
  // The day as its page on the glass tells it (today.h).
  {
    CHECK(today::summary(book) == "2 planes today, 1 in range now");
    CHECK(today::summary(Book{}).rfind("Nothing seen yet", 0) == 0 && today::lines(Book{}, false).empty());
    CHECK(today::busiest_hour(book.today()) == 14 && today::busiest_hour(Day{}) == -1);
    CHECK(today::day_name(20261110) == "Nov 10" && today::day_name(20260101) == "Jan 1" && today::day_name(0) == "");
    auto has = [](const std::vector<today::Line> &lines, today::Tone tone, const std::string &text) {
      for (const auto &l : lines)
        if (l.tone == tone && l.text == text) return true;
      return false;
    };
    const auto lines = today::lines(book, false);
    CHECK(has(lines, today::QUIET, "3 visits, busiest hour 14:00 (2)"));
    // The tiles at the top: the planes today, those in range now, the most at once; none before the clock is set.
    const auto stats = today::stats(book);
    CHECK(stats.size() == 3 && stats[0].value == "2" && stats[0].caption == "planes today" && stats[1].value == "1" &&
          stats[2].value == "2" && stats[2].caption == "most at once");
    CHECK(today::stats(Book{}).empty());
    // A record's row has its parts for the page's columns; a list's row its share of the longest.
    for (const auto &l : lines) {
      if (l.text == "Nearest  UAL1, 2.0 NM") CHECK(l.label == "Nearest" && l.name == "UAL1" && l.value == "2.0 NM" && l.share < 0);
      if (l.tone != today::HEADING && l.label.empty() && !l.name.empty()) CHECK(l.share > 0 && l.share <= 1 && !l.value.empty());
    }
    CHECK(has(lines, today::PLAIN, "2 watched planes, 1 emergency"));
    CHECK(has(lines, today::HEADING, "Records today") && has(lines, today::PLAIN, "Nearest  UAL1, 2.0 NM"));
    CHECK(has(lines, today::PLAIN, "Highest  UAL1, 12000 ft") && has(lines, today::PLAIN, "Fastest  UAL1, 300 kt"));
    CHECK(has(lines, today::HEADING, "Most common types"));
    // Metric: kilometers, meters and kilometers an hour.
    const auto metric = today::lines(book, true);
    CHECK(has(metric, today::PLAIN, "Nearest  UAL1, 3.7 km") && has(metric, today::PLAIN, "Highest  UAL1, 3658 m") &&
          has(metric, today::PLAIN, "Fastest  UAL1, 556 km/h"));
  }
  // Two planes known by made-up addresses ("~", TIS-B) are two planes, and neither is the transponder at that address.
  {
    Book apart;
    std::vector<Alert> none;
    std::vector<Visit> over;
    feed::Flight radar1 = plane("~2ba403", 3, "", ""), radar2 = plane("~2ba404", 4, "", ""), real = plane("2ba403", 6);
    apart.see({radar1, radar2, real}, s, t0, 20261110, 15, none, over);
    CHECK(apart.today().planes == 3);
    CHECK(feed::made_up_address(radar1) && !feed::made_up_address(real) && overlay::name_of(radar1) == "No ID");
  }
  // Kept over a restart, and read back the same.
  const std::vector<uint8_t> bytes = book.pack();
  Book again;
  CHECK(again.unpack(bytes.data(), bytes.size()) && again.today().planes == 2 && again.today().hours[14] == 2 &&
        std::string(again.today().closest.name) == "UAL1" && again.top_operators().size() == 1 &&
        again.top_operators()[0].n == 2 && again.days().size() == 1 && again.days()[0].planes == 2);
  alerts.clear();
  again.see({report[0]}, s, t0 + 3000, 20261110, 15, alerts, ended);
  CHECK(again.today().planes == 2);  // told apart over the restart
  CHECK(!again.unpack(bytes.data(), bytes.size() - 1) && !again.unpack(bytes.data(), 3));
  // A new day starts a new tally, and the days before are kept.
  again.see({report[0]}, s, t0 + 86400, 20261111, 9, alerts, ended);
  CHECK(again.today().planes == 1 && again.today().date == 20261111 && again.days().size() == 2 &&
        again.days()[0].planes == 2 && again.days()[1].planes == 1);
  // The log as CSV.
  auto local = [](int64_t t) { return std::to_string(t); };
  CHECK(csv_header(false).rfind("first,last,hex,callsign", 0) == 0);
  CHECK(csv_line(book.log()[0], false, local) ==
        std::to_string(t0) + "," + std::to_string(t0 + 20) + ",a00001,UAL1,,B738,Test Air,2.0,12000,12000,,7700\n");
  // Its distances in tenths, written without printf's floats (numbers.h), as "%.1f" would write them.
  {
    Visit v = book.log()[0];
    auto closest = [&](double meters, bool metric) {
      v.closest_m = meters;
      const std::string line = csv_line(v, metric, local);
      size_t at = 0;
      for (int comma = 0; comma < 7; ++comma) at = line.find(',', at) + 1;
      return line.substr(at, line.find(',', at) - at);
    };
    CHECK(closest(0, false) == "0.0" && closest(12.36 * geo::NM, false) == "12.4" && closest(1500, true) == "1.5");
    CHECK(closest(9.96 * geo::NM, false) == "10.0" && closest(250 * geo::NM, false) == "250.0");
  }
  // What /api/stats answers. Its names are copied in: they once pointed into lists gone before the reply was written,
  // and the page showed whatever had taken their place. Under ASan a name left pointing stops the run here.
  {
    std::string text;
    {
      JsonDocument doc;
      stats_json(book, doc.to<JsonObject>());
      serializeJson(doc, text);
    }
    JsonDocument back;
    CHECK(!deserializeJson(back, text));
    CHECK(back["date"] == "2026-11-10" && back["planes"] == 2 && back["log"] == book.log().size());
    CHECK(back["types"].size() == 2 && back["types"][0]["name"] == std::string(book.top_types()[0].name) &&
          back["types"][0]["count"] == 1);
    CHECK(back["operators"].size() == 1 && back["operators"][0]["name"] == "Test Air" &&
          back["operators"][0]["count"] == 2);
    CHECK(back["closest_nm"]["name"] == "UAL1" && back["closest_nm"]["value"] == 2.0 &&
          back["highest_ft"]["value"] == 12000 && back["fastest_kt"]["value"] == 300);
    CHECK(back["days"].size() == 1 && back["days"][0]["date"] == "2026-11-10" && back["days"][0]["planes"] == 2);
  }
}

// Numbers as the web server writes them, without printf's floats (numbers.h).
static void test_numbers() {
  using sky::numbers::tenths;
  using sky::numbers::text;
  CHECK(text(12) == "12" && text(0.5) == "0.5" && text(-3.25) == "-3.25" && text(19.507) == "19.507");
  CHECK(text(1.0004) == "1" && text(0.9996) == "1" && text(-0.0004) == "0" && text(-39) == "-39");
  CHECK(text(4294967296.0) == "4294967296" && text(0.05, 1) == "0.1" && text(2.5, 0) == "3" && text(1.25, 9) == "1.25");
  CHECK(text(NAN) == "NaN" && text(INFINITY) == "+Inf" && text(-INFINITY) == "-Inf");
  CHECK(tenths(2.04) == "2.0" && tenths(9.96) == "10.0" && tenths(-0.5) == "-0.5" && tenths(0) == "0.0");
}

static void test_mock_answers() {
  const char *folder = std::getenv("MOCK_ANSWERS");
  if (!folder) {
    std::printf("  (no MOCK_ANSWERS: the mock's answers are not read)\n");
    return;
  }
  auto read = [&](const char *name) {
    std::ifstream file(std::string(folder) + "/" + name + ".json");
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
  };
  const double lat = 40.6413, lon = -73.7781;
  JsonDocument doc;
  std::string error;
  // The airspace: a Class B, a C and two D (the E passed over), and three runways, two of them at the B's airport.
  airspace::Map map;
  std::vector<uint32_t> airports;
  const std::string areas = read("airspace"), runways = read("runways");
  CHECK(airspace::read_airspace(areas.c_str(), areas.size(), doc, map, airports, error) && map.areas.size() == 4);
  CHECK(airspace::read_runways(runways.c_str(), runways.size(), doc, airports, map, error) && map.runways.size() == 3);
  CHECK(map.runways[0].towered && map.runways[1].towered && !map.runways[2].towered);
  // The weather at its station, with the advisories over the place.
  std::vector<weather::Metar> metars;
  const std::string metar = read("metar"), taf = read("taf"), mos = read("mos");
  CHECK(weather::read_metars(metar.c_str(), metar.size(), doc, metars, error) && metars.size() == 1 &&
        metars[0].id == "KTST" && metars[0].category == weather::VFR);
  weather::Taf t;
  CHECK(weather::read_taf(taf.c_str(), taf.size(), doc, t, error) && t.raw.rfind("TAF KTST", 0) == 0);
  CHECK(weather::taf_lines(t.raw).size() == 3);
  std::vector<weather::MosRow> rows;
  CHECK(weather::read_mos(mos.c_str(), mos.size(), doc, rows, error) && rows.size() == 16);
  std::vector<weather::Advisory> advisories;
  const std::string sigmets = read("airsigmet"), gairmets = read("gairmet"), pireps = read("pirep");
  CHECK(weather::read_sigmets(sigmets.c_str(), sigmets.size(), doc, lat, lon, 20, advisories, error) &&
        advisories.size() == 1);
  CHECK(weather::read_gairmets(gairmets.c_str(), gairmets.size(), doc, lat, lon, 20, advisories, error) &&
        advisories.size() == 2);
  std::vector<weather::Pirep> reports;
  CHECK(weather::read_pireps(pireps.c_str(), pireps.size(), doc, reports, error) && reports.size() == 1);
  // The radar's latest scan, the airport and the place searched for, and an exchange's planes.
  const std::string latest = read("radar-latest");
  CHECK(radar::read_latest(latest.c_str(), latest.size(), doc) == 1793347800u / 60 - 5);
  std::vector<places::Found> found;
  const std::string faa = read("airports"), osm = read("places");
  CHECK(places::read_faa(faa.c_str(), faa.size(), doc, found, error) && found.size() == 1 && found[0].name == "KTST");
  found.clear();
  CHECK(places::read_nominatim(osm.c_str(), osm.size(), "testville", doc, found, error) && found.size() == 1 &&
        found[0].name == "Testville");
  // Where the screen's address is: the mock's town, a little south of its place.
  places::Found here;
  const std::string ip = read("ip-place");
  CHECK(places::read_ip_place(ip.c_str(), ip.size(), doc, here, error) && here.name == "Testville" &&
        here.title == "Network-based location" &&
        here.detail.rfind("Testville, New York, United States of America, from", 0) == 0 && here.lat < lat &&
        near(here.lon, lon, 1e-3));
  std::vector<feed::Flight> flights;
  const std::string exchange = read("exchange");
  CHECK(feed::parse(exchange.c_str(), exchange.size(), doc, flights, error) && flights.size() == 1);
  // A plane looked up by its callsign, worldwide (follow.h): that plane.
  flights.clear();
  const std::string lookup = read("lookup");
  CHECK(feed::parse(lookup.c_str(), lookup.size(), doc, flights, error) && flights.size() == 1 &&
        follow::pick(flights) && follow::pick(flights)->callsign == "MOCK1");
  // The same route from hexdb.io.
  follow::Route hexdb_route;
  const std::string hexdb_answer = read("hexdb-route");
  CHECK(follow::read_hexdb(hexdb_answer.c_str(), hexdb_answer.size(), doc, follow::RouteAsk{"MOCK1", lat, lon},
                           hexdb_route, error) &&
        hexdb_route.known && hexdb_route.from == "KTST" && hexdb_route.to == "KFAR");
  // A plane's photo, as Planespotters.net answers: its thumbnail, its page and its photographer.
  photos::Photo photo;
  const std::string photo_answer = read("photo");
  CHECK(photos::read(photo_answer.c_str(), photo_answer.size(), doc, photo, error) && photo.known &&
        photo.src.rfind("https://cdn.planespotters.net/", 0) == 0 && photo.width == 200 &&
        photo.photographer == "A. Mock");
  // Its route, from adsb.lol's routeset: from the mock's airport.
  follow::Route route;
  const std::string route_answer = read("routeset");
  const follow::RouteAsk mock{"MOCK1", lat, lon};
  CHECK(follow::read_routeset(route_answer.c_str(), route_answer.size(), doc, mock, route, error) && route.known && route.from == "KTST" && route.to == "KFAR");
}

int main(int argc, char **argv) {
  test_geo();
  test_settings();
  test_offline();
  test_card();
  test_card_words();
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
  test_emblem();
  test_ticks();
  test_providers();
  test_follow();
  test_climb_text();
  test_pick();
  test_menu();
  test_keyboard();
  test_places();
  test_locate();
  test_pinch();
  test_swipe();
  test_wifi_list();
  test_weather();
  test_radar();
  test_schedule();
  test_firmware();
  test_touch();
  test_tour();
  test_pixels();
  test_pngrow();
  test_photos();
  test_sight();
  test_sightings();
  test_numbers();
  test_sound();
  test_mock_answers();
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
