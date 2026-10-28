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

// A quick atan2, within a fifth of a degree: the angle of every pixel the trail covers, many thousand a frame.
inline float fast_atan2(float y, float x) {
  const float ax = std::fabs(x), ay = std::fabs(y);
  if (ax == 0 && ay == 0) return 0;
  const float a = std::min(ax, ay) / std::max(ax, ay), s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  return y < 0 ? -r : r;
}
// The bearing of (dx, dy) from the middle, degrees from north (up), clockwise.
inline float bearing(float dx, float dy) {
  const float b = fast_atan2(dx, -dy) * 57.2957795f;
  return b < 0 ? b + 360.0f : b;
}

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
  void paint(float was, float now, const plan::Box &box) const {
    if (!canvas || !background) return;
    const float trail = plan::SWEEP_TRAIL;
    const float clean = trail + std::fmod(now - was + 720.0f, 360.0f) + 3.0f;  // degrees behind the beam to repaint
    const float r2 = reach * reach;
    const float a = now * 3.14159265f / 180.0f, sx = std::sin(a), sy = -std::cos(a);  // along the beam
    const float half = beam_width / 2;
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
        const float behind = std::fmod(now - bearing(dx, dy) + 720.0f, 360.0f);
        if (behind > clean) continue;
        // The beam itself: a line a few pixels wide along the bearing, ahead of the middle.
        const float along = dx * sx + dy * sy, across = std::fabs(dx * sy - dy * sx);
        if (along >= 0 && across <= half) {
          out[x] = beam;
        } else if (behind <= trail) {
          // The afterglow: brightest at the beam, gone `trail` degrees behind it.
          out[x] = blend(under[x], glow, (uint32_t) (110.0f * (1.0f - behind / trail)));
        } else {
          out[x] = under[x];
        }
      }
    }
  }
};
}  // namespace sky::beam
