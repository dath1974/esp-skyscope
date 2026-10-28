// Planes over the map on the glass: plan.h's planes painted in one object over the map, in its draw event, so thirty
// planes with their tracks and labels cost one object and one redraw.
#include "view.h"
#include "colours.h"
#include "sprites.h"
#include "ui.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
// lvgl.h leaves out the image cache, which a label image is dropped from when it goes.
#include "src/misc/cache/instance/lv_image_cache.h"

#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sky::view {
namespace ap = plan;
namespace cl = colours;

struct View {
  lv_obj_t *layer = nullptr;
  uint32_t frame = 0, painted_frame = 0;  // the map under the planes, counted: a new one paints a sweeping scope anew
  ap::Overlay overlay;
  float sx = 1, sy = 1;  // the layer on the glass against the frame the planes were placed on
  bool hidden = false;
  std::vector<int> spots;  // where each label stood the last time, kept while it stays clear (aircraft_plan.h)
  std::map<std::string, int> spot_of;  // the same by a plane's id, where it has one: a plane come or gone moves no other
  std::string picked;      // the plane a finger picked, ringed
  lv_timer_t *sweeping = nullptr;  // a radar scope's beam turning, every ap::SWEEP_FRAME_MS
  // The bearing the beam is drawn at, which only the sweep moves: every area drawn in between (a label, a plane) draws
  // the beam where the rest of the glass shows it, so no piece of an older beam is left behind.
  float beam_drawn = -1;
  BeamPainter painter;  // paints the beam into the map under the layer
  // A sweeping scope paints each plane as the beam last passed it, as a radar does (`painted`: the report's style, its
  // planes as they were then, standing still): a plane moves, takes its new label or goes only when the beam crosses it,
  // so whatever else draws a piece of the glass again draws the same pixels there. `drawn`: the box each plane was last
  // drawn in whole (marker, vector, track, label and leader), which is drawn again whole when the beam moves it.
  ap::Overlay painted;
  bool latched = false;
  std::map<std::string, ap::Box> drawn;
  std::vector<ap::Box> again;  // planes whose labels moved in a draw, to draw again whole on the next frame
  // The labels as laid out for one frame of the glass (ui::frame): LVGL draws a frame in several areas, and each asks
  // for the planes again; the layout, which measures every label and keeps them apart, is worked out once.
  uint32_t layout_frame = UINT32_MAX;
  std::vector<ap::Label> labels;
  std::vector<ap::Placed> placed;
  std::vector<int> kept;
  std::vector<ap::Point> laid_at;  // where each plane was when laid out, on the glass
  std::vector<ap::Box> boxes;      // what each plane took then, marker, track and label: what a glide draws again
  // Each label drawn once into an image (render_labels), by label_key: its text, its style and its plane.
  struct LabelImage {
    uint8_t *data = nullptr;
    size_t bytes = 0;
    lv_image_dsc_t dsc{};
  };
  std::map<std::string, LabelImage> label_images;
};

// What the layer shows: a sweeping scope's planes as painted, else the report's.
static bool latches(const ap::Overlay &o) { return o.radar.on && o.radar.sweep; }
static const ap::Overlay &shown(const View *view) { return latches(view->overlay) ? view->painted : view->overlay; }

// ---- A radar scope: one phosphor (colours.h TINTS) from its ground to its hottest light.
static const cl::Tint &tint_of(const ap::Overlay &o) { return cl::tint(o.radar.tint); }
// `light` 0 is the ground, 1 the glow, 2 the hottest light (ap::BAND_LIGHT).
lv_color_t ground(const ap::Overlay &o) { return lv_color_hex(tint_of(o).ground); }
lv_color_t phosphor(const ap::Overlay &o, float light) {
  const auto &t = tint_of(o);
  light = std::max(0.0f, std::min(2.0f, light));
  if (light <= 1) return lv_color_hex(cl::mix(t.glow, t.ground, (uint8_t) std::lround(light * 255)));
  return lv_color_hex(cl::mix(t.hot, t.glow, (uint8_t) std::lround((light - 1) * 255)));
}

// A plane's colour: an emergency's red, a watched plane's own, else its band of altitude in the flight map's rainbow, or
// the accent.
static lv_color_t colour_of(const ap::Overlay &o, const ap::Plane &p) {
  if (p.watch > 0 && p.watch <= ap::WATCH_COLOURS) return lv_color_hex(cl::WATCH[p.watch - 1]);
  // On a scope: its phosphor, dim low down and bright up high, or the glow alone.
  if (o.radar.on) return phosphor(o, o.by_altitude ? ap::band_light(p.band) : 1.0f);
  if (!o.by_altitude) return lv_color_hex(cl::ACCENT);
  return lv_color_hex(cl::BANDS[p.band >= 0 && p.band < ap::BANDS ? p.band : 0]);
}

