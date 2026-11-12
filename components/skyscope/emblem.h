#pragma once
// The screen's emblem: a radar scope with a propeller for its beam. Three blades turn about the scope's middle, and
// the leading one is the beam: its afterglow trails behind it out to the rim, and a plane it has just passed lights up.
// The splash at the start draws it turning (splash.cpp); tools/emblem.cpp draws it still, for the docs and the page.
//
// Everything is worked out here from a handful of sizes given as shares of the scope's radius, so the emblem is the
// same at any size, on the glass and in the SVG. Plain C++, RGB565 out, so the tests draw it on a PC.
//
// Turning it must be cheap: a frame of the P4's glass is half a million pixels, and every pixel read or written there
// costs about as much whatever is done with it. So what never moves (rings, ticks) is drawn once into a picture of its
// own, a byte a pixel, and a frame touches only the slice of the scope the afterglow was in and is in now, and the
// rows under the three blades, which are one small picture of a blade turned into place. How far a pixel is behind
// the beam is a subtraction from its bearing, kept from the start.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "psram.h"

namespace sky::emblem {

constexpr float PI = 3.14159265f;

// ---- Its sizes, as shares of the scope's radius.
constexpr float RIM = 0.965f, RIM_STROKE = 0.018f;        // the outer ring
constexpr float RIM_BLOOM = 0.05f, RIM_BLOOM_LIGHT = 0.3f;  // its light spilling either side, and how far
constexpr float RINGS[] = {0.32f, 0.64f};                 // the range rings inside it
constexpr float RING_STROKE = 0.009f, RING_LIGHT = 0.42f;  // their line, and how bright beside the rim
// Ticks in from the rim every 5 degrees, longer every 10 and longest every 30.
constexpr float TICK = 0.028f, TICK_10 = 0.05f, TICK_30 = 0.088f, TICK_STROKE = 0.01f;
// A faint light in the scope's middle, as a tube has, gone by HAZE_TO.
constexpr float HAZE = 0.07f, HAZE_TO = 0.8f;
// A blade lies along its arm from BLADE_ROOT (under the hub) to BLADE_TIP: slim at its root, broadest two thirds
// out, round at its tip, and swept: its middle leans the way it turns and its tip trails, as a real one's does, so
// the eye reads which way it is going. Its leading edge is lit and its trailing edge in shade; a band near the tip is
// the blips' color, as a propeller's tips are painted to be seen.
constexpr int BLADES = 3;
constexpr float BLADE_ROOT = 0.05f, BLADE_TIP = 0.585f, BLADE_LONG = BLADE_TIP - BLADE_ROOT;
constexpr float BLADE_WIDE = 0.074f, BLADE_SWEEP = 0.034f;
// How much of its breadth lies ahead of its middle, and behind.
constexpr float BLADE_LEAD = 1.12f, BLADE_TRAIL = 0.88f;
constexpr float BAND_FROM = 0.845f, BAND_TO = 0.905f;     // the painted band, along the blade (0 root, 1 tip)
// The hub: a dome lit from the upper left, a fine line around its cap.
constexpr float HUB = 0.1f, HUB_CAP = 0.56f, HUB_LINE = 0.07f;
// The beam: a line on from the leading blade's tip to the rim, and its afterglow fading over GLOW_DEGREES behind it.
constexpr float BEAM_STROKE = 0.012f, GLOW_DEGREES = 72, GLOW_MOST = 0.56f, GLOW_FROM = HUB + 0.01f;
constexpr float GLOW_TO = RIM - RIM_STROKE / 2;
// The planes the beam finds: each a small plane at a bearing and a distance, flying on a heading, `size` from its
// middle to its nose; lit as the beam passes and fading after.
struct Blip {
  float bearing, distance, size, heading;
};
constexpr Blip BLIPS[] = {{16, 0.79f, 0.058f, 305}, {252, 0.47f, 0.046f, 70}, {300, 0.78f, 0.046f, 150}};
constexpr float BLIP_FADE_DEGREES = 300, BLIP_HALO = 2.1f;  // its halo's radius, in its size
// A plane's parts, in its size: lines with round ends from (x, y) to (x2, y2), `r` thick; its nose is at y = -1. The
// wings and the tail are drawn on both sides.
struct Limb {
  float x, y, x2, y2, r;
  bool both;
};
constexpr Limb PLANE[] = {
    {0, -1, 0, 0.95f, 0.15f, false}, {0, -0.08f, 0.98f, 0.42f, 0.115f, true}, {0, 0.76f, 0.4f, 0.98f, 0.085f, true}};
// Where the leading blade rests once it has stopped: the emblem as it is printed.
constexpr float REST_DEGREES = 38;

struct Colors {
  uint32_t ground = 0x000000;
  uint32_t scope = 0x33B5FF;  // rings, ticks and the afterglow
  uint32_t blade = 0xF2F4F7;  // the propeller where the light falls on it, and the beam
  uint32_t shade = 0x5E7388;  // and where it does not
  uint32_t blip = 0xFFB000;
};

inline uint16_t rgb565(uint32_t c) { return (uint16_t) ((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)); }
// `alpha` (0 to 256) of `over` on `under`.
inline uint16_t blend(uint16_t under, uint16_t over, uint32_t alpha) {
  const uint32_t r = ((under >> 11) * (256 - alpha) + (over >> 11) * alpha) >> 8;
  const uint32_t g = ((under >> 5 & 0x3F) * (256 - alpha) + (over >> 5 & 0x3F) * alpha) >> 8;
  const uint32_t b = ((under & 0x1F) * (256 - alpha) + (over & 0x1F) * alpha) >> 8;
  return (uint16_t) (r << 11 | g << 5 | b);
}
// How far (dx, dy) is from the middle: a plain square root, without the care std::hypot takes over sizes that never
// come up here.
inline float far(float dx, float dy) { return std::sqrt(dx * dx + dy * dy); }
// How much of a pixel a shape covers, from how far the pixel's middle is outside its edge (`distance`, negative
// inside), both in the same unit as `pixel`, a pixel's size.
inline float cover(float distance, float pixel) { return std::clamp(0.5f - distance / pixel, 0.0f, 1.0f); }

inline float smooth(float from, float to, float x) {
  const float t = std::clamp((x - from) / (to - from), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// ---- The blade's own shape, lying along +x from the hub, +y the way it turns: used for its picture here and its
// outline in the SVG. `t` is how far along it, 0 at the root and 1 at the tip.
inline float blade_half_wide(float t) {
  const float round = t < 0.8f ? 1.0f : std::sqrt(std::max(0.0f, 1 - (t - 0.8f) * (t - 0.8f) * 25));
  return BLADE_WIDE * (0.4f + 0.6f * smooth(0, 0.64f, t)) * round;
}
inline float blade_middle(float t) { return BLADE_SWEEP * 4 * t * (1 - t); }
inline float blade_lead(float t) { return blade_middle(t) + BLADE_LEAD * blade_half_wide(t); }
inline float blade_trail(float t) { return blade_middle(t) - BLADE_TRAIL * blade_half_wide(t); }
inline bool blade_holds(float x, float y) {
  const float t = (x - BLADE_ROOT) / BLADE_LONG;
  return t >= 0 && t <= 1 && y <= blade_lead(t) && y >= blade_trail(t);
}
// How lit the blade is there, 0 to 1: brightest along its leading edge, and darker in under the hub.
inline float blade_lit(float x, float y) {
  const float t = std::clamp((x - BLADE_ROOT) / BLADE_LONG, 0.0f, 1.0f);
  const float lead = blade_lead(t), trail = blade_trail(t);
  const float across = std::clamp((y - trail) / std::max(1e-4f, lead - trail), 0.0f, 1.0f);
  return (0.18f + 0.82f * std::pow(across, 1.25f)) * (0.55f + 0.45f * smooth(0.12f, 0.4f, t));
}
inline bool blade_band(float x) {
  const float t = (x - BLADE_ROOT) / BLADE_LONG;
  return t >= BAND_FROM && t <= BAND_TO;
}

// How lit the hub's dome is at (x, y) from its middle, in its radius: 0 to 1.
inline float hub_lit(float x, float y) {
  const float r2 = std::min(1.0f, x * x + y * y), r = std::sqrt(r2);
  const float toward = -0.42f * x - 0.52f * y + 0.74f * std::sqrt(1 - r2);  // the light, from the upper left
  const float line = 1 - 0.5f * std::clamp(1 - std::fabs(r - HUB_CAP) / HUB_LINE, 0.0f, 1.0f);
  return std::clamp(0.2f + 0.8f * toward, 0.0f, 1.0f) * line;
}

// How far (x, y) is from a line from (ax, ay) to (bx, by).
inline float from_line(float x, float y, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
  const float t = std::clamp((wx * vx + wy * vy) / std::max(1e-9f, vx * vx + vy * vy), 0.0f, 1.0f);
  return far(wx - t * vx, wy - t * vy);
}
// How far (x, y), in a plane's size from its middle with its nose up, is outside the plane; negative inside.
inline float plane_distance(float x, float y) {
  float d = 1e9f;
  x = std::fabs(x);
  for (const Limb &l : PLANE) d = std::min(d, from_line(x, y, l.x, l.y, l.x2, l.y2) - l.r);
  return d;
}

// How bright the afterglow is `behind` degrees after the beam: brightest at the beam, gone at GLOW_DEGREES.
inline float glow_at(float behind) {
  if (behind < 0 || behind >= GLOW_DEGREES) return 0;
  const float left = 1 - behind / GLOW_DEGREES;
  return GLOW_MOST * left * left * (0.35f + 0.65f * left);
}

// How bright a blip is once the beam is `behind` degrees past it (0 to 360): lit at once, fading as the beam goes on.
inline float blip_light(float behind) {
  return behind >= BLIP_FADE_DEGREES ? 0.0f : std::pow(1 - behind / BLIP_FADE_DEGREES, 1.6f);
}

// A part of the picture a frame changed, for the glass to send again.
struct Box {
  int x, y, w, h;
};

// The arc tangent of `t` (0 or more) in degrees, within a hundredth of one: no library call a pixel.
inline float atan_degrees(float t) {
  const bool steep = t > 1;
  const float u = steep ? 1 / t : t, u2 = u * u;
  const float a =
      u * (0.9998660f + u2 * (-0.3302995f + u2 * (0.1801410f + u2 * (-0.0851330f + u2 * 0.0208351f)))) * 57.29578f;
  return steep ? 90 - a : a;
}

// The bearing of (dx, dy) from the middle, 0 to 360: north (up the glass) is 0, clockwise.
inline float bearing_of(float dx, float dy) {
  const float ax = std::fabs(dx), ay = std::fabs(dy);
  if (ax == 0 && ay == 0) return 0;
  const float a = atan_degrees(ax / std::max(ay, 1e-6f));  // from the vertical, 0 to 90
  const float degrees = dy < 0 ? (dx >= 0 ? a : 360 - a) : (dx >= 0 ? 180 - a : 180 + a);
  return degrees >= 360 ? 0 : degrees;
}

// The emblem at one size, drawn turned any way, frame after frame into the same pixels.
class Emblem {
 public:
  // Bytes prepare() takes for a square `size` pixels a side, with a scope `radius` pixels: so a screen short of memory
  // can go without.
  static size_t bytes(int size, float radius) {
    return (size_t) size * size * 3 / 2 + (size_t) sprite_wide(radius) * sprite_tall(radius) * 4 + 64 * 1024;
  }

  // The emblem `size` pixels square, its scope's `radius` pixels about the square's middle.
  void prepare(int size, float radius, const Colors &colors = {}) {
    size &= ~1;  // even, so the scope's quarters are each other's mirror
    size_ = size;
    radius_ = radius;
    colors_ = colors;
    last_out_ = nullptr;
    const float middle = size / 2.0f, unit = 1 / radius;  // a pixel, as a share of the radius
    half_ = (size + 1) / 2;
    base_.resize((size_t) size * size);
    bearing_.assign((size_t) half_ * half_, 0);
    // The scope's color at each of its 256 lights on the ground: a byte of the scope's picture to a pixel.
    const uint16_t ground = rgb565(colors.ground), lit = rgb565(colors.scope);
    for (int i = 0; i < 256; ++i) ramp_[i] = blend(ground, lit, (uint32_t) (i + (i >> 7)));
    // And the propeller's, from its shade to its light.
    for (int i = 0; i < 256; ++i)
      blade_ramp_[i] = blend(rgb565(colors.shade), rgb565(colors.blade), (uint32_t) (i + (i >> 7)));
    // Each two-by-two pixels' bearing from the middle, as a compass has it (north up, clockwise), a whole turn 65536.
    for (int y = 0; y < half_; ++y)
      for (int x = 0; x < half_; ++x)
        bearing_[(size_t) y * half_ + x] =
            (uint16_t) (int32_t) (bearing_of(2 * x + 1 - middle, 2 * y + 1 - middle) * (65536.0f / 360));
    // The scope is the same in each quarter, mirrored (its ticks every 5 degrees too), so one quarter is worked out
    // and put down four times; and everything in it but the ticks depends only on how far a pixel is from the
    // middle, so that is worked out once for every sixteenth of a pixel out. Making it is then quick enough not to
    // hold up the start.
    constexpr int STEPS = 16;
    const int reach = (int) std::ceil(size * 0.75f) * STEPS;
    std::vector<uint8_t> soft((size_t) reach), stroke((size_t) reach);
    for (int i = 0; i < reach; ++i) {
      const float r = (i + 0.5f) / STEPS * unit;
      float glow = r < HAZE_TO ? HAZE * (1 - r / HAZE_TO) * (1 - r / HAZE_TO) : 0.0f;
      const float off_rim = std::fabs(r - RIM) / RIM_BLOOM;
      glow += RIM_BLOOM_LIGHT * std::exp(-off_rim * 1.6f) * (r > RIM ? std::max(0.0f, 1 - off_rim / 2.4f) : 1.0f);
      soft[i] = (uint8_t) std::lround(std::min(1.0f, glow) * 255);
      float line = cover(std::fabs(r - RIM) - RIM_STROKE / 2, unit);
      for (const float ring : RINGS)
        line = std::max(line, RING_LIGHT * cover(std::fabs(r - ring) - RING_STROKE / 2, unit));
      stroke[i] = (uint8_t) std::lround(line * 255);
    }
    const int half = size / 2;  // size is even: the middle lies between four pixels
    std::vector<uint8_t> row_soft((size_t) half), row_stroke((size_t) half);
    for (int y = 0; y < half; ++y) {
      const float dy = y + 0.5f - middle;
      for (int i = 0; i < half; ++i) {
        const float dx = i + 0.5f;  // the pixel `half + i` of the row, right of the middle
        const float d = std::sqrt(dx * dx + dy * dy), r = d * unit;
        const int at = std::min(reach - 1, (int) (d * STEPS));
        row_soft[i] = soft[at];
        float line = stroke[at] / 255.0f;
        if (r > RIM - TICK_30 - unit && r < RIM) {
          const float degrees = bearing_of(dx, dy);
          const float nearest = std::round(degrees / 5) * 5;
          const int tick = (int) std::lround(nearest) % 360;
          const float length = tick % 30 == 0 ? TICK_30 : tick % 10 == 0 ? TICK_10 : TICK;
          const float off = std::fabs(degrees - nearest) * PI / 180 * r;  // across the tick
          const float in = std::max(off - TICK_STROKE / 2, (RIM - length) - r);
          line = std::max(line, (tick % 30 == 0 ? 1.0f : tick % 10 == 0 ? 0.72f : 0.45f) * cover(in, unit));
        }
        row_stroke[i] = (uint8_t) std::lround(line * 255);
      }
      // The four mirrored pixels. The soft lights are put down a little more or less by a fixed pattern: the glass
      // has few shades of a dim color, which would show as steps.
      for (const int row : {y, size - 1 - y}) {
        uint8_t *to = &base_[(size_t) row * size];
        for (int i = 0; i < half; ++i)
          for (const int x : {half + i, half - 1 - i}) {
            const int glow = row_soft[i] ? std::max(0, row_soft[i] + (DITHER[row & 3][x & 3] * 7 - 52) / 16) : 0;
            to[x] = (uint8_t) std::max<int>(std::min(255, glow), row_stroke[i]);
          }
      }
    }
    // One blade lying along its arm, a pixel of it a pixel of the glass: small enough to stay in the chip's cache
    // while it is turned into place three times a frame. Each pixel of it: how much the blade covers (nine looks at
    // it), how lit it is there, and how much of it is the painted band.
    sprite_w_ = sprite_wide(radius);
    sprite_h_ = sprite_tall(radius);
    sprite_.assign((size_t) sprite_w_ * sprite_h_ * 4, 0);
    for (int j = 0; j < sprite_h_; ++j)
      for (int i = 0; i < sprite_w_; ++i) {
        int held = 0, band = 0;
        for (int v = 0; v < 3; ++v)
          for (int u = 0; u < 3; ++u) {
            const float x = SPRITE_FROM + (i + (u + 0.5f) / 3) * unit, y = SPRITE_TOP + (j + (v + 0.5f) / 3) * unit;
            if (!blade_holds(x, y)) continue;
            ++held;
            band += blade_band(x);
          }
        uint8_t *texel = &sprite_[((size_t) j * sprite_w_ + i) * 4];
        texel[0] = (uint8_t) (held * 255 / 9);
        texel[1] = (uint8_t) (blade_lit(SPRITE_FROM + (i + 0.5f) * unit, SPRITE_TOP + (j + 0.5f) * unit) * 255);
        texel[2] = (uint8_t) (held ? band * 255 / held : 0);
      }
    for (int i = 0; i < 256; ++i) glow_[i] = (uint8_t) (glow_at(i * 90.0f / 256) * 256);
    // Each plane the beam finds, with its halo: how much of each pixel around it is its color when it is lit most.
    for (size_t i = 0; i < BLIP_COUNT; ++i) {
      const Blip &b = BLIPS[i];
      const float a = b.bearing * PI / 180, halo = b.size * BLIP_HALO * radius + 1;
      const float cx = middle + std::sin(a) * b.distance * radius, cy = middle - std::cos(a) * b.distance * radius;
      const Box box = clipped((int) std::floor(cx - halo), (int) std::floor(cy - halo), (int) std::ceil(cx + halo),
                              (int) std::ceil(cy + halo));
      blip_boxes_[i] = box;
      blip_masks_[i].assign((size_t) box.w * box.h, 0);
      const float h = b.heading * PI / 180, hs = std::sin(h), hc = std::cos(h), pixel = unit / b.size;
      for (int y = box.y; y < box.y + box.h; ++y)
        for (int x = box.x; x < box.x + box.w; ++x) {
          // The pixel in the plane's own size, its nose up.
          const float dx = (x + 0.5f - cx) * pixel, dy = (y + 0.5f - cy) * pixel;
          const float px = dx * hc + dy * hs, py = -dx * hs + dy * hc;
          const float fringe = std::max(0.0f, 1 - far(px, py) / BLIP_HALO);
          const float alpha = std::max(cover(plane_distance(px, py), pixel), fringe * fringe * 0.3f);
          blip_masks_[i][(size_t) (y - box.y) * box.w + (x - box.x)] = (uint8_t) (alpha * 255 + 0.5f);
        }
    }
    // The hub: how much of each pixel it covers, and how lit its dome is there.
    const int hub = (int) std::ceil(HUB * radius) + 1;
    hub_box_ = clipped((int) middle - hub, (int) middle - hub, (int) middle + hub, (int) middle + hub);
    hub_.assign((size_t) hub_box_.w * hub_box_.h * 2, 0);
    for (int y = hub_box_.y; y < hub_box_.y + hub_box_.h; ++y)
      for (int x = hub_box_.x; x < hub_box_.x + hub_box_.w; ++x) {
        const float dx = (x + 0.5f - middle) * unit, dy = (y + 0.5f - middle) * unit;
        uint8_t *at = &hub_[((size_t) (y - hub_box_.y) * hub_box_.w + (x - hub_box_.x)) * 2];
        at[0] = (uint8_t) (cover(far(dx, dy) - HUB, unit) * 255 + 0.5f);
        at[1] = (uint8_t) (hub_lit(dx / HUB, dy / HUB) * 255);
      }
  }

  int size() const { return size_; }
  bool ready() const { return size_ > 0; }
  // The memory let go.
  void release() {
    size_ = 0;
    Bytes().swap(base_);
    Words().swap(bearing_);
    Bytes().swap(sprite_);
    Bytes().swap(hub_);
    for (Bytes &mask : blip_masks_) Bytes().swap(mask);
  }

  // The emblem into `out` (size x size pixels, `stride` pixels a row), its leading blade at the bearing `degrees`,
  // having turned `turned` degrees in all since it began (which lights the blips it has passed), the glow, the beam
  // and the blips at `light` of their own (0 to 1).
  //
  // The first frame into `out` draws all of it. Every later one puts back only where the blades and the afterglow
  // were, and draws them where they are now; `changed` (if given) gets the parts of the picture that differ.
  void draw(uint16_t *out, int stride, float degrees, float turned, float light = 1,
            std::vector<Box> *changed = nullptr) {
    if (!ready()) return;
    if (changed) changed->clear();
    const bool whole = out != last_out_ || stride != last_stride_;
    const float middle = size_ / 2.0f;
    const uint16_t scope = rgb565(colors_.scope), blade = rgb565(colors_.blade);
    const uint32_t strength = (uint32_t) (std::clamp(light, 0.0f, 1.0f) * 256);
    if (whole) {
      copy_base(out, stride, {0, 0, size_, size_});
      if (changed) changed->push_back({0, 0, size_, size_});
    }
    // What a frame changes is gathered a band of rows at a time, each band a box no wider than its rows need: one box
    // around a blade lying aslant, or around the afterglow, would be mostly pixels that did not change.
    Bands bands{whole ? nullptr : changed, std::max(16, size_ / 8)};
    auto put_back = [&](const Box &b) {
      copy_base(out, stride, b);
      if (changed) changed->push_back(b);
    };
    if (!whole) {
      // Where the blades, the hub and the blips were: the scope alone again.
      for (int k = 0; k < BLADES; ++k) {
        const BladeAt at = blade_at(was_ + k * 360.0f / BLADES);
        for (int y = at.box.y; y < at.box.y + at.box.h; ++y) {
          const Span s = blade_span(at, y);
          if (s.x0 > s.x1) continue;
          copy_base(out, stride, {s.x0, y, s.x1 - s.x0 + 1, 1});
          bands.add(y, s.x0, s.x1);
        }
        bands.close();
      }
      put_back(hub_box_);
      for (const Box &b : blip_boxes_) put_back(b);
    }

    // ---- The afterglow and the beam, over the part of the scope they were in and are in now: from the end of the
    // old trail around to just ahead of the beam. Each row of that slice lies between two straight edges and the
    // rim, so its ends are worked out, and nothing outside it is read or written.
    float moved = whole ? 360.0f : std::fmod(degrees - was_ + 720.0f, 360.0f);
    if (moved > 180) moved = 360;  // turned back, or a long way: all of it
    const float margin = 3;
    const float slice = GLOW_DEGREES + moved + 2 * margin;
    const bool all = whole || slice >= 170;
    const float to = (degrees + margin) * PI / 180, from = (degrees + margin - slice) * PI / 180;
    const float tx = std::sin(to), ty = -std::cos(to), fx = std::sin(from), fy = -std::cos(from);
    const float a = degrees * PI / 180, bx = std::sin(a), by = -std::cos(a);  // along the beam
    const float reach = GLOW_TO * radius_, reach2 = reach * reach;
    const float inner = GLOW_FROM * radius_, inner2 = inner * inner;
    const float beam_from = (BLADE_TIP - 0.03f) * radius_;
    const float beam_half = std::max(0.75f, BEAM_STROKE * radius_ / 2);
    // How far behind the beam a pixel is comes from its bearing, kept from the start for every two-by-two pixels: a
    // subtraction a pixel. The glow is smooth enough that the coarser bearing never shows, and its one sharp edge
    // lies under the beam's own line, which is worked out pixel by pixel.
    const uint16_t beam_at = (uint16_t) (int32_t) std::lround(degrees * (65536.0f / 360));
    for (int y = 0; y < size_; ++y) {
      const float dy = y + 0.5f - middle;
      if (dy * dy >= reach2) continue;
      const float span = std::sqrt(reach2 - dy * dy);
      float lo = -span, hi = span;
      if (!all) {
        // Clockwise of the slice's first edge, and not past its last.
        if (std::fabs(fy) < 1e-4f) {
          if (fx * dy < 0) continue;
        } else if (fy > 0) {
          hi = std::min(hi, fx * dy / fy);
        } else {
          lo = std::max(lo, fx * dy / fy);
        }
        if (std::fabs(ty) < 1e-4f) {
          if (tx * dy > 0) continue;
        } else if (ty > 0) {
          lo = std::max(lo, tx * dy / ty);
        } else {
          hi = std::min(hi, tx * dy / ty);
        }
      }
      const int x0 = std::max(0, (int) std::floor(middle + lo)), x1 = std::min(size_ - 1, (int) std::ceil(middle + hi));
      if (x0 > x1) continue;
      bands.add(y, x0, x1);
      // The pixels of this row too near the middle to glow: the hub's.
      const float hub = dy * dy < inner2 ? std::sqrt(inner2 - dy * dy) : -1;
      const int hub0 = hub < 0 ? size_ : (int) std::ceil(middle - hub);
      const int hub1 = hub < 0 ? -1 : (int) std::floor(middle + hub) - 1;
      const uint8_t *base = &base_[(size_t) y * size_];
      const uint16_t *bearing = &bearing_[(size_t) (y >> 1) * half_];
      uint16_t *row = out + (size_t) y * stride;
      const uint8_t *dither = DITHER_HALF[y & 3];
      // The glow: whole numbers only, a pixel. The chip's floating point is slow enough that a multiplication or two a
      // pixel here cost more than everything else in the frame.
      for (int x = x0; x <= x1; ++x) {
        uint16_t pixel = ramp_[base[x]];
        if (x < hub0 || x > hub1) {
          const uint16_t behind = (uint16_t) (beam_at - bearing[x >> 1]);  // a turn's 65536ths after the beam
          if (behind < 16384) {
            uint32_t glow = glow_[behind >> 6] * strength >> 8;
            if (glow) {
              // A ring or a tick the beam has just passed glows more than the dark around it, as on a tube; and the
              // glow is put down by the fixed pattern too, so that its fading shows no steps.
              glow += (glow * base[x] >> 6) + dither[x & 3];
              pixel = blend(pixel, scope, std::min<uint32_t>(glow, 256));
            }
          }
        }
        row[x] = pixel;
      }
      // The beam: a line on from the blade's tip, a pixel's smooth edge either side. Only the few pixels of the row
      // that lie across it are looked at: how far each is across the beam, and along it, from where it is.
      const float along_row = dy * by, across_row = -bx * dy, wide = beam_half + 0.5f;
      float first = (float) x0, last = (float) x1;
      if (std::fabs(by) > 1e-4f) {
        const float p = (-wide - across_row) / by + middle - 0.5f, q = (wide - across_row) / by + middle - 0.5f;
        first = std::max(first, std::floor(std::min(p, q)));
        last = std::min(last, std::ceil(std::max(p, q)));
      } else if (std::fabs(across_row) >= wide) {
        last = first - 1;
      }
      for (int x = (int) first; x <= (int) last; ++x) {
        if (x >= hub0 && x <= hub1) continue;
        const float dx = x + 0.5f - middle, along = dx * bx + along_row, across = by * dx + across_row;
        const float off = std::fabs(across) - beam_half;
        if (off < 0.5f && along >= beam_from)
          row[x] = blend(row[x], blade, (uint32_t) (std::min(1.0f, 0.5f - off) * strength));
      }
    }
    bands.close();

    // ---- The blips, each lit by how long ago the beam passed it; before its first pass it is dark.
    const uint16_t blip_color = rgb565(colors_.blip);
    for (size_t i = 0; i < BLIP_COUNT; ++i) {
      const Blip &b = BLIPS[i];
      float behind = std::fmod(degrees - b.bearing, 360.0f);
      if (behind < 0) behind += 360;
      if (turned < behind) continue;
      const float lit = blip_light(behind) * light;
      if (lit <= 0.01f) continue;
      const uint32_t most = (uint32_t) (std::min(1.0f, lit) * 256);
      const Box &box = blip_boxes_[i];
      const uint8_t *mask = blip_masks_[i].data();
      for (int y = box.y; y < box.y + box.h; ++y) {
        uint16_t *row = out + (size_t) y * stride + box.x;
        for (int x = 0; x < box.w; ++x, ++mask)
          if (*mask) row[x] = blend(row[x], blip_color, *mask * most >> 8);
      }
    }

    // ---- The blades: the one picture turned to each arm, a glass pixel taking the four of the picture's nearest it.
    for (int k = 0; k < BLADES; ++k) {
      const BladeAt at = blade_at(degrees + k * 360.0f / BLADES);
      const int32_t most_u = (sprite_w_ - 1) << 16, most_v = (sprite_h_ - 1) << 16;
      const size_t down = (size_t) sprite_w_ * 4;
      const uint16_t band_color = rgb565(colors_.blip);
      for (int y = at.box.y; y < at.box.y + at.box.h; ++y) {
        const Span s = blade_span(at, y);
        if (s.x0 > s.x1) continue;
        bands.add(y, s.x0, s.x1);
        // The picture's column and row under each glass pixel, in 65536ths, stepped along the row.
        int32_t u = s.u, v = s.v;
        uint16_t *row = out + (size_t) y * stride;
        for (int x = s.x0; x <= s.x1; ++x, u += at.step_u, v += at.step_v) {
          if (u < 0 || v < 0 || u >= most_u || v >= most_v) continue;
          const uint8_t *p = &sprite_[((size_t) (v >> 16) * sprite_w_ + (u >> 16)) * 4], *q = p + down;
          const uint32_t fu = (uint32_t) (u >> 8 & 0xFF), fv = (uint32_t) (v >> 8 & 0xFF);
          const uint32_t w00 = (256 - fu) * (256 - fv), w10 = fu * (256 - fv), w01 = (256 - fu) * fv, w11 = fu * fv;
          const uint32_t alpha = (p[0] * w00 + p[4] * w10 + q[0] * w01 + q[4] * w11) >> 16;
          if (!alpha) continue;
          uint16_t color = blade_ramp_[(p[1] * w00 + p[5] * w10 + q[1] * w01 + q[5] * w11) >> 16];
          const uint32_t band = (p[2] * w00 + p[6] * w10 + q[2] * w01 + q[6] * w11) >> 16;
          if (band) color = blend(color, band_color, band * 215 >> 8);
          row[x] = blend(row[x], color, alpha + (alpha >> 7));
        }
      }
      bands.close();
    }
    // ---- The hub over the blades' roots.
    const uint8_t *hub = hub_.data();
    for (int y = hub_box_.y; y < hub_box_.y + hub_box_.h; ++y) {
      uint16_t *row = out + (size_t) y * stride + hub_box_.x;
      for (int x = 0; x < hub_box_.w; ++x, hub += 2)
        if (hub[0]) row[x] = blend(row[x], blade_ramp_[hub[1]], hub[0] + (hub[0] >> 7));
    }
    if (changed) join(*changed);
    was_ = degrees;
    last_out_ = out;
    last_stride_ = stride;
  }

 private:
  // The blade's picture: from a little inside its root to a little past its tip, and from its trailing edge to its
  // leading one where it leans farthest.
  static constexpr float SPRITE_FROM = BLADE_ROOT - 0.012f;
  static constexpr float SPRITE_TOP = -(BLADE_TRAIL * BLADE_WIDE + 0.012f);
  static constexpr float SPRITE_BOTTOM = BLADE_SWEEP + BLADE_LEAD * BLADE_WIDE + 0.012f;
  // The fixed pattern that dim lights are put down by (0 to 15), and half of it.
  static constexpr uint8_t DITHER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  static constexpr uint8_t DITHER_HALF[4][4] = {{0, 4, 1, 5}, {6, 2, 7, 3}, {1, 5, 0, 4}, {7, 3, 6, 2}};
  static constexpr size_t BLIP_COUNT = sizeof(BLIPS) / sizeof(BLIPS[0]);
  static int sprite_wide(float radius) { return (int) std::ceil((BLADE_TIP + 0.02f - SPRITE_FROM) * radius) + 2; }
  static int sprite_tall(float radius) { return (int) std::ceil((SPRITE_BOTTOM - SPRITE_TOP) * radius) + 2; }
  // The scope alone into `b` of `out`.
  void copy_base(uint16_t *out, int stride, const Box &b) const {
    for (int y = b.y; y < b.y + b.h; ++y) {
      const uint8_t *from = &base_[(size_t) y * size_ + b.x];
      uint16_t *to = out + (size_t) y * stride + b.x;
      for (int x = 0; x < b.w; ++x) to[x] = ramp_[from[x]];
    }
  }
  // The box from (x0, y0) to (x1, y1), both in it, cut to the picture.
  Box clipped(int x0, int y0, int x1, int y1) const {
    x0 = std::max(0, x0), y0 = std::max(0, y0);
    x1 = std::min(size_ - 1, x1), y1 = std::min(size_ - 1, y1);
    return {x0, y0, std::max(0, x1 - x0 + 1), std::max(0, y1 - y0 + 1)};
  }

  // Boxes that lie mostly over one another made one (a blade where it was and where it is now), and then the
  // nearest to one another, until there are few enough that the glass's own list of what to draw again holds them
  // all: LVGL keeps 32, and past that draws the whole glass.
  static constexpr size_t MOST_BOXES = 20;
  static void join(std::vector<Box> &boxes) {
    auto area = [](const Box &b) { return (int64_t) b.w * b.h; };
    auto around = [](const Box &a, const Box &b) {
      const int x = std::min(a.x, b.x), y = std::min(a.y, b.y);
      return Box{x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
    };
    boxes.erase(std::remove_if(boxes.begin(), boxes.end(), [](const Box &b) { return b.w <= 0 || b.h <= 0; }),
                boxes.end());
    while (boxes.size() > 1) {
      // The two whose joining adds the fewest pixels that neither held.
      size_t best_i = 0, best_j = 1;
      int64_t best = INT64_MAX;
      for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j) {
          const int64_t more = area(around(boxes[i], boxes[j])) - area(boxes[i]) - area(boxes[j]);
          if (more < best) best = more, best_i = i, best_j = j;
        }
      // Overlapping enough that one box is fewer pixels than the two, or still too many boxes.
      if (best > 0 && boxes.size() <= MOST_BOXES) break;
      boxes[best_i] = around(boxes[best_i], boxes[best_j]);
      boxes.erase(boxes.begin() + (long) best_j);
    }
  }

  // Rows' changed pixels gathered into boxes, a band of rows each.
  struct Bands {
    std::vector<Box> *out;
    int band;
    int top = -1, bottom = -1, left = 0, right = -1;
    void add(int y, int x0, int x1) {
      if (!out) return;
      if (top >= 0 && y / band != top / band) close();
      if (top < 0) top = y, left = x0, right = x1;
      left = std::min(left, x0), right = std::max(right, x1), bottom = y;
    }
    void close() {
      if (out && top >= 0) out->push_back({left, top, right - left + 1, bottom - top + 1});
      top = -1;
    }
  };

  // The blade's picture turned so its arm is at a bearing: the rows it reaches, and how the picture's column (u) and
  // row (v) go with a glass pixel: u = ux * x + uy_row(y) and the like, in pixels of the picture.
  struct BladeAt {
    float ux, uy, vx, vy;
    int32_t step_u, step_v;
    Box box;
  };
  BladeAt blade_at(float degrees) const {
    const float arm = degrees * PI / 180, middle = size_ / 2.0f;
    BladeAt at;
    at.ux = std::sin(arm), at.uy = -std::cos(arm);  // along the arm
    at.vx = std::cos(arm), at.vy = std::sin(arm);   // across it, clockwise
    at.step_u = (int32_t) std::lround(at.ux * 65536);
    at.step_v = (int32_t) std::lround(at.vx * 65536);
    const float near = SPRITE_FROM * radius_, far = near + sprite_w_;
    const float up = SPRITE_TOP * radius_, down = up + sprite_h_;
    float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
    for (const float along : {near, far})
      for (const float across : {up, down}) {
        const float px = along * at.ux + across * at.vx, py = along * at.uy + across * at.vy;
        left = std::min(left, px), right = std::max(right, px), top = std::min(top, py), bottom = std::max(bottom, py);
      }
    at.box = clipped((int) std::floor(middle + left) - 1, (int) std::floor(middle + top) - 1,
                     (int) std::ceil(middle + right) + 1, (int) std::ceil(middle + bottom) + 1);
    return at;
  }
  // The pixels of row `y` under the turned picture, first to last (none: x0 > x1), and the picture's column and row
  // under the first, in 65536ths.
  struct Span {
    int x0, x1;
    int32_t u, v;
  };
  Span blade_span(const BladeAt &at, int y) const {
    const float middle = size_ / 2.0f, dy = y + 0.5f - middle;
    // u and v at x = 0, then a step of ux and vx a pixel.
    const float u0 = (0.5f - middle) * at.ux + dy * at.uy - SPRITE_FROM * radius_ - 0.5f;
    const float v0 = (0.5f - middle) * at.vx + dy * at.vy - SPRITE_TOP * radius_ - 0.5f;
    float lo = (float) at.box.x, hi = (float) (at.box.x + at.box.w - 1);
    // Where a value that starts at `start` and steps by `step` lies from 0 up to `most`.
    auto within = [&](float start, float step, float most) {
      if (std::fabs(step) < 1e-5f) {
        if (start < 0 || start > most) hi = lo - 1;
        return;
      }
      const float first = -start / step, last = (most - start) / step;
      lo = std::max(lo, std::min(first, last));
      hi = std::min(hi, std::max(first, last));
    };
    within(u0, at.ux, (float) (sprite_w_ - 1));
    within(v0, at.vx, (float) (sprite_h_ - 1));
    Span s;
    s.x0 = (int) std::ceil(lo);
    s.x1 = (int) std::floor(hi);
    s.u = (int32_t) std::lround((u0 + at.ux * s.x0) * 65536);
    s.v = (int32_t) std::lround((v0 + at.vx * s.x0) * 65536);
    return s;
  }

  int size_ = 0, half_ = 0, sprite_w_ = 0, sprite_h_ = 0;
  float radius_ = 0;
  Colors colors_;
  // In PSRAM on the screen: a megabyte and more on a large glass.
  using Words = std::vector<uint16_t, InPsram<uint16_t>>;
  using Bytes = std::vector<uint8_t, InPsram<uint8_t>>;
  Bytes base_;     // the scope with nothing turning in it: how lit each pixel is
  Words bearing_;  // the bearing from the middle of each two-by-two pixels
  Bytes sprite_;   // one blade: four bytes a pixel (how much of it, how lit, how much the band, one spare)
  Bytes hub_;      // the hub: two bytes a pixel (how much of it, how lit)
  uint16_t blade_ramp_[256] = {};
  uint8_t glow_[256] = {};
  uint16_t ramp_[256] = {};
  // The last frame: where it was drawn and the beam's bearing then. The hub's and the blips' boxes never move.
  const uint16_t *last_out_ = nullptr;
  int last_stride_ = 0;
  float was_ = 0;
  Box hub_box_ = {};
  Box blip_boxes_[BLIP_COUNT] = {};
  Bytes blip_masks_[BLIP_COUNT];  // each plane and its halo, a byte a pixel of its box
};

// How the splash turns it: a sweep that slows evenly to rest at REST_DEGREES, TURN_DEGREES in SPIN_S seconds, slow
// enough that a glass drawing fifteen frames a second shows it turn smoothly, and far enough to pass every blip. The
// bearing of the leading blade `seconds` after it began, and how far it has turned by then.
constexpr float SPIN_S = 2.8f, TURN_DEGREES = 262;
inline float turned_at(float seconds) {
  const float left = 1 - std::clamp(seconds / SPIN_S, 0.0f, 1.0f);
  return TURN_DEGREES * (1 - left * left);
}
inline float bearing_at(float seconds) {
  float degrees = std::fmod(REST_DEGREES - TURN_DEGREES + turned_at(seconds), 360.0f);
  return degrees < 0 ? degrees + 360 : degrees;
}

}  // namespace sky::emblem
