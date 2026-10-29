// Range rings and a compass rose round the middle of the frame, drawn once into the map's canvas whenever the frame
// changes. On the radar they are a lit phosphor on the dark tube, with bearing lines every 30 degrees; over a map of
// tiles they are thin ink, so the map stays readable. The airspace goes under them: in the phosphor on the radar, in
// the charts' blue and magenta over a map.
#include "scope.h"
#include "colours.h"
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sky::scope {
namespace {
// Range rings in nautical miles, as every radar scope counts them: the first of these that leaves at most RINGS rings
// between the middle and the nearer edge of the frame.
constexpr double RING_STEPS[] = {0.25, 0.5, 1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500};
constexpr int RINGS = 4;

lv_point_precise_t at(double cx, double cy, double bearing, double r) {
  const double a = bearing * geo::PI / 180.0;
  return {(lv_value_precise_t) std::lround(cx + std::sin(a) * r), (lv_value_precise_t) std::lround(cy - std::cos(a) * r)};
}
void line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, uint32_t colour, int width, lv_opa_t opa) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(colour);
  d.width = width;
  d.opa = opa;
  d.p1 = a;
  d.p2 = b;
  lv_draw_line(layer, &d);
}
void circle(lv_layer_t *layer, double cx, double cy, double r, uint32_t colour, int width, lv_opa_t opa) {
  if (r < 2) return;
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.color = lv_color_hex(colour);
  d.width = width;
  d.opa = opa;
  d.center = {(int32_t) std::lround(cx), (int32_t) std::lround(cy)};
  d.radius = (uint16_t) std::lround(r);
  d.start_angle = 0;
  d.end_angle = 360;
  lv_draw_arc(layer, &d);
}
void disc(lv_layer_t *layer, double cx, double cy, double r, uint32_t colour, lv_opa_t opa) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = LV_RADIUS_CIRCLE;
  d.bg_color = lv_color_hex(colour);
  d.bg_opa = opa;
  const int x = (int) std::lround(cx - r), y = (int) std::lround(cy - r), s = (int) std::lround(2 * r);
  lv_area_t box{x, y, x + s - 1, y + s - 1};
  lv_draw_rect(layer, &d, &box);
}
// `text` with its middle (`centred`) or its left end at x, y.
void text(lv_layer_t *layer, const char *s, double x, double y, uint32_t colour, lv_opa_t opa, bool centred) {
  const lv_font_t *font = ui::fonts.small;
  if (!font) return;
  const int w = ui::text_width(s, font), h = lv_font_get_line_height(font);
  const int x0 = (int) std::lround(centred ? x - w / 2.0 : x), y0 = (int) std::lround(y - h / 2.0);
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.font = font;
  d.color = lv_color_hex(colour);
  d.opa = opa;
  d.text = s;
  d.text_local = 1;
  lv_area_t area{x0, y0, x0 + w, y0 + h - 1};
  lv_draw_label(layer, &d, &area);
}
std::string miles(double nm) {
  char out[16];
  std::snprintf(out, sizeof(out), "%g", nm);
  return out[0] == '0' && out[1] == '.' ? std::string(out + 1) : std::string(out);
}
}  // namespace

double ring_step(double px_per_nm, double near) {
  for (const double step : RING_STEPS)
    if (near / (step * px_per_nm) <= RINGS) return step;
  return RING_STEPS[sizeof(RING_STEPS) / sizeof(RING_STEPS[0]) - 1];
}

int rose_radius(const geo::View &v, double cx, double cy) {
  return (int) std::floor(std::min({cx, v.width - cx, cy, v.height - cy}) - ui::px(4));
}

