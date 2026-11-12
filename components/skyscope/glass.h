#pragma once
// The glass's shape. Most are rectangles the size of the display; a round one is a circle as wide as a square display,
// and its corners are not there at all. What sits on the glass (the pages, the status, the credit, a plane's card, the
// planes and their labels) asks here where it may go, so nothing lands in a corner nobody can see or touch.
#include <algorithm>
#include <cmath>

namespace sky::glass {

struct Shape {
  int width = 0, height = 0;
  bool round = false;
  double radius() const { return std::min(width, height) / 2.0; }
};

// Whether the point (x, y) is on the glass, or within `margin` of it.
inline bool holds(const Shape &g, double x, double y, double margin = 0) {
  if (!g.round) return x >= -margin && x <= g.width + margin && y >= -margin && y <= g.height + margin;
  return std::hypot(x - g.width / 2.0, y - g.height / 2.0) <= g.radius() + margin;
}

// Whether a box at (x, y), `w` by `h`, lies wholly on the glass: on a circle, all four of its corners do.
inline bool holds_box(const Shape &g, double x, double y, double w, double h) {
  if (x < 0 || y < 0 || x + w > g.width || y + h > g.height) return false;
  return !g.round || (holds(g, x, y) && holds(g, x + w, y) && holds(g, x, y + h) && holds(g, x + w, y + h));
}

// How far in from the top (or the bottom) edge a box `w` wide, centered across the glass, must sit for its corners to
// be on it: none on a rectangle; on a circle, the more the wider the box, and half the circle for one wider than it.
inline int edge_inset(const Shape &g, int w) {
  if (!g.round) return 0;
  const double r = g.radius(), half = w / 2.0;
  if (half >= r) return (int) std::ceil(r);
  return (int) std::ceil(r - std::sqrt(r * r - half * half));
}

// A page over the whole glass (the menu, the keyboard, the weather): its padding on each side, `pad` on a rectangle; on
// a circle `pad` inside the largest square the circle holds, so its rows and keys are never cut off by the edge.
struct Insets {
  int left = 0, top = 0, right = 0, bottom = 0;
};
inline Insets page(const Shape &g, int pad) {
  if (!g.round) return {pad, pad, pad, pad};
  const int side = (int) std::floor(g.radius() * std::sqrt(2.0));
  const int x = (g.width - side + 1) / 2 + pad, y = (g.height - side + 1) / 2 + pad;
  return {x, y, x, y};
}
// The width a page leaves its content.
inline int page_width(const Shape &g, int pad) {
  const Insets in = page(g, pad);
  return g.width - in.left - in.right;
}

}  // namespace sky::glass
