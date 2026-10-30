#pragma once
// The small parts the menu and its keyboard are built of (menu.cpp, editor.cpp): plain boxes, labels set only when
// they change (LVGL draws a label again whenever it is set), and arrows drawn as chevrons (the fonts have no arrows,
// and lines stay sharp at any size).
#include "lvgl.h"

#include <cstring>
#include <string>

namespace esphome::skyscope::widgets {

enum { ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT };
inline lv_point_precise_t arrow_points[4][3];  // LVGL keeps a pointer to a line's points: kept here, for every key
inline int arrow_side = 0, arrow_stroke = 0;

inline lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
inline lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, const char *s) {
  lv_obj_t *l = lv_label_create(parent);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
  lv_label_set_text(l, s);
  return l;
}
inline void set_colour(lv_obj_t *label, uint32_t colour) {
  if (lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(colour))) {
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
  }
}
inline void set_label(lv_obj_t *label, const std::string &s) {
  if (s != lv_label_get_text(label)) lv_label_set_text(label, s.c_str());
}
inline void set_hidden(lv_obj_t *o, bool hidden) {
  if (hidden != lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}
// The chevrons `side` wide (up, down) or tall (left, right), half as deep, for a stroke `stroke` wide: its round ends
// inside the line's own box.
inline void make_arrows(int side, int stroke) {
  if (side == arrow_side && stroke == arrow_stroke) return;
  arrow_side = side;
  arrow_stroke = stroke;
  const lv_value_precise_t h = side / 2, o = stroke / 2;
  const lv_point_precise_t up[3] = {{o, o + h}, {o + h, o}, {o + side, o + h}};
  const lv_point_precise_t down[3] = {{o, o}, {o + h, o + h}, {o + side, o}};
  const lv_point_precise_t left[3] = {{o + h, o}, {o, o + h}, {o + h, o + side}};
  const lv_point_precise_t right[3] = {{o, o}, {o + h, o + h}, {o, o + side}};
  std::memcpy(arrow_points[ARROW_UP], up, sizeof(up));
  std::memcpy(arrow_points[ARROW_DOWN], down, sizeof(down));
  std::memcpy(arrow_points[ARROW_LEFT], left, sizeof(left));
  std::memcpy(arrow_points[ARROW_RIGHT], right, sizeof(right));
}
inline lv_obj_t *arrow(lv_obj_t *parent, int which, uint32_t colour) {
  lv_obj_t *line = lv_line_create(parent);
  lv_line_set_points(line, arrow_points[which], 3);
  const bool flat = which == ARROW_UP || which == ARROW_DOWN;
  const int along = arrow_side + arrow_stroke, across = arrow_side / 2 + arrow_stroke;
  lv_obj_set_size(line, flat ? along : across, flat ? across : along);
  lv_obj_set_style_line_width(line, arrow_stroke, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  lv_obj_set_style_line_color(line, lv_color_hex(colour), 0);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  return line;
}
}  // namespace esphome::skyscope::widgets
