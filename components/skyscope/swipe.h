#pragma once
// A swipe left or right along the strip at the bottom of the map changes the map (swipe.cpp), through the menu's own
// Map row (menu_rows.h), so it goes through the same maps in the same order and skips the same ones. A drag that
// starts in the strip is never a pan (preview.cpp); everywhere else on the map it is.
#include <cmath>
#include <vector>

#include "menu_rows.h"
#include "settings.h"

namespace sky::swipe {

constexpr float STRIP_MM = 6;   // the strip's height, along the bottom edge
constexpr float SWIPE_MM = 8;   // the least a finger goes sideways for a swipe

// Whether a finger that came down at `y` is in the strip of a glass `height` high, `strip_px` of it.
inline bool in_strip(double y, int height, int strip_px) { return y >= height - strip_px && y < height; }

// A finger from (x0, y0) to (x1, y1): 1 for a swipe to the left (the next map), -1 to the right (the one before), 0
// for none: at least `least` sideways, and at least twice as far sideways as up or down.
inline int direction(double x0, double y0, double x1, double y1, double least) {
  const double dx = x1 - x0, dy = y1 - y0;
  if (std::fabs(dx) < least || std::fabs(dx) < 2 * std::fabs(dy)) return 0;
  return dx < 0 ? 1 : -1;
}

inline const menu::Row &map_row() {
  for (const auto &r : menu::rows())
    if (std::string(r.key) == "map") return r;
  return menu::rows()[0];
}

// The maps a swipe goes through, in its order: every one, but the custom server only where one is set.
inline std::vector<MapStyle> maps(const Settings &s) {
  std::vector<MapStyle> out;
  for (const auto &c : map_row().choices) {
    const int m = MAP_STYLES.find(c.value);
    if (m < 0 || (m == CUSTOM && s.custom_url.empty())) continue;
    out.push_back((MapStyle) m);
  }
  return out;
}
}  // namespace sky::swipe
