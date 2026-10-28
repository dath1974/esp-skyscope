#pragma once
// The radar's beam and its afterglow, painted straight into the map's canvas (RGB565) by the screen's own code. LVGL
// drew them as see-through shapes, a mask over every pixel the beam swept: some 1.7 microseconds a pixel on the
// ESP32-S3, 80 ms and more a frame. Here a pixel the trail covers is one blend of the scope's own pixel (kept clean in
// `background`) with the glow, and a pixel it left is that clean pixel again; LVGL then only copies the canvas.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "colours.h"
#include "plan.h"

namespace sky::beam {

inline uint16_t pack(uint32_t rgb) {
  return (uint16_t) ((rgb >> 8 & 0xF800) | (rgb >> 5 & 0x07E0) | (rgb >> 3 & 0x001F));
}
// `opa` (0 to 256) of `over` on `under`, both RGB565.
inline uint16_t blend(uint16_t under, uint16_t over, uint32_t opa) {
  const uint32_t r = ((under >> 11) * (256 - opa) + (over >> 11) * opa) >> 8;
  const uint32_t g = ((under >> 5 & 0x3F) * (256 - opa) + (over >> 5 & 0x3F) * opa) >> 8;
  const uint32_t b = ((under & 0x1F) * (256 - opa) + (over & 0x1F) * opa) >> 8;
  return (uint16_t) (r << 11 | g << 5 | b);
}

struct Painter {
  uint16_t *canvas = nullptr, *background = nullptr;
  int width = 0, height = 0;
  float cx = 0, cy = 0, reach = 0;
  uint16_t glow = 0, beam = 0;
  float beam_width = 2;

  void set_tint(const colours::Tint &t) {
    glow = pack(t.glow);
    beam = pack(colours::mix(t.hot, t.glow, 153));
  }

  // The beam at `now` (degrees) with its trail behind it, and every pixel between `was - TRAIL` and the trail's end
  // made clean, within `box`: what plan.h's wedge_box(was - TRAIL, now) gives, which the sweep invalidates.
  //
  // How far a pixel is behind the beam comes from the dot and cross products of its offset with the beam's direction
  // (the tangent of the angle between them), not from its bearing: no arc tangent a pixel, which with a modulo cost some
  // 50 ms a step over a box of 28,000 pixels and held ESPHome's loop to 12 passes a second.
  void paint(float was, float now, const plan::Box &box) const {
    if (!canvas || !background) return;
    const float trail = plan::SWEEP_TRAIL;
    const float clean = trail + std::fmod(now - was + 720.0f, 360.0f) + 3.0f;  // degrees behind the beam to repaint
    // Every angle here is under 90 degrees: past `clean` a pixel is left alone, as the box only just holds the wedge.
    const float tan_clean = std::tan(std::min(clean, 80.0f) * 3.14159265f / 180.0f);
    const float r2 = reach * reach;
    const float a = now * 3.14159265f / 180.0f, bx = std::sin(a), by = -std::cos(a);  // along the beam
    const float half = beam_width / 2;
    const float per_degree = 110.0f / trail;
    const int x0 = std::max(0, box.x), x1 = std::min(width - 1, box.x + box.w - 1);
    const int y0 = std::max(0, box.y), y1 = std::min(height - 1, box.y + box.h - 1);
    for (int y = y0; y <= y1; ++y) {
      const float dy = y + 0.5f - cy;
      if (dy * dy > r2) continue;
      // Only the row's span inside the scope's circle.
      const float span = std::sqrt(r2 - dy * dy);
      const int from = std::max(x0, (int) std::floor(cx - span)), to = std::min(x1, (int) std::ceil(cx + span));
      uint16_t *out = canvas + y * width;
      const uint16_t *under = background + y * width;
      for (int x = from; x <= to; ++x) {
        const float dx = x + 0.5f - cx;
        const float along = dx * bx + dy * by;      // |p| cos(behind)
        const float behind_s = by * dx - bx * dy;   // |p| sin(behind): positive behind the beam, negative ahead
        if (along <= 0) continue;
        // The beam itself: a line a few pixels wide along the bearing, ahead of the middle.
        if (std::fabs(behind_s) <= half) {
          out[x] = beam;
          continue;
        }
        if (behind_s < 0 || behind_s > tan_clean * along) continue;
        const float t = behind_s / along;  // tan(behind), under tan(clean)
        const float degrees = t * (1.0f - t * t * (0.3333333f - t * t * 0.2f)) * 57.2957795f;
        // The afterglow: brightest at the beam, gone `trail` degrees behind it; past it, the scope again.
        out[x] = degrees <= trail ? blend(under[x], glow, (uint32_t) (110.0f - degrees * per_degree)) : under[x];
      }
    }
  }
};
}  // namespace sky::beam
