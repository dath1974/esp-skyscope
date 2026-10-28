#pragma once
// Plane symbols drawn once and stamped many times. plan::plane_shape is twelve triangles, and LVGL drew each one (with
// its halo, twenty-four) through a mask every time a plane was drawn: some 25 ms of a radar frame. Here the shape is
// rasterised once per size and heading step into an alpha mask (anti-aliased, 4 x 4 samples a pixel), and view.cpp
// draws the mask in the plane's colour, which LVGL blends in one pass over its few hundred pixels.
#include <algorithm>
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

// The plane symbol `length` pixels nose to tail, turned to heading step `step`. Each triangle marks the samples inside
// its own box on a grid SAMPLES times finer than the mask, and each pixel is the share of its samples marked: a few
// milliseconds a mask, where testing every sample against every triangle took some 145 ms on the ESP32-S3.
inline Mask render(float length, int step) {
  Mask m;
  m.size = (int) std::ceil(length) + 3;
  m.alpha.assign((size_t) m.size * m.size, 0);
  const int fine = m.size * SAMPLES;
  std::vector<uint8_t> hit((size_t) fine * fine, 0);
  const float mid = m.size / 2.0f * SAMPLES;
  for (const auto &t : plan::plane_shape({mid, mid}, length * SAMPLES, step * HEADING_STEP)) {
    const float x0 = std::min({t.p[0].x, t.p[1].x, t.p[2].x}), x1 = std::max({t.p[0].x, t.p[1].x, t.p[2].x});
    const float y0 = std::min({t.p[0].y, t.p[1].y, t.p[2].y}), y1 = std::max({t.p[0].y, t.p[1].y, t.p[2].y});
    const int sx0 = std::max(0, (int) std::floor(x0)), sx1 = std::min(fine - 1, (int) std::ceil(x1));
    const int sy0 = std::max(0, (int) std::floor(y0)), sy1 = std::min(fine - 1, (int) std::ceil(y1));
    for (int y = sy0; y <= sy1; ++y) {
      uint8_t *row = hit.data() + (size_t) y * fine;
      for (int x = sx0; x <= sx1; ++x)
        if (!row[x] && inside(t, x + 0.5f, y + 0.5f)) row[x] = 1;
    }
  }
  for (int y = 0; y < m.size; ++y)
    for (int x = 0; x < m.size; ++x) {
      int hits = 0;
      for (int sy = 0; sy < SAMPLES; ++sy) {
        const uint8_t *row = hit.data() + (size_t) (y * SAMPLES + sy) * fine + x * SAMPLES;
        for (int sx = 0; sx < SAMPLES; ++sx) hits += row[sx];
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
  // The mask of `length` and `heading`. At most `budget` masks are made by the calls of one `frame`: past that, a mask
  // not made yet is the nearest heading of that length already made (for a frame or two), so no frame waits for many.
  Entry *get(int length, int heading, uint32_t frame = 0, int budget = 1 << 30) {
    const int step = heading_step(heading);
    auto found = entries_.find(key(length, step));
    if (found != entries_.end()) return found->second.get();
    if (frame != frame_) {
      frame_ = frame;
      made_ = 0;
    }
    if (made_ >= budget) {
      constexpr int steps = 360 / HEADING_STEP;
      for (int d = 1; d <= steps / 2; ++d)
        for (const int near : {(step + d) % steps, (step - d + steps) % steps}) {
          auto other = entries_.find(key(length, near));
          if (other != entries_.end()) return other->second.get();
        }
      // None of this length yet: this one is made all the same.
    }
    ++made_;
    auto entry = std::make_unique<Entry>();
    entry->mask = render((float) length, step);
    return (entries_[key(length, step)] = std::move(entry)).get();
  }
  size_t size() const { return entries_.size(); }

 private:
  static uint32_t key(int length, int step) { return (uint32_t) length << 8 | (uint32_t) step; }
  std::map<uint32_t, std::unique_ptr<Entry>> entries_;
  uint32_t frame_ = 0;
  int made_ = 0;
};
}  // namespace sky::sprites