// The sizes of the marks, in the screen's own pixels (ui::px). A scope's blips and plane symbols are half as large: a
// radar's returns are small points on the tube.
struct Sizes { int dot, plane, tick, line, halo, pad, gap, radius; };
static Sizes sizes(const ap::Overlay &o) {
  const bool full = true;
  const int part = o.radar.on ? 2 : 1;
  return {std::max(3, ui::px(full ? 12 : 8) / part), std::max(6, ui::px(full ? 24 : 16) / part), ui::px(full ? 9 : 6),
          std::max(1, ui::px(2)), std::max(1, ui::px(2)), ui::px(3), ui::px(4), ui::px(4)};
}

// LVGL's precise points are whole numbers or floats by its configuration.
static lv_point_precise_t precise(ap::Point p) {
  return {static_cast<lv_value_precise_t>(std::lround(p.x)), static_cast<lv_value_precise_t>(std::lround(p.y))};
}
// Rounded ends only where a line ends: segments of one track that overlap there would show darker dots at their joints.
static void line(lv_layer_t *layer, ap::Point a, ap::Point b, lv_color_t colour, int width, lv_opa_t opa, bool round_start = true,
                 bool round_end = true) {
  lv_draw_line_dsc_t dsc;
  lv_draw_line_dsc_init(&dsc);
  dsc.color = colour; dsc.width = width; dsc.opa = opa; dsc.round_start = round_start; dsc.round_end = round_end;
  dsc.p1 = precise(a); dsc.p2 = precise(b);
  lv_draw_line(layer, &dsc);
}
// A circle's outline alone: the ring around a watched plane.
static void ring(lv_layer_t *layer, ap::Point c, int d, int width, lv_color_t colour) {
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = LV_RADIUS_CIRCLE; dsc.bg_opa = LV_OPA_TRANSP;
  dsc.border_width = width; dsc.border_color = colour; dsc.border_opa = LV_OPA_COVER;
  const int x = (int) std::lround(c.x) - d / 2, y = (int) std::lround(c.y) - d / 2;
  lv_area_t box{x, y, x + d - 1, y + d - 1};
  lv_draw_rect(layer, &dsc, &box);
}
static void dot(lv_layer_t *layer, ap::Point c, int d, lv_color_t fill, int border, lv_color_t edge, lv_opa_t opa = LV_OPA_COVER) {
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = LV_RADIUS_CIRCLE; dsc.bg_color = fill; dsc.bg_opa = opa;
  dsc.border_width = border; dsc.border_color = edge; dsc.border_opa = border ? LV_OPA_COVER : LV_OPA_TRANSP;
  const int x = (int) std::lround(c.x) - d / 2, y = (int) std::lround(c.y) - d / 2;
  lv_area_t box{x, y, x + d - 1, y + d - 1};
  lv_draw_rect(layer, &dsc, &box);
}
// A plane symbol, stamped from its mask (sprites.h) in `colour`: the mask in PSRAM, drawn by LVGL as an A8 image, one
// blend over its pixels.
struct Image {
  uint8_t *data = nullptr;
  lv_image_dsc_t dsc{};
};
static sprites::Cache<Image> symbols;
static void shape(lv_layer_t *layer, ap::Point c, float size, int heading, lv_color_t colour, lv_opa_t opa) {
  // At most two masks made a frame (sprites.h): a plane that turned onto a new step takes the nearest one meanwhile.
  auto *found = symbols.get((int) std::lround(size), heading, ui::frame, 2);
  if (!found) return;
  auto &entry = *found;
  Image &image = entry.extra;
  if (!image.data) {
    const sprites::Mask &m = entry.mask;
    image.data = static_cast<uint8_t *>(heap_caps_malloc(m.alpha.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!image.data) return;
    std::memcpy(image.data, m.alpha.data(), m.alpha.size());
    image.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.dsc.header.cf = LV_COLOR_FORMAT_A8;
    image.dsc.header.w = m.size;
    image.dsc.header.h = m.size;
    image.dsc.header.stride = m.size;
    image.dsc.data_size = m.alpha.size();
    image.dsc.data = image.data;
    // Its pixels live in PSRAM now, not in the internal heap.
    entry.mask.alpha = {};
  }
  const int n = image.dsc.header.w;
  const int x = (int) std::lround(c.x - n / 2.0f), y = (int) std::lround(c.y - n / 2.0f);
  lv_draw_image_dsc_t d;
  lv_draw_image_dsc_init(&d);
  d.src = &image.dsc;
  d.recolor = colour;
  d.recolor_opa = LV_OPA_COVER;
  d.opa = opa;
  lv_area_t area{x, y, x + n - 1, y + n - 1};
  lv_draw_image(layer, &d, &area);
}

// The middle of a scope on the glass, and how far its beam reaches: to its rose, else the farthest corner.
static ap::Point scope_middle(const View *view, const lv_area_t &a) {
  return {a.x1 + view->overlay.radar.x * view->sx, a.y1 + view->overlay.radar.y * view->sy};
}
static float scope_reach(const View *view, ap::Point c, const lv_area_t &a) {
  if (view->overlay.radar.rose > 0) return view->overlay.radar.rose * std::min(view->sx, view->sy) + 2;
  float far = 0;
  for (const float x : {(float) a.x1, (float) a.x2}) for (const float y : {(float) a.y1, (float) a.y2})
    far = std::max(far, std::hypot(x - c.x, y - c.y));
  return far + 2;
}
// The box a plane's marker, its glow and rings, its vector and its track take on the glass, at `c`; its label beside.
static ap::Box plane_box(const View *view, const ap::Plane &p, ap::Point c) {
  const ap::Overlay &o = view->overlay;
  const Sizes s = sizes(o);
  const int r = (int) std::lround((o.marker == ap::PLANE ? s.plane : s.dot) * ap::WATCH_SCALE) + 8 * s.halo + 4 * s.line;
  ap::Box box{(int) std::floor(c.x) - r, (int) std::floor(c.y) - r, 2 * r + 2, 2 * r + 2};
  auto take = [&](float x, float y, int pad) { box = ap::join(box, {(int) std::floor(x) - pad, (int) std::floor(y) - pad, 2 * pad + 2, 2 * pad + 2}); };
  if (p.vx || p.vy) take(c.x + p.vx * view->sx, c.y + p.vy * view->sy, s.line + 1);
  lv_area_t a;
  if (view->layer) lv_obj_get_coords(view->layer, &a); else a = {0, 0, 0, 0};
  for (size_t n = 0; n + 1 < p.track.size(); n += 2) take(a.x1 + p.track[n] * view->sx, a.y1 + p.track[n + 1] * view->sy, s.line + 2);
  return box;
}
// How lit a plane is by the beam, in steps, so a plane is drawn again only when it changes a step.
static constexpr int GLOW_STEPS = 6;
static int glow_step(const View *view, ap::Point c, ap::Point plane, float beam) {
  if (!latches(view->overlay)) return 0;
  return (int) std::lround(ap::afterglow(ap::behind(beam, ap::bearing(c, plane))) * GLOW_STEPS);
}

// ---- Labels, each drawn once into an image and stamped: drawn as text, every label the glass drew again was its
// glyphs unpacked from the font and blended one by one, a third of a radar frame. A label is drawn again only when its
// text or style changes, and never while LVGL draws the glass (show and the sweep call render_labels).
static std::string style_of(const ap::Overlay &o, const ap::Plane &p) {
  const bool radar = o.radar.on;
  const lv_color_t first = radar ? colour_of(o, p) : lv_color_hex(cl::INK);
  const lv_color_t rest = radar ? phosphor(o, 0.65f) : lv_color_hex(cl::MUTED);
  const lv_color_t ground = radar ? lv_color_hex(tint_of(o).ground) : lv_color_hex(cl::CARD);
  char out[48];
  std::snprintf(out, sizeof(out), "%06x%06x%06x%c%c", (unsigned) lv_color_to_u32(first) & 0xFFFFFF,
                (unsigned) lv_color_to_u32(rest) & 0xFFFFFF, (unsigned) lv_color_to_u32(ground) & 0xFFFFFF,
                p.watch ? 'w' : '-', radar ? 'r' : '-');
  return out;
}
static std::string label_key(const ap::Overlay &o, const ap::Plane &p, size_t i) {
  return (p.id.empty() ? "#" + std::to_string(i) : p.id) + '\x1f' + p.label + '\x1f' + style_of(o, p);
}
static void render_labels(View *view) {
  const lv_font_t *font = ui::fonts.label ? ui::fonts.label : ui::fonts.small;
  if (!font || !view->layer) return;
  static lv_obj_t *canvas = nullptr;
  if (!canvas) {
    canvas = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  }
  const ap::Overlay &style = view->overlay;
  const Sizes s = sizes(style);
  const bool radar = style.radar.on;
  const int line_h = lv_font_get_line_height(font);
  std::set<std::string> live;
  auto render = [&](const ap::Plane &p, size_t i) {
    const std::vector<std::string> rows = ap::lines(p.label);
    if (rows.empty()) return;
    const std::string key = label_key(style, p, i);
    live.insert(key);
    View::LabelImage &image = view->label_images[key];
    if (image.data) return;
    int w = 0;
    for (const auto &row : rows) w = std::max(w, ui::text_width(row, font));
    w += 2 * s.pad;
    const int h = (int) rows.size() * line_h + s.pad;
    image.bytes = (size_t) w * h * 4;
    image.data = static_cast<uint8_t *>(heap_caps_malloc(image.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!image.data) return;
    lv_canvas_set_buffer(canvas, image.data, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    // On a scope a data block: square, the tube showing through, its text in the plane's own phosphor.
    bg.radius = radar ? 0 : s.radius;
    bg.bg_color = radar ? lv_color_hex(tint_of(style).ground) : lv_color_hex(cl::CARD);
    bg.bg_opa = radar ? LV_OPA_60 : LV_OPA_80;
    // A watched plane's label is edged in its colour.
    if (p.watch) { bg.border_width = s.line; bg.border_color = colour_of(style, p); bg.border_opa = LV_OPA_COVER; }
    lv_area_t all{0, 0, w - 1, h - 1};
    lv_draw_rect(&layer, &bg, &all);
    for (size_t n = 0; n < rows.size(); ++n) {
      lv_draw_label_dsc_t dsc;
      lv_draw_label_dsc_init(&dsc);
      dsc.font = font;
      dsc.text = rows[n].c_str();
      dsc.color = radar ? (n ? phosphor(style, 0.65f) : colour_of(style, p)) : lv_color_hex(n ? cl::MUTED : cl::INK);
      dsc.text_local = 1;
      const int y = s.pad / 2 + (int) n * line_h;
      lv_area_t text{s.pad, y, w - 1 - s.pad, y + line_h - 1};
      lv_draw_label(&layer, &dsc, &text);
    }
    lv_canvas_finish_layer(canvas, &layer);
    image.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image.dsc.header.w = w;
    image.dsc.header.h = h;
    image.dsc.header.stride = w * 4;
    image.dsc.data_size = image.bytes;
    image.dsc.data = image.data;
  };
  for (size_t i = 0; i < view->overlay.planes.size(); ++i) render(view->overlay.planes[i], i);
  if (latches(view->overlay))
    for (size_t i = 0; i < view->painted.planes.size(); ++i) render(view->painted.planes[i], i);
  for (auto it = view->label_images.begin(); it != view->label_images.end();) {
    if (live.count(it->first)) { ++it; continue; }
    if (it->second.data) {
      lv_image_cache_drop(&it->second.dsc);
      heap_caps_free(it->second.data);
    }
    it = view->label_images.erase(it);
  }
}

static void draw(lv_event_t *e) {
  auto *view = static_cast<View *>(lv_event_get_user_data(e));
  if (!view || view->hidden) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const ap::Overlay &o = shown(view);
  const Sizes s = sizes(o);
  const uint32_t now = lv_tick_get();
  const bool radar = o.radar.on;
  // On a scope the halo is its ground, so every mark reads as lit phosphor on the dark tube.
  const lv_color_t halo = radar ? lv_color_hex(tint_of(o).ground) : lv_color_hex(cl::CARD);
  const ap::Point middle = radar ? scope_middle(view, a) : ap::Point{0, 0};
  const float beam = radar && o.radar.sweep ? (view->beam_drawn >= 0 ? view->beam_drawn : ap::beam(now)) : 0;
  // The beam itself is in the map's canvas (beam.h), painted by the sweep.
  if (radar && o.radar.sweep && view->beam_drawn < 0) view->beam_drawn = beam;
  auto on_glass = [&](float x, float y) { return ap::Point{a.x1 + x * view->sx, a.y1 + y * view->sy}; };
  std::vector<ap::Point> places;
  places.reserve(o.planes.size());
  for (const auto &p : o.planes) {
    const ap::Point at = ap::at(o, p, now, view->sx, view->sy);
    places.push_back({a.x1 + at.x, a.y1 + at.y});
  }
  // Only the planes that reach the part of the glass being drawn: a sweep's frame is a thin wedge of it, and a plane
  // outside it would cost its draw tasks (two dozen triangles for a plane marker) for nothing.
  const lv_area_t &clip = layer->_clip_area;
  const ap::Box clip_box{clip.x1, clip.y1, clip.x2 - clip.x1 + 1, clip.y2 - clip.y1 + 1};
  std::vector<uint8_t> reaches(o.planes.size());
  for (size_t i = 0; i < o.planes.size(); ++i) reaches[i] = ap::overlaps(plane_box(view, o.planes[i], places[i]), clip_box);
  ++ui::draw_calls;
  int64_t mark = esp_timer_get_time();
  auto lap = [&](ui::Part part) {
    const int64_t now_us = esp_timer_get_time();
    ui::part_us[part] += now_us - mark;
    mark = now_us;
  };
  // The tracks first, under every marker: from the oldest place to where the plane is now. On a scope its history,
  // a dot at each place it was, the older ones fainter, as a radar keeps a plane's last returns.
  for (size_t i = 0; i < o.planes.size(); ++i) {
    const auto &track = o.planes[i].track;
    if (track.size() < 4 || !reaches[i]) continue;
    const lv_color_t colour = colour_of(o, o.planes[i]);
    const size_t points = track.size() / 2;
    if (radar) {
      // Small squares: at a few pixels across they look the same as circles, and cost no circle's mask.
      const int d = std::max(2, s.line + 1);
      for (size_t n = 0; n + 1 < points; ++n) {
        const ap::Point p = on_glass(track[2 * n], track[2 * n + 1]);
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = colour;
        dsc.bg_opa = (lv_opa_t) (60 + 140 * (n + 1) / points);
        const int x = (int) std::lround(p.x) - d / 2, y = (int) std::lround(p.y) - d / 2;
        lv_area_t box{x, y, x + d - 1, y + d - 1};
        lv_draw_rect(layer, &dsc, &box);
      }
      continue;
    }
    for (size_t n = 0; n + 1 < points; ++n) {
      const ap::Point from = on_glass(track[2 * n], track[2 * n + 1]);
      const ap::Point to = n + 2 == points ? places[i] : on_glass(track[2 * n + 2], track[2 * n + 3]);
      line(layer, from, to, colour, s.line, LV_OPA_70, n == 0, n + 2 == points);
    }
  }
  lap(ui::TRACKS);
  // The watched planes come last, so they lie over the others.
  for (size_t i = 0; i < o.planes.size(); ++i) {
    if (!reaches[i]) continue;
    const auto &p = o.planes[i];
    lv_color_t colour = colour_of(o, p);
    const ap::Point c = places[i];
    const float grow = p.watch ? ap::WATCH_SCALE : 1.0f;
    const int d = (int) std::lround(s.dot * grow), size = (int) std::lround(s.plane * grow);
    if (radar && o.radar.sweep && !p.watch) {
      // A return: at full light as the beam passes, dimming over the turn to under half of its own until the next pass.
      const float lit = (float) glow_step(view, middle, c, beam) / GLOW_STEPS;
      const float own = ap::band_light(o.by_altitude ? p.band : 2), dim = own * 0.45f;
      colour = phosphor(o, dim + (1.9f - dim) * lit);
    }
    if (!view->picked.empty() && p.id == view->picked) {
      // The plane picked: a wide ring in the ink round a halo, outside a watched plane's own.
      const int across = (o.marker == ap::PLANE && p.heading != ap::NO_HEADING ? size : d) + 8 * s.halo + 4 * s.line;
      ring(layer, c, across + 2 * s.halo, 2 * s.line + 2 * s.halo, halo);
      ring(layer, c, across, 2 * s.line, radar ? phosphor(o, 1.0f) : lv_color_hex(cl::INK));
    }
    if (p.watch) {
      // A ring in its colour round a halo of the card's, a little beyond the marker.
      const int across = (o.marker == ap::PLANE && p.heading != ap::NO_HEADING ? size : d) + 4 * s.halo + 2 * s.line;
      ring(layer, c, across + 2 * s.halo, s.line + 2 * s.halo, halo);
      ring(layer, c, across, s.line, colour);
    }
    if (o.marker == ap::PLANE && p.heading != ap::NO_HEADING) {
      // A halo in the card's colour, so the plane reads on streets and on water alike.
      shape(layer, c, size + 2 * s.halo, p.heading, halo, LV_OPA_80);
      shape(layer, c, size, p.heading, colour, LV_OPA_COVER);
      continue;
    }
    if (radar && o.marker == ap::ARROW && (p.vx || p.vy)) {
      // A scope's vector: where the plane will be in a minute, as a controller's scope draws it.
      const ap::Point ahead{c.x + p.vx * view->sx, c.y + p.vy * view->sy};
      line(layer, c, ahead, colour, s.line, LV_OPA_80);
    } else if (o.marker == ap::ARROW && p.heading != ap::NO_HEADING) {
      const auto tick = ap::heading_tick(c, d / 2.0f, s.tick * grow, p.heading);
      line(layer, tick[0], tick[1], halo, s.line + 2 * s.halo, LV_OPA_80);
      line(layer, tick[0], tick[1], colour, s.line, LV_OPA_COVER);
    }
    dot(layer, c, d, colour, radar ? 0 : s.halo, halo);
  }
  lap(ui::MARKERS);
  // The labels last, over everything: a soft pill of the card's colour with the plane's lines in ink, kept apart from each
  // other and from the markers, with a leader line to a label moved off its plane (aircraft_plan.h place_labels).
  const lv_font_t *font = ui::fonts.label ? ui::fonts.label : ui::fonts.small;
  if (!font) return;
  const int line_h = lv_font_get_line_height(font);
  const int frame_w = lv_area_get_width(&a), frame_h = lv_area_get_height(&a);
  if (view->layout_frame != ui::frame || view->placed.size() != o.planes.size()) {
    view->layout_frame = ui::frame;
    auto &labels = view->labels;
    labels.assign(o.planes.size(), {});
    for (size_t i = 0; i < o.planes.size(); ++i) {
      const bool watched = o.planes[i].watch != 0;
      ap::Label &l = labels[i];
      l.c = {places[i].x - a.x1, places[i].y - a.y1};
      l.reach = (int) std::lround((o.marker == ap::PLANE ? s.plane : s.dot) * (watched ? ap::WATCH_SCALE : 1.0f) / 2) +
                (watched ? 2 * s.halo + s.line : 0);
      l.watched = watched;
      const auto image = view->label_images.find(label_key(o, o.planes[i], i));
      if (image == view->label_images.end() || !image->second.data) continue;
      l.w = image->second.dsc.header.w;
      l.h = image->second.dsc.header.h;
    }
    auto &kept = view->kept;
    kept.assign(o.planes.size(), ap::NO_SPOT);
    for (size_t i = 0; i < o.planes.size(); ++i) {
      const auto &id = o.planes[i].id;
      const auto found = id.empty() ? view->spot_of.end() : view->spot_of.find(id);
      kept[i] = found != view->spot_of.end() ? found->second : id.empty() && i < view->spots.size() ? view->spots[i] : ap::NO_SPOT;
    }
    const std::vector<ap::Box> before = view->placed.size() == o.planes.size() ? view->boxes : std::vector<ap::Box>();
    view->placed = ap::place_labels(labels, frame_w, frame_h, s.gap, line_h, kept);
    view->spots.assign(view->placed.size(), ap::NO_SPOT);
    view->laid_at = places;
    view->boxes.assign(o.planes.size(), {0, 0, 0, 0});
    for (size_t i = 0; i < view->placed.size(); ++i) {
      const ap::Placed &pl = view->placed[i];
      view->spots[i] = pl.spot;
      if (!o.planes[i].id.empty()) view->spot_of[o.planes[i].id] = pl.spot;
      ap::Box box = plane_box(view, o.planes[i], places[i]);
      if (pl.shown) box = ap::join(box, {a.x1 + pl.box.x - 1, a.y1 + pl.box.y - 1, pl.box.w + 2, pl.box.h + 2});
      view->boxes[i] = box;
      // A label that took another spot is drawn again whole on the next frame, where it was and where it is.
      if (pl.spot != kept[i] && kept[i] != ap::NO_SPOT)
        view->again.push_back(i < before.size() && before[i].w > 0 ? ap::join(before[i], box) : box);
    }
  }
  const auto &labels = view->labels;
  const auto &placed = view->placed;
  lap(ui::LAYOUT);
  // Where each plane of a sweeping scope stands whole, kept from a draw that held all of it: what the glass shows.
  if (latches(view->overlay)) {
    for (size_t i = 0; i < o.planes.size(); ++i) {
      if (o.planes[i].id.empty()) continue;
      auto &was = view->drawn[o.planes[i].id];
      if (ap::holds(clip_box, view->boxes[i])) was = view->boxes[i];
    }
  }
  // The leaders under every label: a halo of the card's colour, then the plane's own.
  for (size_t i = 0; i < placed.size(); ++i) {
    if (!placed[i].shown || !placed[i].leader || !ap::overlaps(view->boxes[i], clip_box)) continue;
    const auto ends = ap::leader(labels[i], placed[i].box);
    const ap::Point from{a.x1 + ends[0].x, a.y1 + ends[0].y}, to{a.x1 + ends[1].x, a.y1 + ends[1].y};
    line(layer, from, to, halo, s.line + 2 * s.halo, LV_OPA_80);
    line(layer, from, to, colour_of(o, o.planes[i]), std::max(1, s.line / 2), LV_OPA_COVER);
  }
  for (size_t i = 0; i < placed.size(); ++i) {
    if (!placed[i].shown) continue;
    const ap::Box &box = placed[i].box;
    if (!ap::overlaps({a.x1 + box.x, a.y1 + box.y, box.w, box.h}, clip_box)) continue;
    const auto image = view->label_images.find(label_key(o, o.planes[i], i));
    if (image == view->label_images.end() || !image->second.data) continue;
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &image->second.dsc;
    lv_area_t area{a.x1 + box.x, a.y1 + box.y, a.x1 + box.x + box.w - 1, a.y1 + box.y + box.h - 1};
    lv_draw_image(layer, &d, &area);
  }
  lap(ui::LABELS);
}

// A scope's beam turning: what it swept since the last frame is drawn again, each plane it crossed is painted where it
// is now with its new label (or goes, when the reports dropped it), drawn again whole where it was and where it is, and
// the planes whose light changed a step are drawn again round their marker, the same pixels everywhere else.
static ap::Box guess_box(const View *view, const ap::Plane &p, ap::Point c) {
  const int r = sizes(view->overlay).plane * 2;
  return ap::join(plane_box(view, p, c), {(int) c.x - 3 * r, (int) c.y - r, 6 * r, 2 * r});
}
static void sweep(lv_timer_t *timer) {
  auto *view = static_cast<View *>(lv_timer_get_user_data(timer));
  if (!view || !view->layer) return;
  lv_area_t a;
  lv_obj_get_coords(view->layer, &a);
  const ap::Overlay &o = view->overlay;
  ap::Overlay &painted = view->painted;
  const ap::Point c = scope_middle(view, a);
  const uint32_t now = lv_tick_get();
  const float was = view->beam_drawn < 0 ? ap::beam(now) : view->beam_drawn, beam = ap::beam(now);
  // A hidden layer draws nothing again: its parent, which lies over the same glass, does.
  lv_obj_t *target = view->hidden ? lv_obj_get_parent(view->layer) : view->layer;
  auto invalidate = [&](const ap::Box &box) {
    lv_area_t area{box.x, box.y, box.x + box.w - 1, box.y + box.h - 1};
    lv_obj_invalidate_area(target, &area);
  };
  const ap::Box swept = ap::wedge_box(c, scope_reach(view, c, a), was - ap::SWEEP_TRAIL - 2, beam + 2);
  if (view->painter) view->painter(was, beam, swept);
  invalidate(swept);
  view->beam_drawn = beam;
  // Hidden (a measurement without the planes, /api/debug): the beam goes on over the map alone.
  if (view->hidden) return;
  for (const auto &box : view->again) invalidate(box);
  view->again.clear();
  auto on_glass = [&](const ap::Plane &p) { return ap::Point{a.x1 + p.x * view->sx, a.y1 + p.y * view->sy}; };
  auto old_box = [&](const ap::Plane &p) {
    const auto d = view->drawn.find(p.id);
    return d != view->drawn.end() && d->second.w > 0 ? d->second : guess_box(view, p, on_glass(p));
  };
  bool changed = false;
  for (const auto &p : o.planes) {
    const ap::Point at = ap::at(o, p, now);
    ap::Plane now_p = p;
    now_p.x = (int) std::lround(at.x);
    now_p.y = (int) std::lround(at.y);
    if (!ap::crossed(was, beam, ap::bearing(c, on_glass(now_p)))) continue;
    auto it = std::find_if(painted.planes.begin(), painted.planes.end(), [&](const ap::Plane &q) { return q.id == p.id; });
    ap::Box box = guess_box(view, now_p, on_glass(now_p));
    if (it != painted.planes.end()) {
      const ap::Box before = old_box(*it);
      box = ap::join(box, ap::join(before, ap::moved(before, (now_p.x - it->x) * view->sx, (now_p.y - it->y) * view->sy)));
      *it = now_p;
    } else {
      painted.planes.push_back(now_p);
    }
    invalidate(box);
    changed = true;
  }
  for (auto it = painted.planes.begin(); it != painted.planes.end();) {
    const bool listed = std::any_of(o.planes.begin(), o.planes.end(), [&](const ap::Plane &p) { return p.id == it->id; });
    if (listed || !ap::crossed(was, beam, ap::bearing(c, on_glass(*it)))) { ++it; continue; }
    invalidate(old_box(*it));
    view->drawn.erase(it->id);
    view->spot_of.erase(it->id);
    it = painted.planes.erase(it);
    changed = true;
  }
  // The watched planes last, so they lie over the others, as the app sends them.
  if (changed) {
    std::stable_partition(painted.planes.begin(), painted.planes.end(), [](const ap::Plane &p) { return p.watch == 0; });
    render_labels(view);
  }
  const int reach = sizes(o).plane * 2;
  for (const auto &p : painted.planes) {
    const ap::Point at = on_glass(p);
    if (glow_step(view, c, at, was) == glow_step(view, c, at, beam)) continue;
    invalidate({(int) at.x - reach, (int) at.y - reach, 2 * reach + 1, 2 * reach + 1});
  }
}
// A scope whose beam turns draws itself again as the beam goes round, as a radar does: a new report, or a plane that
// glides, shows where the beam passes it, and the whole layer is never drawn again at once (a full screen of it is a
// seventh of a second on the AMOLED 2.41, a stall in the beam).
static bool sweeps(const View *view) { return view->overlay.radar.on && view->overlay.radar.sweep && view->sweeping; }
static void stop_sweep(View *view) {
  if (view->sweeping) lv_timer_delete(view->sweeping);
  view->sweeping = nullptr;
  view->beam_drawn = -1;
}

static void deleted(lv_event_t *e) {
  auto *view = static_cast<View *>(lv_event_get_user_data(e));
  if (!view) return;
  view->layer = nullptr;
  stop_sweep(view);
}

void show(View *&view, lv_obj_t *parent, const ap::Overlay &o) {
  if (!parent) return;
  if (!view) view = new View();
  if (!view->layer) {
    view->layer = lv_obj_create(parent);
    lv_obj_remove_style_all(view->layer);
    lv_obj_remove_flag(view->layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(view->layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(view->layer, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(view->layer, draw, LV_EVENT_DRAW_MAIN, view);
    lv_obj_add_event_cb(view->layer, deleted, LV_EVENT_DELETE, view);
    lv_obj_update_layout(view->layer);
  }
  // A scope that sweeps paints the planes as they are when it opens, or when the frame under them changes (another
  // range, another middle); afterwards each plane changes as the beam crosses it (sweep).
  const bool fresh = !view->latched || view->painted.width != o.width || view->painted.height != o.height ||
                     view->painted.radar.x != o.radar.x || view->painted.radar.y != o.radar.y ||
                     view->painted.radar.rose != o.radar.rose || view->frame != view->painted_frame;
  view->overlay = o;
  view->sx = view->sy = 1;
  if (latches(o)) {
    std::vector<ap::Plane> planes = std::move(view->painted.planes);
    view->painted = o;
    view->painted.glide = false;
    if (fresh) {
      for (auto &p : view->painted.planes) {
        const ap::Point at = ap::at(o, p, lv_tick_get());
        p.x = (int) std::lround(at.x);
        p.y = (int) std::lround(at.y);
      }
      view->drawn.clear();
      view->painted_frame = view->frame;
    } else {
      view->painted.planes = std::move(planes);
    }
    view->latched = true;
  } else {
    view->latched = false;
    view->painted = {};
  }
  render_labels(view);
  if (o.radar.on && o.radar.sweep && !view->sweeping) view->sweeping = lv_timer_create(sweep, ap::SWEEP_FRAME_MS, view);
  else if (!(o.radar.on && o.radar.sweep)) stop_sweep(view);
  if (!sweeps(view) || fresh) lv_obj_invalidate(view->layer);
}

void set_beam_painter(View *&view, BeamPainter painter) {
  if (!view) view = new View();
  view->painter = std::move(painter);
}

void new_frame(View *view) {
  if (view) ++view->frame;
}

void set_hidden(View *view, bool hidden) {
  if (!view || view->hidden == hidden) return;
  view->hidden = hidden;
  if (!view->layer) return;
  if (hidden) lv_obj_add_flag(view->layer, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(view->layer, LV_OBJ_FLAG_HIDDEN);
}

void release(View *&view) {
  if (!view) return;
  stop_sweep(view);
  if (view->layer) {
    // Its user data goes first, so the delete event finds no view to write to.
    lv_obj_remove_event_cb_with_user_data(view->layer, deleted, view);
    lv_obj_delete(view->layer);
  }
  delete view;
  view = nullptr;
}

std::string pick(View *view, int x, int y, int reach) {
  if (!view || !view->layer || view->hidden) return "";
  lv_area_t a;
  lv_obj_get_coords(view->layer, &a);
  return ap::pick(shown(view), (float) (x - a.x1), (float) (y - a.y1), (float) reach, lv_tick_get(), view->sx, view->sy);
}

void set_picked(View *view, const std::string &id) {
  if (!view || view->picked == id) return;
  view->picked = id;
  if (view->layer) lv_obj_invalidate(view->layer);
}

void tick(View *view, uint32_t now) {
  if (!view || !view->layer || view->hidden || !view->overlay.glide || view->overlay.planes.empty() || sweeps(view)) return;
  // Gliding until GLIDE_MS after the report, then once more where it stopped.
  if (now - view->overlay.at > ap::GLIDE_MS + 1000) return;
  const ap::Overlay &o = view->overlay;
  if (view->boxes.size() != o.planes.size() || view->laid_at.size() != o.planes.size()) {
    lv_obj_invalidate(view->layer);
    return;
  }
  // Each plane where it was drawn and where it is now, its label with it: a few small boxes, never the whole glass,
  // which takes a seventh of a second to draw again.
  lv_area_t a;
  lv_obj_get_coords(view->layer, &a);
  auto invalidate = [&](const ap::Box &b) {
    lv_area_t area{b.x, b.y, b.x + b.w - 1, b.y + b.h - 1};
    lv_obj_invalidate_area(view->layer, &area);
  };
  for (size_t i = 0; i < o.planes.size(); ++i) {
    const ap::Point at = ap::at(o, o.planes[i], now, view->sx, view->sy);
    const float dx = a.x1 + at.x - view->laid_at[i].x, dy = a.y1 + at.y - view->laid_at[i].y;
    if (std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f) continue;
    invalidate(ap::join(view->boxes[i], ap::moved(view->boxes[i], dx, dy)));
  }
  for (const auto &box : view->again) invalidate(box);
  view->again.clear();
}
}  // namespace sky::view
