#pragma once
// Touches without a finger, for tests on a real screen (POST /api/touch, probe.cpp): a gesture as the steps a finger
// takes (down here for so long, then there, then up), and a player that says where the finger is at any moment.
// probe.cpp hands that to LVGL as an input device of its own, so a scripted touch reaches the screen the way a
// finger's does: the same objects, events, long press and drag. Nothing here knows LVGL, so the tests run on a PC.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace sky::touch {

struct Step {
  int16_t x = 0, y = 0;
  bool pressed = false;
  uint32_t ms = 0;  // how long the finger stays so
};
using Script = std::vector<Step>;

// A tap: down for a moment, then up.
inline Script tap(int x, int y) {
  return {{(int16_t) x, (int16_t) y, true, 80}, {(int16_t) x, (int16_t) y, false, 60}};
}
// Down for `ms` (a long press is 400 ms or more), then up.
inline Script hold(int x, int y, uint32_t ms) {
  return {{(int16_t) x, (int16_t) y, true, ms}, {(int16_t) x, (int16_t) y, false, 60}};
}
// Down at the start, along to the end in `ms` (a point every 30 ms or so, as a touch panel reports), then up there.
inline Script drag(int x0, int y0, int x1, int y1, uint32_t ms) {
  Script s;
  s.push_back({(int16_t) x0, (int16_t) y0, true, 60});
  const int n = std::max(2, (int) (ms / 30));
  for (int i = 1; i <= n; ++i)
    s.push_back({(int16_t) (x0 + (x1 - x0) * i / n), (int16_t) (y0 + (y1 - y0) * i / n), true, ms / n});
  s.push_back({(int16_t) x1, (int16_t) y1, false, 60});
  return s;
}
inline uint32_t length(const Script &s) {
  uint32_t ms = 0;
  for (const Step &step : s) ms += step.ms;
  return ms;
}

// Plays a script: at any time after, where the finger is and whether it is down. Before a script and after one, up where
// it was last. It starts at the first read after it is given, not when it is given: on a loop slower than a tap (a busy
// sky, a map being drawn) a press could otherwise come and go between two reads, never seen. After that it goes by the
// clock, as a finger does: points of a drag a slow loop does not read are passed over.
class Player {
 public:
  void play(const Script &s) {
    script_ = s;
    started_ = false;
  }
  // Until a read finds it over.
  bool busy() const { return !script_.empty(); }
  Step at(uint32_t now) {
    if (script_.empty()) return last_;
    if (!started_) {
      started_ = true;
      start_ = now;
    }
    uint32_t t = now - start_;
    for (const Step &step : script_) {
      if (t < step.ms) return last_ = step;
      t -= step.ms;
    }
    last_ = script_.back();
    last_.pressed = false;
    script_.clear();
    return last_;
  }

 private:
  Script script_;
  bool started_ = false;
  uint32_t start_ = 0;
  Step last_{};
};

// A gesture as /api/touch names it: "tap", "hold" or "drag". "" for one it does not know.
inline Script gesture(const std::string &kind, int x, int y, int x1, int y1, uint32_t ms) {
  if (kind == "tap") return tap(x, y);
  if (kind == "hold") return hold(x, y, ms ? ms : 1200);
  if (kind == "drag") return drag(x, y, x1, y1, ms ? ms : 600);
  return {};
}
}  // namespace sky::touch
