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
