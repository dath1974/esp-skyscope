#pragma once
// The screen's sizes and fonts: every size in code is a number of millimetres or of reference pixels turned into the
// glass's own pixels here, so a board only states its density (`dpi` in the YAML).
#include "lvgl.h"

#include <cmath>
#include <string>

namespace sky::ui {
// Reference pixels are those of a 200 dpi glass.
inline float density = 1.0f;  // the glass's pixels per reference pixel
inline float px_per_mm = 200.0f / 25.4f;
inline int px(float reference) { return (int) std::lround(reference * density); }
inline int mm(float millimetres) { return (int) std::lround(millimetres * px_per_mm); }
inline void set_dpi(float dpi) {
  density = dpi / 200.0f;
  px_per_mm = dpi / 25.4f;
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
enum Phase : uint8_t { IDLE, FEED_FETCH, FEED_PARSE, TILE_FETCH, TILE_DECODE, PHASES };
inline const char *const PHASE_NAMES[PHASES] = {"idle", "feed_fetch", "feed_parse", "tile_fetch", "tile_decode"};
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
