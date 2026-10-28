#pragma once
// Plane symbols drawn once and stamped many times. plan::plane_shape is twelve triangles, and LVGL drew each one (with
// its halo, twenty-four) through a mask every time a plane was drawn: some 25 ms of a radar frame. Here the shape is
// rasterised once per size and heading step into an alpha mask (anti-aliased, 4 x 4 samples a pixel), and view.cpp
// draws the mask in the plane's colour, which LVGL blends in one pass over its few hundred pixels.
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "plan.h"

namespace sky::sprites {
// Headings are drawn in steps of this many degrees: at the largest symbol a step moves its nose by under 2 pixels.
constexpr int HEADING_STEP = 4;
constexpr int SAMPLES = 4;  // a side, so 16 a pixel

struct Mask {
  int size = 0;                // width and height, the shape's middle at size / 2
  std::vector<uint8_t> alpha;  // size * size, 0 to 255
};

inline int heading_step(int heading) { return ((heading < 0 ? 0 : heading) % 360 + HEADING_STEP / 2) / HEADING_STEP % (360 / HEADING_STEP); }

inline bool inside(const plan::Triangle &t, float x, float y) {
  auto edge = [&](const plan::Point &a, const plan::Point &b) { return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x); };
  const float e0 = edge(t.p[0], t.p[1]), e1 = edge(t.p[1], t.p[2]), e2 = edge(t.p[2], t.p[0]);
  return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

// The plane symbol `length` pixels nose to tail, turned to heading step `step`.
inline Mask render(float length, int step) {
  Mask m;
  m.size = (int) std::ceil(length) + 3;
  m.alpha.assign((size_t) m.size * m.size, 0);
  const float mid = m.size / 2.0f;
  const auto shape = plan::plane_shape({mid, mid}, length, step * HEADING_STEP);
  for (int y = 0; y < m.size; ++y)
    for (int x = 0; x < m.size; ++x) {
      int hits = 0;
      for (int sy = 0; sy < SAMPLES; ++sy)
        for (int sx = 0; sx < SAMPLES; ++sx) {
          const float px = x + (sx + 0.5f) / SAMPLES, py = y + (sy + 0.5f) / SAMPLES;
          for (const auto &t : shape)
            if (inside(t, px, py)) { ++hits; break; }
        }
      m.alpha[(size_t) y * m.size + x] = (uint8_t) (hits * 255 / (SAMPLES * SAMPLES));
    }
  return m;
}

// Masks made as they are first needed, by length (whole pixels) and heading step, kept for as long as the screen runs:
// a few sizes (plain, watched, each with its halo) times 90 headings, about 2.5 KB each.
template<typename Extra> class Cache {
 public:
  struct Entry {
    Mask mask;
    Extra extra;  // what the drawing side keeps beside it (an LVGL image descriptor)
  };
  Entry &get(int length, int heading) {
    const int step = heading_step(heading);
    const uint32_t key = (uint32_t) length << 8 | (uint32_t) step;
    auto found = entries_.find(key);
    if (found != entries_.end()) return *found->second;
    auto entry = std::make_unique<Entry>();
    entry->mask = render((float) length, step);
    return *(entries_[key] = std::move(entry));
  }
  size_t size() const { return entries_.size(); }

 private:
  std::map<uint32_t, std::unique_ptr<Entry>> entries_;
};
}  // namespace sky::sprites
