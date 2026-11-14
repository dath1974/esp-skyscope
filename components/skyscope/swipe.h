#pragma once
// A swipe left or right along the strip at the bottom of the map changes the map (swipe.cpp), in the order of the
// menu's own Map row (menu_rows.h), but past the custom map until its server is set: the menu offers it to set one. A
// drag that starts in the strip is never a pan (preview.cpp); everywhere else on the map it is.
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "menu_rows.h"
#include "settings.h"

namespace sky::swipe {

constexpr float STRIP_MM = 6;   // the strip's height, along the bottom edge
constexpr float SWIPE_MM = 8;   // the least a finger goes sideways for a swipe

// Whether a finger that came down at `y` is in the strip of a glass `height` high, `strip_px` of it.
inline bool in_strip(double y, int height, int strip_px) { return y >= height - strip_px && y < height; }

// The same along the top edge, where a swipe down opens the day's tally (todaypage.cpp).
inline bool in_top_strip(double y, int strip_px) { return y >= 0 && y < strip_px; }

// A finger from (x0, y0) to (x1, y1): 1 for a swipe to the left (the next map), -1 to the right (the one before), 0
// for none: at least `least` sideways, and at least twice as far sideways as up or down.
inline int direction(double x0, double y0, double x1, double y1, double least) {
  const double dx = x1 - x0, dy = y1 - y0;
  if (std::fabs(dx) < least || std::fabs(dx) < 2 * std::fabs(dy)) return 0;
  return dx < 0 ? 1 : -1;
}

// A finger from (x0, y0) to (x1, y1) going up: at least `least` up, and at least twice as far up as across.
inline bool upward(double x0, double y0, double x1, double y1, double least) {
  const double dx = x1 - x0, dy = y0 - y1;
  return dy >= least && dy >= 2 * std::fabs(dx);
}

// And going down: at least `least` down, and at least twice as far down as across.
inline bool downward(double x0, double y0, double x1, double y1, double least) {
  return upward(x0, -y0, x1, -y1, least);
}

inline const menu::Row &map_row() {
  for (const auto &r : menu::rows())
    if (std::string(r.key) == "map") return r;
  return menu::rows()[0];
}

// The maps a swipe goes through, in the menu's order: every one, but the custom map only once its server is set. The
// menu offers it either way, to set the server under it; a swipe never lands on a map that would show nothing.
inline std::vector<MapStyle> maps(const Settings &s) {
  std::vector<MapStyle> out;
  for (const auto &c : map_row().choices) {
    const int m = MAP_STYLES.find(c.value);
    if (m < 0 || (m == CUSTOM && s.custom_url.empty())) continue;
    out.push_back((MapStyle) m);
  }
  return out;
}

// `s` with the map a swipe goes to, `step` on (1) or back (-1) in maps(), around at the ends; from the custom map with
// no server (chosen in the menu), on from where it is in the menu's order. False where nothing changes.
inline bool swiped(Settings &s, int step) {
  const auto all = maps(s);
  const auto &choices = map_row().choices;
  const int n = (int) choices.size();
  int at = 0;
  for (int i = 0; i < n; ++i)
    if (MAP_STYLES.find(choices[i].value) == s.map) at = i;
  for (int k = 1; k <= n; ++k) {
    const int m = MAP_STYLES.find(choices[((at + step * k) % n + n) % n].value);
    if (m == s.map || std::find(all.begin(), all.end(), (MapStyle) m) == all.end()) continue;
    JsonDocument change;
    change["map"] = MAP_STYLES[m];
    std::string error;
    return apply(change.as<JsonObjectConst>(), s, error);
  }
  return false;
}
}  // namespace sky::swipe
