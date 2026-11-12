// The emblem (components/skyscope/emblem.h) drawn without a screen:
//
//     make emblem             (writes docs/emblem.svg, and tests/build/emblem.png to look at)
//
// The PNG is the emblem as the screen's own code draws it, still and at a few moments of the splash's turn; the SVG is
// the same shapes from the same sizes, for the README and the web page's icon.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../components/skyscope/emblem.h"

using namespace sky;

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}

// An RGB PNG from RGB565 pixels, stored in deflate's blocks that are not compressed: no zlib needed.
static bool write_png(const char *path, int w, int h, const std::vector<uint16_t> &pixels) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const uint16_t p = pixels[(size_t) y * w + x];
      raw.push_back((uint8_t) ((p >> 11) * 255 / 31));
      raw.push_back((uint8_t) ((p >> 5 & 0x3F) * 255 / 63));
      raw.push_back((uint8_t) ((p & 0x1F) * 255 / 31));
    }
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size(); at += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size());
    z.push_back(n & 0xFF), z.push_back(n >> 8), z.push_back(~n & 0xFF), z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
  }
  const uint32_t adler = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);
  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  auto be = [&](uint32_t v) {
    const uint8_t q[4] = {(uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v};
    std::fwrite(q, 1, 4, f);
  };
  auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
    be((uint32_t) data.size());
    std::vector<uint8_t> all(type, type + 4);
    all.insert(all.end(), data.begin(), data.end());
    std::fwrite(all.data(), 1, all.size(), f);
    be(crc(all.data(), all.size()) ^ 0xFFFFFFFFu);
  };
  std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  chunk("IHDR", {(uint8_t) (w >> 24), (uint8_t) (w >> 16), (uint8_t) (w >> 8), (uint8_t) w, (uint8_t) (h >> 24),
                 (uint8_t) (h >> 16), (uint8_t) (h >> 8), (uint8_t) h, 8, 2, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  return std::fclose(f) == 0;
}

// The same shapes as an SVG, 200 units across, the scope's radius 100 about (0, 0).
// `compact` for the web page, which carries it in the firmware: fewer slices and points, a decimal less.
static std::string svg(const emblem::Colors &c, bool compact) {
  using namespace emblem;
  char line[600];
  std::string out;
  auto add = [&](const char *format, auto... values) {
    std::snprintf(line, sizeof(line), format, values...);
    out += line;
  };
  auto hex = [](uint32_t color) {
    static char text[4][8];
    static int next = 0;
    char *t = text[next++ % 4];
    std::snprintf(t, 8, "#%06X", (unsigned) color);
    return t;
  };
  auto at = [](float bearing, float r, float &x, float &y) {
    const float a = bearing * PI / 180;
    x = std::sin(a) * r * 100;
    y = -std::cos(a) * r * 100;
  };
  out += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-104 -104 208 208\" role=\"img\" "
         "aria-label=\"Skyscope\">\n";
  // The blade's light, across it from its trailing edge to its leading one; the hub's, from the upper left; the haze.
  add("<defs><linearGradient id=\"lit\" gradientUnits=\"userSpaceOnUse\" x1=\"0\" y1=\"%.1f\" x2=\"0\" y2=\"%.1f\">"
      "<stop offset=\"0\" stop-color=\"%s\"/><stop offset=\"1\" stop-color=\"%s\"/></linearGradient>",
      -BLADE_TRAIL * BLADE_WIDE * 100, (BLADE_SWEEP + BLADE_LEAD * BLADE_WIDE) * 100, hex(c.shade), hex(c.blade));
  add("<radialGradient id=\"dome\" cx=\"0.34\" cy=\"0.3\" r=\"0.75\"><stop offset=\"0\" stop-color=\"%s\"/>"
      "<stop offset=\"1\" stop-color=\"%s\"/></radialGradient>",
      hex(c.blade), hex(c.shade));
  add("<radialGradient id=\"haze\"><stop offset=\"0\" stop-color=\"%s\" stop-opacity=\"%.2f\"/>"
      "<stop offset=\"1\" stop-color=\"%s\" stop-opacity=\"0\"/></radialGradient></defs>\n",
      hex(c.scope), HAZE, hex(c.scope));
  add("<circle r=\"104\" fill=\"%s\"/>\n", hex(c.ground));
  add("<circle r=\"%.1f\" fill=\"url(#haze)\"/>\n", HAZE_TO * 100);
  add("<circle r=\"%.2f\" fill=\"none\" stroke=\"%s\" stroke-width=\"%.1f\" stroke-opacity=\"0.13\"/>\n", RIM * 100,
      hex(c.scope), RIM_BLOOM * 150);
  // The afterglow: slices behind the beam, each a little fainter.
  const int slices = compact ? 9 : 17;
  for (int i = 0; i < slices; ++i) {
    const float from = REST_DEGREES - (i + 1) * GLOW_DEGREES / slices, to = REST_DEGREES - i * GLOW_DEGREES / slices;
    float ax, ay, bx, by, cx, cy, dx, dy;
    at(from, GLOW_FROM, ax, ay);
    at(from, GLOW_TO, bx, by);
    at(to, GLOW_TO, cx, cy);
    at(to, GLOW_FROM, dx, dy);
    add("<path d=\"M%.2f %.2fL%.2f %.2fA%.2f %.2f 0 0 1 %.2f %.2fL%.2f %.2fA%.2f %.2f 0 0 0 %.2f %.2fZ\" "
        "fill=\"%s\" fill-opacity=\"%.3f\"/>\n",
        ax, ay, bx, by, GLOW_TO * 100, GLOW_TO * 100, cx, cy, dx, dy, GLOW_FROM * 100, GLOW_FROM * 100, ax, ay,
        hex(c.scope), glow_at((i + 0.5f) * GLOW_DEGREES / slices));
  }
  add("<g fill=\"none\" stroke=\"%s\">\n", hex(c.scope));
  add("<circle r=\"%.2f\" stroke-width=\"%.2f\"/>\n", RIM * 100, RIM_STROKE * 100);
  for (const float ring : RINGS)
    add("<circle r=\"%.2f\" stroke-width=\"%.2f\" stroke-opacity=\"%.2f\"/>\n", ring * 100, RING_STROKE * 100,
        RING_LIGHT);
  for (int tick = 0; tick < 360; tick += compact ? 10 : 5) {
    const float length = tick % 30 == 0 ? TICK_30 : tick % 10 == 0 ? TICK_10 : TICK;
    float ax, ay, bx, by;
    at(tick, RIM, ax, ay);
    at(tick, RIM - length, bx, by);
    add("<path d=\"M%.2f %.2fL%.2f %.2f\" stroke-width=\"%.2f\" stroke-opacity=\"%.2f\"/>\n", ax, ay, bx, by,
        TICK_STROKE * 100, tick % 30 == 0 ? 1.0 : tick % 10 == 0 ? 0.72 : 0.45);
  }
  out += "</g>\n";
  // The plane the beam has just passed, and the others as faint as the beam left them.
  for (const Blip &b : BLIPS) {
    float behind = std::fmod(REST_DEGREES - b.bearing + 360, 360.0f);
    float x, y;
    at(b.bearing, b.distance, x, y);
    const float lit = blip_light(behind);
    if (lit < 0.02f) continue;
    add("<circle cx=\"%.2f\" cy=\"%.2f\" r=\"%.2f\" fill=\"%s\" fill-opacity=\"%.2f\"/>\n", x, y,
        b.size * BLIP_HALO * 70, hex(c.blip), 0.14 * lit);
    add("<g transform=\"translate(%.2f %.2f) rotate(%.0f) scale(%.2f)\" stroke=\"%s\" stroke-opacity=\"%.2f\" "
        "stroke-linecap=\"round\" fill=\"none\">",
        x, y, b.heading, b.size * 100, hex(c.blip), lit);
    for (const Limb &l : PLANE)
      for (int side = 0; side < (l.both ? 2 : 1); ++side)
        add("<path d=\"M%.2f %.2fL%.2f %.2f\" stroke-width=\"%.2f\"/>", side ? -l.x : l.x, l.y, side ? -l.x2 : l.x2,
            l.y2, l.r * 2);
    out += "</g>\n";
  }
  // The beam, on from the leading blade's tip.
  float ax, ay, bx, by;
  at(REST_DEGREES, BLADE_TIP - 0.03f, ax, ay);
  at(REST_DEGREES, GLOW_TO, bx, by);
  add("<path d=\"M%.2f %.2fL%.2f %.2f\" stroke=\"%s\" stroke-width=\"%.2f\"/>\n", ax, ay, bx, by, hex(c.blade),
      BEAM_STROKE * 100);
  // The blades: one outline, out along the leading edge and back along the trailing one, drawn three times around.
  std::string outline, band;
  const int points = compact ? 14 : 40;
  auto point = [&](std::string &to, float t, bool lead) {
    std::snprintf(line, sizeof(line), compact ? "%c%.1f %.1f" : "%c%.2f %.2f", to.empty() ? 'M' : 'L',
                  (BLADE_ROOT + BLADE_LONG * t) * 100, (lead ? blade_lead(t) : blade_trail(t)) * 100);
    to += line;
  };
  // More points toward the tip, where it curves most.
  auto along = [&](int i) { return 1 - (1 - (float) i / points) * (1 - (float) i / points); };
  for (int i = 0; i <= points; ++i) point(outline, along(i), true);
  for (int i = points - 1; i >= 0; --i) point(outline, along(i), false);
  for (int i = 0; i <= 4; ++i) point(band, BAND_FROM + (BAND_TO - BAND_FROM) * i / 4, true);
  for (int i = 4; i >= 0; --i) point(band, BAND_FROM + (BAND_TO - BAND_FROM) * i / 4, false);
  // The blade lies along +x, which is bearing 90.
  out += "<g id=\"blade\" transform=\"rotate(" + std::to_string((int) REST_DEGREES - 90) + ")\">";
  out += "<path fill=\"url(#lit)\" d=\"" + outline + "Z\"/>";
  add("<path fill=\"%s\" fill-opacity=\"0.84\" d=\"%sZ\"/></g>\n", hex(c.blip), band.c_str());
  for (int blade = 1; blade < BLADES; ++blade)
    add("<use href=\"#blade\" transform=\"rotate(%d)\"/>\n", blade * 360 / BLADES);
  add("<circle r=\"%.2f\" fill=\"url(#dome)\"/>\n", HUB * 100);
  add("<circle r=\"%.2f\" fill=\"none\" stroke=\"%s\" stroke-opacity=\"0.5\" stroke-width=\"%.2f\"/>\n",
      HUB * HUB_CAP * 100, hex(c.shade), HUB * HUB_LINE * 100);
  out += "</svg>\n";
  return out;
}

int main(int argc, char **argv) {
  const char *png = argc > 1 ? argv[1] : "emblem.png";
  const char *svg_path = argc > 2 ? argv[2] : nullptr;
  // Still, large; then four moments of the splash's turn, as a small glass shows them.
  const int large = 720, small = 360;
  const int w = large + 2 * small, h = large;
  std::vector<uint16_t> sheet((size_t) w * h, 0);
  emblem::Emblem e;
  e.prepare(large, large / 2.0f - 2);
  e.draw(sheet.data(), w, emblem::REST_DEGREES, emblem::TURN_DEGREES);
  e.prepare(small, small / 2.0f - 2);
  const float moments[] = {0.2f, 0.8f, 1.5f, 2.2f};
  for (int i = 0; i < 4; ++i)
    e.draw(sheet.data() + (size_t) (i / 2) * small * w + large + (i % 2) * small, w, emblem::bearing_at(moments[i]),
           emblem::turned_at(moments[i]));
  if (!write_png(png, w, h, sheet)) return 1;
  std::printf("%s: %d x %d\n", png, w, h);
  if (svg_path) {
    FILE *f = std::fopen(svg_path, "w");
    if (!f) return 1;
    const std::string text = svg({}, false);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::printf("%s: %zu bytes\n", svg_path, text.size());
  }
  // The page's: the same on one line, for components/skyscope/page.h.
  if (argc > 3) {
    FILE *f = std::fopen(argv[3], "w");
    if (!f) return 1;
    std::string text = svg({}, true);
    for (char &ch : text)
      if (ch == '\n') ch = ' ';
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    std::printf("%s: %zu bytes\n", argv[3], text.size());
  }
  return 0;
}
