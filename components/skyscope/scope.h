#pragma once
// Range rings and a compass rose round the middle of the frame, on the radar or over a map.
#include "lvgl.h"
#include "geo.h"

#include <string>

namespace sky::scope {
struct Look {
  bool radar = false;  // the scope's own phosphor and bearing lines, else thin ink over a map
  int tint = 0;        // colours.h TINTS
};
// Draws into `layer` (the map's canvas) for the frame `v`, round its middle.
void draw(lv_layer_t *layer, const geo::View &v, const Look &look);
// The radius of the rose, the largest circle round (cx, cy) the frame holds whole: the circle the beam turns in.
int rose_radius(const geo::View &v, double cx, double cy);
// The nautical miles between two range rings, for a scope whose nearer edge is `near` pixels from its middle.
double ring_step(double px_per_nm, double near);
}  // namespace sky::scope
