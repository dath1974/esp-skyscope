#pragma once
// Planes over the map on the glass: plan.h's planes painted in one object that lies over the map, in its draw event.
#include "lvgl.h"
#include "plan.h"

#include <string>

namespace sky::view {
struct View;  // the object over the map and the planes it shows
// Shows `o` in a layer filling `parent`, made the first time; afterwards only its planes change.
void show(View *&view, lv_obj_t *parent, const plan::Overlay &o);
// The map under the planes changed (another range, a new picture): a sweeping scope paints its planes anew.
void new_frame(View *view);
void set_hidden(View *view, bool hidden);
void release(View *&view);
// The id of the plane nearest a finger at `x`, `y` on the glass, within `reach` pixels of it; "" for none.
std::string pick(View *view, int x, int y, int reach);
// The plane picked, ringed; "" for none.
void set_picked(View *view, const std::string &id);
// Every second: planes that glide are drawn where they are now.
void tick(View *view, uint32_t now);
// A radar scope's colours: its phosphor at `light` (0 the ground, 1 the glow, 2 the hottest light), and its ground.
lv_color_t phosphor(const plan::Overlay &o, float light);
lv_color_t ground(const plan::Overlay &o);
}  // namespace sky::view
