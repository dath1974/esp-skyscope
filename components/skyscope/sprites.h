#pragma once
// Plane symbols drawn once and stamped many times. plan::plane_shape is twelve triangles, and LVGL drew each one (with
// its halo, twenty-four) through a mask every time a plane was drawn: some 25 ms of a radar frame. Here the shape is
// rasterised once per size and heading step into an alpha mask (anti-aliased, 4 x 4 samples a pixel), and view.cpp
// draws the mask in the plane's colour, which LVGL blends in one pass over its few hundred pixels.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <memory>
#include <vector>

#include "plan.h"

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif

namespace sky::sprites {
// Headings are drawn in steps of this many degrees: at the largest symbol a step moves its nose by under 2 pixels.
constexpr int HEADING_STEP = 4;
constexpr int SAMPLES = 4;  // a side, so 16 a pixel

// Scratch memory for the sample grid: PSRAM on the screen, the heap on a PC; nullptr when there is none.
inline void *scratch(size_t bytes) {
#if __has_include(<esp_heap_caps.h>)
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return std::malloc(bytes);
#endif
}
inline void release(void *p) {
#if __has_include(<esp_heap_caps.h>)
  heap_caps_free(p);
#else
  std::free(p);
#endif
}

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
  const int size = (int) std::ceil(length) + 3;
  const int fine = size * SAMPLES;
  // The sample grid is the one large allocation here (a watched plane's at full size: some 80 KB): from PSRAM where the
  // screen has it, and checked, so a screen short of memory skips a symbol for a frame instead of stopping.
  std::unique_ptr<uint8_t, void (*)(void *)> hit(static_cast<uint8_t *>(scratch((size_t) fine * fine)), release);
  if (!hit) return m;
  std::memset(hit.get(), 0, (size_t) fine * fine);
  m.size = size;
  m.alpha.assign((size_t) m.size * m.size, 0);
  const float mid = m.size / 2.0f * SAMPLES;
  for (const auto &t : plan::plane_shape({mid, mid}, length * SAMPLES, step * HEADING_STEP)) {
    const float x0 = std::min({t.p[0].x, t.p[1].x, t.p[2].x}), x1 = std::max({t.p[0].x, t.p[1].x, t.p[2].x});
    const float y0 = std::min({t.p[0].y, t.p[1].y, t.p[2].y}), y1 = std::max({t.p[0].y, t.p[1].y, t.p[2].y});
    const int sx0 = std::max(0, (int) std::floor(x0)), sx1 = std::min(fine - 1, (int) std::ceil(x1));
    const int sy0 = std::max(0, (int) std::floor(y0)), sy1 = std::min(fine - 1, (int) std::ceil(y1));
    for (int y = sy0; y <= sy1; ++y) {
      uint8_t *row = hit.get() + (size_t) y * fine;
      for (int x = sx0; x <= sx1; ++x)
        if (!row[x] && inside(t, x + 0.5f, y + 0.5f)) row[x] = 1;
    }
  }
  for (int y = 0; y < m.size; ++y)
    for (int x = 0; x < m.size; ++x) {
      int hits = 0;
      for (int sy = 0; sy < SAMPLES; ++sy) {
        const uint8_t *row = hit.get() + (size_t) (y * SAMPLES + sy) * fine + x * SAMPLES;
        for (int sx = 0; sx < SAMPLES; ++sx) hits += row[sx];
      }
      m.alpha[(size_t) y * m.size + x] = (uint8_t) (hits * 255 / (SAMPLES * SAMPLES));
    }
  return m;
}

// Masks made as they are first needed, by length (whole pixels) and heading step, kept for as long as the screen runs:
// a few lengths (plain, watched, each with its halo, larger off the radar) times 90 headings. Each length is one table
// of 90 entries, and each entry is made in PSRAM as its mask is: internal RAM holds only the short list of lengths, so
// it does not shrink as the headings come (one small allocation an entry, some 260 bytes with the map's, used it up).
template<typename Extra> class Cache {
 public:
  struct Entry {
    Mask mask;
    Extra extra;  // what the drawing side keeps beside it (an LVGL image descriptor)
  };
  static constexpr int STEPS = 360 / HEADING_STEP;
  Cache() = default;
  Cache(const Cache &) = delete;
  Cache &operator=(const Cache &) = delete;
  ~Cache() {
    for (auto &t : tables_) {
      for (int i = 0; i < STEPS; ++i)
        if (t.steps[i]) {
          t.steps[i]->~Entry();
          release(t.steps[i]);
        }
      release(t.steps);
    }
  }
  // The mask of `length` and `heading`. At most `budget` masks are made by the calls of one `frame`: past that, a mask
  // not made yet is the nearest heading of that length already made (for a frame or two), so no frame waits for many.
  Entry *get(int length, int heading, uint32_t frame = 0, int budget = 1 << 30) {
    const int step = heading_step(heading);
    Entry **steps = table(length);
    if (!steps) return nullptr;
    if (steps[step]) return steps[step];
    if (frame != frame_) {
      frame_ = frame;
      made_ = 0;
    }
    if (made_ >= budget) {
      for (int d = 1; d <= STEPS / 2; ++d)
        for (const int near : {(step + d) % STEPS, (step - d + STEPS) % STEPS})
          if (steps[near]) return steps[near];
      // None of this length yet: this one is made all the same.
    }
    ++made_;
    Mask mask = render((float) length, step);
    if (mask.size == 0) return nullptr;  // no memory for it now: nothing drawn, asked again next frame
    void *room = scratch(sizeof(Entry));
    if (!room) return nullptr;
    Entry *entry = new (room) Entry();
    entry->mask = std::move(mask);
    steps[step] = entry;
    ++count_;
    return entry;
  }
  size_t size() const { return count_; }

 private:
  struct Table {
    int length;
    Entry **steps;  // STEPS of them, in PSRAM
  };
  Entry **table(int length) {
    for (auto &t : tables_)
      if (t.length == length) return t.steps;
    auto **steps = static_cast<Entry **>(scratch(sizeof(Entry *) * STEPS));
    if (!steps) return nullptr;
    std::memset(steps, 0, sizeof(Entry *) * STEPS);
    tables_.push_back({length, steps});
    return steps;
  }
  std::vector<Table> tables_;
  uint32_t frame_ = 0;
  int made_ = 0;
  size_t count_ = 0;
};
}  // namespace sky::sprites
