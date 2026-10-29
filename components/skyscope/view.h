#pragma once
// Planes over the map on the glass: plan.h's planes painted in one object that lies over the map, in its draw event.
#include "lvgl.h"
#include "plan.h"

#include <functional>
#include <string>
#include <vector>

namespace sky::view {
struct View;  // the object over the map and the planes it shows
// Shows `o` in a layer filling `parent`, made the first time; afterwards only its planes change.
void show(View *&view, lv_obj_t *parent, const plan::Overlay &o);
// The map under the planes changed (another range, a new picture): a sweeping scope paints its planes anew.
void new_frame(View *view);
// What paints a sweeping scope's beam and its afterglow into the map under the planes, from bearing `was` to `now`,
// within `box` (beam.h): called by the sweep before it draws that box again.
using BeamPainter = std::function<void(float was, float now, const plan::Box &box)>;
void set_beam_painter(View *&view, BeamPainter painter);
void set_hidden(View *view, bool hidden);
void release(View *&view);
// The id of the plane nearest a finger at `x`, `y` on the glass, within `reach` pixels of it; "" for none.
std::string pick(View *view, int x, int y, int reach);
// The planes as a finger finds them now, on the glass: where each is drawn (on a sweeping scope, where the beam last
// painted it; a plane it has not reached yet is not there).
struct Spot {
  std::string id;
  int x, y;
  uint8_t kind;
};
std::vector<Spot> spots(View *view);
// The plane picked, ringed; "" for none.
void set_picked(View *view, const std::string &id);
// Every 200 ms: planes that glide are drawn where they are now, each in the small part of the glass it covers.
void tick(View *view, uint32_t now);
// How many entries each of the layer's own records holds, for /api/status: what grows as the screen runs.
struct Counts { size_t spot_of = 0, drawn = 0, box_of = 0, label_images = 0, symbols = 0, again = 0; };
Counts counts(const View *view);
// A radar scope's colours: its phosphor at `light` (0 the ground, 1 the glow, 2 the hottest light), and its ground.
lv_color_t phosphor(const plan::Overlay &o, float light);
lv_color_t ground(const plan::Overlay &o);
}  // namespace sky::view
