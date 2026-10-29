#pragma once
// What is drawn over the map once a frame: range rings and a compass rose round its middle, on the radar or over a map,
// and under them the airspace and runways (airspace.h).
#include "lvgl.h"
#include "airspace.h"
#include "geo.h"

#include <string>

namespace sky::scope {
struct Look {
  bool radar = false;  // the scope's own phosphor and bearing lines, else thin ink over a map
  int tint = 0;        // colours.h TINTS
  bool light = false;  // over a map in the light appearance: dark ink over a light halo
};
// Draws into `layer` (the map's canvas) for the frame `v`, round its middle.
void draw(lv_layer_t *layer, const geo::View &v, const Look &look);
// The radius of the rose, the largest circle round (cx, cy) the frame holds whole: the circle the beam turns in.
int rose_radius(const geo::View &v, double cx, double cy);
// The airspace's colours and sizes for `look` (airspace.h draws it): in the phosphor on the radar, Class B the
// brightest, as a controller's map is in one colour; over a map in the charts' blue and magenta.
airspace::Ink airspace_ink(const Look &look);
// The nautical miles between two range rings, for a scope whose nearer edge is `near` pixels from its middle.
double ring_step(double px_per_nm, double near);
}  // namespace sky::scope