void draw(lv_layer_t *layer, const geo::View &v, const Look &look) {
  const double cx = v.width / 2.0, cy = v.height / 2.0;
  const double far = std::hypot(cx, cy);
  const int rose = rose_radius(v, cx, cy);
  const colours::Tint &tint = colours::tint(look.tint);
  const bool radar = look.radar;
  const int thin = std::max(1, ui::px(1));
  // A map's rings in ink, see-through, each over a dark line so it reads on light and dark maps alike.
  const colours::Palette &pal = colours::palette(look.light);
  const uint32_t ring_colour = radar ? colours::glow_share(tint, colours::share::RING) : pal.ink;
  const uint32_t rose_colour = radar ? colours::glow_share(tint, colours::share::ROSE) : pal.ink;
  // Under each ring on a map, a wider line of the other end of the palette, so it reads on light and dark parts alike.
  const uint32_t under = look.light ? pal.card : colours::BACKGROUND;
  const lv_opa_t opa = radar ? LV_OPA_COVER : LV_OPA_60;
  if (radar) {
    // The glow of a lit tube, strongest in the middle.
    for (int n = 1; n <= 4; ++n) disc(layer, cx, cy, far * n / 4, tint.glow, 6);
    const uint32_t grid = colours::glow_share(tint, colours::share::GRID);
    for (int bearing = 0; bearing < 360; bearing += 30) line(layer, at(cx, cy, bearing, 0), at(cx, cy, bearing, far), grid, thin, LV_OPA_COVER);
  }
  const double px_per_nm = geo::NM / v.metres_per_px();
  const double step = ring_step(px_per_nm, std::max<double>(rose, ui::px(40)));
  char label[24];
  for (int n = 1; n < 64 && step * n * px_per_nm < far; ++n) {
    const double r = step * n * px_per_nm;
    if (!radar) circle(layer, cx, cy, r, under, thin + 2, LV_OPA_30);
    circle(layer, cx, cy, r, ring_colour, thin, opa);
    // The miles of each ring where it crosses the bearing 105, between the rose's figures, the first with its unit.
    if (std::fabs(r - rose) < ui::px(20)) continue;
    std::snprintf(label, sizeof(label), "%s%s", miles(step * n).c_str(), n == 1 ? " NM" : "");
    const lv_point_precise_t p = at(cx, cy, 105, r);
    if (p.x + ui::px(34) < v.width && p.y > ui::px(8) && p.y < v.height - ui::px(12))
      text(layer, label, p.x + ui::px(3), p.y + ui::px(6), ring_colour, opa, false);
  }
  if (rose >= ui::px(36)) {
    // Ticks every 10 degrees, longer every 30, inward from the circle; bearings in three figures on a large rose, the
    // four quarters alone on a small one.
    const bool large = rose >= ui::px(110);
    if (!radar) circle(layer, cx, cy, rose, under, thin + 2, LV_OPA_30);
    circle(layer, cx, cy, rose, rose_colour, thin, opa);
    for (int bearing = 0; bearing < 360; bearing += large ? 5 : 10) {
      const int length = ui::px(bearing % 30 == 0 ? 9 : bearing % 10 == 0 ? 5 : 3);
      line(layer, at(cx, cy, bearing, rose), at(cx, cy, bearing, rose - length), rose_colour, thin, opa);
      if (bearing % (large ? 30 : 90) != 0) continue;
      // North is 360, as a radar's rose writes it, not 000.
      if (large) std::snprintf(label, sizeof(label), "%03d", bearing ? bearing : 360);
      else std::snprintf(label, sizeof(label), "%c", "NESW"[bearing / 90]);
      const lv_point_precise_t p = at(cx, cy, bearing, rose - length - ui::px(10));
      text(layer, label, p.x, p.y, bearing == 0 && radar ? tint.glow : rose_colour, opa, true);
    }
  }
  const int arm = ui::px(5);
  const uint32_t cross = radar ? tint.hot : pal.ink;
  line(layer, {(lv_value_precise_t) (cx - arm), (lv_value_precise_t) cy}, {(lv_value_precise_t) (cx + arm), (lv_value_precise_t) cy}, cross, thin, LV_OPA_COVER);
  line(layer, {(lv_value_precise_t) cx, (lv_value_precise_t) (cy - arm)}, {(lv_value_precise_t) cx, (lv_value_precise_t) (cy + arm)}, cross, thin, LV_OPA_COVER);
}

airspace::Ink airspace_ink(const Look &look) {
  const colours::Tint &tint = colours::tint(look.tint);
  const colours::AirspaceInk &ink = colours::airspace_ink(look.light);
  const colours::Palette &pal = colours::palette(look.light);
  auto pick = [&](float share, uint32_t on_map) {
    return pixels::rgb565(look.radar ? colours::glow_share(tint, share) : on_map);
  };
  airspace::Ink out;
  out.b = pick(colours::share::CLASS_B, ink.b);
  out.c = pick(colours::share::CLASS_C, ink.c);
  out.d = pick(colours::share::CLASS_D, ink.d);
  out.runway = pick(colours::share::RUNWAY, pal.ink);
  out.centerline = pick(colours::share::CENTERLINE, pal.muted);
  out.thin = std::max(1.0f, ui::density);
  out.wide = std::max(2.0f, 2 * ui::density);
  out.dash = (float) ui::px(6);
  out.gap = (float) ui::px(4);
  return out;
}
}  // namespace sky::scope
