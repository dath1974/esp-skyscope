#pragma once
// The screen's sizes and fonts: every size in code is a number of millimeters or of reference pixels turned into the
// glass's own pixels here, so a board only states its density (`dpi` in the YAML).
#include "lvgl.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "glass.h"

namespace sky::ui {
// Reference pixels are those of a 200 dpi glass.
inline float density = 1.0f;  // the glass's pixels per reference pixel
inline float px_per_mm = 200.0f / 25.4f;
inline int px(float reference) { return (int) std::lround(reference * density); }
inline int mm(float millimeters) { return (int) std::lround(millimeters * px_per_mm); }
inline void set_dpi(float dpi) {
  density = dpi / 200.0f;
  px_per_mm = dpi / 25.4f;
}

// The glass's size and shape (glass.h), once LVGL's display is known.
inline glass::Shape shape;

// A page over the whole glass, padded `pad` all round, and on round glass kept inside the circle.
inline void pad_page(lv_obj_t *page, int pad) {
  const glass::Insets in = glass::page(shape, pad);
  lv_obj_set_style_pad_left(page, in.left, 0);
  lv_obj_set_style_pad_top(page, in.top, 0);
  lv_obj_set_style_pad_right(page, in.right, 0);
  lv_obj_set_style_pad_bottom(page, in.bottom, 0);
}

// A box along the top or bottom edge, aligned as LVGL aligns it (`align` one of the TOP_ or BOTTOM_ ones, `x` and `y`
// from there). On round glass it is centered instead, `y` in from the edge plus as far as its width needs to lie wholly
// on the glass, and moved again whenever its size changes: a status that grows, a card with more rows.
inline void place_on_edge(lv_obj_t *o, bool top, int gap) {
  const int in = glass::edge_inset(shape, lv_obj_get_width(o)) + gap;
  lv_obj_align(o, top ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, top ? in : -in);
}
inline void hug_edge(lv_obj_t *o, lv_align_t align, int x, int y) {
  if (!shape.round) {
    lv_obj_align(o, align, x, y);
    return;
  }
  const bool top = align == LV_ALIGN_TOP_LEFT || align == LV_ALIGN_TOP_MID || align == LV_ALIGN_TOP_RIGHT;
  const intptr_t packed = (intptr_t) std::abs(y) << 1 | (top ? 1 : 0);
  place_on_edge(o, top, std::abs(y));
  lv_obj_add_event_cb(o, [](lv_event_t *e) {
    const intptr_t packed = (intptr_t) lv_event_get_user_data(e);
    place_on_edge(lv_event_get_current_target_obj(e), packed & 1, (int) (packed >> 1));
  }, LV_EVENT_SIZE_CHANGED, (void *) packed);
}

// Frames of the glass LVGL has started, and how long they took (skyscope.cpp counts them): what /api/status reports.
inline uint32_t frame = 0;
struct FrameStats {
  uint32_t frames = 0;
  float per_second = 0, average_ms = 0, longest_ms = 0;
  // Of an average frame: drawing it, waiting for the panel's tearing signal, turning and sending it, and its pixels.
  float draw_ms = 0, wait_ms = 0, send_ms = 0, pixels = 0;
};
inline FrameStats frame_stats;  // of the last five seconds

// What the worker is doing, so a slow frame can be put down to it, and the slow frames (over SLOW_MS) by what it was
// doing as they started, since the screen started: /api/status.
enum Phase : uint8_t { IDLE, FEED_FETCH, FEED_PARSE, TILE_FETCH, TILE_DECODE, AIRSPACE, PHASES };
inline const char *const PHASE_NAMES[PHASES] = {"idle", "feed_fetch", "feed_parse", "tile_fetch", "tile_decode",
                                                "airspace"};
inline volatile uint8_t worker_phase = IDLE;
constexpr float SLOW_MS = 150;
inline uint32_t slow_frames[PHASES] = {};
inline uint32_t slow_with_report = 0;  // of those, frames that came while the loop took a report
inline volatile bool report_taken = false;
// Microseconds the plane layer's draw spent in this frame, by part: its tracks, its markers, laying out its labels,
// and its leaders and labels; and how many times LVGL asked it to draw.
enum Part : uint8_t { TRACKS, MARKERS, LAYOUT, LABELS, PARTS };
inline int64_t part_us[PARTS] = {};
inline uint32_t draw_calls = 0;
// Steps the beam has taken (view.cpp's sweep), and ESPHome's loop: passes and the longest gap between two, for
// /api/status.
inline uint32_t sweep_steps = 0;
inline int64_t paint_us = 0;  // spent painting the beam into the map, summed

struct Fonts {
  const lv_font_t *small = nullptr;  // planes' labels on a small glass, credits
  const lv_font_t *label = nullptr;  // planes' labels, status
  const lv_font_t *title = nullptr;  // the card of a plane picked, the setup screen
};
inline Fonts fonts;

inline int text_width(const std::string &text, const lv_font_t *font) {
  lv_point_t size;
  lv_text_get_size(&size, text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}
}  // namespace sky::ui
