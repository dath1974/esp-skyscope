#pragma once
// The small parts the menu and its keyboard are built of (menu.cpp, editor.cpp): plain boxes, labels set only when
// they change (LVGL draws a label again whenever it is set), and arrows drawn as chevrons, with the keyboard's erase
// and shift drawn as outlines as a phone's keyboard has them (the fonts have no symbols, and lines stay sharp at any
// size).
#include "lvgl.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace esphome::skyscope::widgets {

enum { ARROW_UP, ARROW_DOWN, ARROW_LEFT, ARROW_RIGHT, ICON_ERASE, ICON_SHIFT };
inline lv_point_precise_t arrow_points[4][3];  // LVGL keeps a pointer to a line's points: kept here, for every key
inline lv_point_precise_t erase_points[6], erase_cross[2][2], shift_points[8];
inline int arrow_side = 0, arrow_stroke = 0, icon_stroke = 0, erase_wide = 0, erase_tall = 0, shift_wide = 0,
           shift_tall = 0;

inline lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
inline lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *s) {
  lv_obj_t *l = lv_label_create(parent);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, s);
  return l;
}
inline void set_color(lv_obj_t *label, uint32_t color) {
  if (lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(color))) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
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

  // The outlines take a finer line than the chevrons, or the cross inside the erase would close up.
  icon_stroke = std::max(2, stroke * 3 / 5);
  const lv_value_precise_t e = icon_stroke / 2;
  // Erase: a tag pointing left with a cross in it.
  erase_tall = side * 17 / 20;
  erase_wide = side * 5 / 4;
  const int point = erase_tall * 9 / 20, mid = erase_tall / 2;
  const lv_point_precise_t tag[6] = {{e, e + mid},         {e + point, e},           {e + erase_wide, e},
                                     {e + erase_wide, e + erase_tall}, {e + point, e + erase_tall}, {e, e + mid}};
  std::memcpy(erase_points, tag, sizeof(tag));
  // The cross a little left of the middle of the tag's square part, where the eye takes the middle of the whole tag.
  const int cx = (point + erase_wide) / 2 - erase_tall / 10, arm = erase_tall / 5;
  const lv_point_precise_t cross[2][2] = {{{e + cx - arm, e + mid - arm}, {e + cx + arm, e + mid + arm}},
                                          {{e + cx - arm, e + mid + arm}, {e + cx + arm, e + mid - arm}}};
  std::memcpy(erase_cross, cross, sizeof(cross));
  // Shift: an arrow pointing up, a broad head on a short stem.
  shift_tall = side;
  shift_wide = side * 9 / 10;
  const int center = shift_wide / 2, stem = shift_wide / 5, shoulder = shift_tall * 11 / 20;
  const lv_point_precise_t up_arrow[8] = {
      {e + center - stem, e + shift_tall}, {e + center - stem, e + shoulder}, {e, e + shoulder},
      {e + center, e},                     {e + shift_wide, e + shoulder},    {e + center + stem, e + shoulder},
      {e + center + stem, e + shift_tall}, {e + center - stem, e + shift_tall}};
  std::memcpy(shift_points, up_arrow, sizeof(up_arrow));
}
inline lv_obj_t *arrow(lv_obj_t *parent, int which, uint32_t color) {
  lv_obj_t *line = lv_line_create(parent);
  lv_line_set_points(line, arrow_points[which], 3);
  const bool flat = which == ARROW_UP || which == ARROW_DOWN;
  const int along = arrow_side + arrow_stroke, across = arrow_side / 2 + arrow_stroke;
  lv_obj_set_size(line, flat ? along : across, flat ? across : along);
  lv_obj_set_style_line_width(line, arrow_stroke, 0);
  lv_obj_set_style_line_rounded(line, true, 0);
  lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
  lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  return line;
}
// The erase or the shift: its lines in a box of their own, so the key centers it as one.
inline lv_obj_t *icon(lv_obj_t *parent, int which, uint32_t color) {
  const bool erase = which == ICON_ERASE;
  lv_obj_t *box = plain(parent);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(box, (erase ? erase_wide : shift_wide) + icon_stroke, (erase ? erase_tall : shift_tall) + icon_stroke);
  auto stroke = [&](const lv_point_precise_t *points, int count) {
    lv_obj_t *line = lv_line_create(box);
    lv_line_set_points(line, points, count);
    lv_obj_set_size(line, lv_pct(100), lv_pct(100));
    lv_obj_set_style_line_width(line, icon_stroke, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
  };
  if (erase) {
    stroke(erase_points, 6);
    stroke(erase_cross[0], 2);
    stroke(erase_cross[1], 2);
  } else {
    stroke(shift_points, 8);
  }
  return box;
}
// What a key without words shows: a chevron, or one of the keyboard's outlines.
inline lv_obj_t *drawn(lv_obj_t *parent, int which, uint32_t color) {
  return which >= ICON_ERASE ? icon(parent, which, color) : arrow(parent, which, color);
}
}  // namespace esphome::skyscope::widgets
