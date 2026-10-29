// The parts of Skyscope that need no screen or network, on a PC: settings, the feed's JSON, the tile plan, which planes
// a frame shows and where, and the label placement. tests/run.sh builds and runs it; a reply saved from a feed can be
// given as an argument to read it too.
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../components/skyscope/airspace.h"
#include "../components/skyscope/beam.h"
#include "../components/skyscope/feed.h"
#include "../components/skyscope/firmware.h"
#include "../components/skyscope/geo.h"
#include "../components/skyscope/menu_rows.h"
#include "../components/skyscope/overlay.h"
#include "../components/skyscope/pixels.h"
#include "../components/skyscope/plan.h"
#include "../components/skyscope/providers.h"
#include "../components/skyscope/schedule.h"
#include "../components/skyscope/settings.h"
#include "../components/skyscope/sprites.h"
#include "../components/skyscope/tiles.h"
#include "../components/skyscope/touch.h"

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
  // The radar's scope is on unless switched off, and its beam turns only round it, on the radar.
  Settings s;
  CHECK(s.scope && s.sweep && s.map == RADAR && scope_shown(s) && sweeps(s));
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
  test_scope();
  test_airspace();
  test_feed();
  test_tiles();
  test_overlay();
  test_labels();
  test_sprites();
  test_shapes();
  test_beam();
  test_providers();
  test_menu();
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
