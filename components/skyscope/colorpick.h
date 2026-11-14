#pragma once
// The menu's color picker for the radar's phosphor (colorpick.cpp draws it): the named phosphors as swatches, and a
// slider for each of red, green and blue, whose numbers can be typed too, as can the color whole. What it keeps is the
// color as chosen; the settings light it enough for a scope when it is put in them (colors.h phosphor_lit), and the
// picker says so first.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

#include "colors.h"
#include "settings.h"

namespace sky::colorpick {
inline constexpr const char *CHANNELS[3] = {"Red", "Green", "Blue"};

// What the picker takes under its title at full size, in millimeters: a small scope and the color written out, a row
// of swatches, and three sliders. On a glass with less room (the AMOLED's is some 37 mm across) all of it is drawn
// smaller by the one share, so nothing falls off its edge.
constexpr float NEED_WIDE_MM = 54, NEED_TALL_MM = 53;
// That share, for a room `width` by `height` pixels at `px_per_mm`: 1 where it all fits.
inline float scale_for(int width, int height, float px_per_mm) {
  const float wide = width / (NEED_WIDE_MM * px_per_mm), tall = height / (NEED_TALL_MM * px_per_mm);
  const float k = wide < tall ? wide : tall;
  return k < 1 ? (k > 0.3f ? k : 0.3f) : 1;
}

// A channel of `color` (0 red, 1 green, 2 blue), 0 to 255; and `color` with it set to `value`.
inline int channel(uint32_t color, int which) { return (int) (color >> (16 - 8 * which) & 0xFF); }
inline uint32_t with_channel(uint32_t color, int which, int value) {
  const int shift = 16 - 8 * which;
  value = value < 0 ? 0 : value > 255 ? 255 : value;
  return (color & ~(0xFFu << shift) & 0xFFFFFF) | (uint32_t) value << shift;
}

// A slider `width` wide with a knob `knob` wide: the knob's left edge for `value`, and the value a finger `x` from the
// slider's left edge gives, its knob centered under the finger (and held at the ends past them).
inline int knob_left(int value, int width, int knob) {
  const int room = width - knob;
  return room <= 0 ? 0 : (value * room + 127) / 255;
}
inline int value_at(int x, int width, int knob) {
  const int room = width - knob;
  if (room <= 0) return 0;
  const int v = ((x - knob / 2) * 255 + room / 2) / room;
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

// "#FF66CC".
inline std::string hex_of(uint32_t color) {
  char text[8];
  std::snprintf(text, sizeof(text), "#%06X", (unsigned) (color & 0xFFFFFF));
  return text;
}
// What the menu's key says for a phosphor: a named one's name ("Green"), else its color as hex.
inline std::string name_of(uint32_t color) {
  const int named = colors::phosphor_named(color);
  if (named < 0) return hex_of(color);
  std::string name = colors::PHOSPHORS[named].name;
  name[0] = (char) std::toupper((unsigned char) name[0]);
  return name;
}

// A channel typed on the keyboard: a whole number 0 to 255. False for anything else.
inline bool channel_typed(const std::string &text, int &value) {
  if (text.empty() || text.size() > 3) return false;
  int v = 0;
  for (char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (c - '0');
  }
  if (v > 255) return false;
  value = v;
  return true;
}
// The color typed whole: "#RRGGBB", "RRGGBB" or a name (settings.h phosphor_typed), as it is.
inline bool color_typed(const std::string &text, uint32_t &color) { return phosphor_typed(text.c_str(), color); }

// What the picker says under the color: its name, or that it is brightened when kept, and to what.
inline std::string note_of(uint32_t color) {
  const uint32_t lit = colors::phosphor_lit(color);
  if (lit != color) return "Too dark for the scope: kept as " + hex_of(lit);
  return colors::phosphor_named(color) >= 0 ? name_of(color) : "Your own color";
}
}  // namespace sky::colorpick
