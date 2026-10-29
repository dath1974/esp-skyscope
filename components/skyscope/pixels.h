#pragma once
// A decoded map tile laid into the frame (decode.cpp): at its own pixels, made smaller (the tile pixel under each frame
// pixel's middle), or enlarged smoothly (each frame pixel a blend of the four tile pixels nearest it), a pixel only
// partly opaque blended over what is already there; and lines straight into the frame (the airspace, airspace.h),
// thousands of them in a few milliseconds where LVGL took a second. RGB565 throughout; nothing here decodes, so the
// tests run on a PC.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace sky::pixels {

// A decoded tile: its colours and opacity (0 to 255), `stride` pixels a row, `width` x `height` of them used.
struct Tile {
  const uint16_t *colour;
  const uint8_t *alpha;
  int width, height, stride;
};

// `colour` over `under` at an opacity of `alpha` (0 to 255).
inline uint16_t blend(uint16_t under, uint16_t colour, uint32_t alpha) {
  if (alpha >= 255) return colour;
  if (alpha == 0) return under;
  const uint32_t a = alpha + (alpha >> 7);  // 0 to 256
  const uint32_t r = ((under >> 11) * (256 - a) + (colour >> 11) * a) >> 8;
  const uint32_t g = ((under >> 5 & 0x3F) * (256 - a) + (colour >> 5 & 0x3F) * a) >> 8;
  const uint32_t b = ((under & 0x1F) * (256 - a) + (colour & 0x1F) * a) >> 8;
  return (uint16_t) (r << 11 | g << 5 | b);
}

// 0xRRGGBB as RGB565.
inline uint16_t rgb565(uint32_t c) { return (uint16_t) ((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)); }

// line(), its ends within the reach of single precision; `r` half its width, `reach` how far its pixels go.
inline void line_near(uint16_t *frame, int fw, int fh, float x0, float y0, float x1, float y1, uint16_t colour, float r,
                      float reach) {
  if (std::max(x0, x1) < -reach || std::min(x0, x1) > fw - 1 + reach || std::max(y0, y1) < -reach ||
      std::min(y0, y1) > fh - 1 + reach)
    return;
  const float dx = x1 - x0, dy = y1 - y0, length2 = dx * dx + dy * dy, inverse = length2 > 0 ? 1 / length2 : 0;
  const bool steep = std::fabs(dy) > std::fabs(dx);
  float a0 = steep ? y0 : x0, a1 = steep ? y1 : x1, b0 = steep ? x0 : y0, b1 = steep ? x1 : y1;
  if (a0 > a1) {
    std::swap(a0, a1);
    std::swap(b0, b1);
  }
  const float slope = a1 > a0 ? (b1 - b0) / (a1 - a0) : 0;
  const float across = reach * std::sqrt(1 + slope * slope);  // half the span, along the shorter axis
  const int along_n = steep ? fh : fw, across_n = steep ? fw : fh;
  const int from = (int) std::max(0.0f, std::floor(a0 - reach));
  const int to = (int) std::min((float) (along_n - 1), std::ceil(a1 + reach));
  for (int m = from; m <= to; ++m) {
    const float c = b0 + (std::max(a0, std::min(a1, (float) m)) - a0) * slope;
    const int lo = (int) std::max(0.0f, std::floor(c - across));
    const int hi = (int) std::min((float) (across_n - 1), std::ceil(c + across));
    for (int n = lo; n <= hi; ++n) {
      const float px = (float) (steep ? n : m), py = (float) (steep ? m : n);
      float t = ((px - x0) * dx + (py - y0) * dy) * inverse;
      t = std::max(0.0f, std::min(1.0f, t));
      const float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
      const float cover = r + 0.5f - std::sqrt(ex * ex + ey * ey);
      if (cover <= 0) continue;
      uint16_t &out = frame[(int) py * fw + (int) px];
      out = blend(out, colour, cover >= 1 ? 255 : (uint32_t) (cover * 255 + 0.5f));
    }
  }
}

// A line from (x0, y0) to (x1, y1), `width` pixels across with round ends and smoothed edges, in `colour`, clipped to
// the frame. It is walked along its longer axis, a short span across it each step, each pixel covered by how far its
// middle is from the line. Single precision throughout, which the chip has hardware for; a line from far off is first
// cut to the frame, so its arithmetic stays in pixels the frame has.
inline void line(uint16_t *frame, int fw, int fh, double ax, double ay, double bx, double by, uint16_t colour,
                 float width) {
  const float r = std::max(0.5f, width / 2), reach = r + 1;
  constexpr double FAR = 16384;
  if (std::fabs(ax) < FAR && std::fabs(ay) < FAR && std::fabs(bx) < FAR && std::fabs(by) < FAR) {
    line_near(frame, fw, fh, (float) ax, (float) ay, (float) bx, (float) by, colour, r, reach);
    return;
  }
  // Liang and Barsky's clipping, to the frame and `reach` round it.
  double t0 = 0, t1 = 1;
  const double ddx = bx - ax, ddy = by - ay;
  for (const auto &[p, q] : {std::pair<double, double>{-ddx, ax + reach}, {ddx, fw - 1 + reach - ax},
                             {-ddy, ay + reach}, {ddy, fh - 1 + reach - ay}}) {
    if (p == 0) {
      if (q < 0) return;
      continue;
    }
    const double t = q / p;
    if (p < 0) t0 = std::max(t0, t);
    else t1 = std::min(t1, t);
    if (t0 > t1) return;
  }
  line_near(frame, fw, fh, (float) (ax + t0 * ddx), (float) (ay + t0 * ddy), (float) (ax + t1 * ddx),
            (float) (ay + t1 * ddy), colour, r, reach);
}

// Four RGB565 pixels mixed by weights summing to 256.
inline uint16_t mix4(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint32_t wa, uint32_t wb, uint32_t wc, uint32_t wd) {
  const uint32_t r = ((a >> 11) * wa + (b >> 11) * wb + (c >> 11) * wc + (d >> 11) * wd) >> 8;
  const uint32_t g = ((a >> 5 & 0x3F) * wa + (b >> 5 & 0x3F) * wb + (c >> 5 & 0x3F) * wc + (d >> 5 & 0x3F) * wd) >> 8;
  const uint32_t bl = ((a & 0x1F) * wa + (b & 0x1F) * wb + (c & 0x1F) * wc + (d & 0x1F) * wd) >> 8;
  return (uint16_t) (r << 11 | g << 5 | bl);
}

// The decoded tile into the frame at (ox, oy), `scale` frame pixels a tile pixel.
inline void lay(uint16_t *frame, int width, int height, int ox, int oy, double scale, const Tile &tile) {
  const int tw = tile.width, th = tile.height, stride = tile.stride;
  const uint16_t *colours = tile.colour;
  const uint8_t *alphas = tile.alpha;
  const int fw = (int) std::lround(tw * scale), fh = (int) std::lround(th * scale);
  const int x0 = std::max(0, ox), x1 = std::min(width, ox + fw);
  const int y0 = std::max(0, oy), y1 = std::min(height, oy + fh);
  if (x0 >= x1 || y0 >= y1) return;
  if (scale == 1.0) {
    for (int y = y0; y < y1; ++y) {
      uint16_t *out = frame + y * width;
      const uint16_t *c = colours + (y - oy) * stride;
      const uint8_t *a = alphas + (y - oy) * stride;
      for (int x = x0; x < x1; ++x) out[x] = blend(out[x], c[x - ox], a[x - ox]);
    }
    return;
  }
  if (scale < 1.0) {
    // Smaller: the tile pixel under each frame pixel's middle.
    const double step = 1.0 / scale;
    for (int y = y0; y < y1; ++y) {
      const int ty = std::min(th - 1, (int) ((y - oy + 0.5) * step));
      uint16_t *out = frame + y * width;
      for (int x = x0; x < x1; ++x) {
        const int tx = std::min(tw - 1, (int) ((x - ox + 0.5) * step));
        out[x] = blend(out[x], colours[ty * stride + tx], alphas[ty * stride + tx]);
      }
    }
    return;
  }
  // Larger: bilinear, in 1/256ths of a tile pixel. Past the tile's edge the edge pixel stands in for its neighbour's.
  const int32_t step = (int32_t) std::lround(256.0 / scale);
  for (int y = y0; y < y1; ++y) {
    const int32_t sy = (int32_t) std::lround(((y - oy) + 0.5) * 256.0 / scale) - 128;
    const int ya = std::max(0, std::min(th - 1, (int) (sy >> 8))), yb = std::max(0, std::min(th - 1, (int) (sy >> 8) + 1));
    const uint32_t fy = sy < 0 ? 0 : (uint32_t) (sy & 0xFF);
    uint16_t *out = frame + y * width;
    int32_t sx = (int32_t) std::lround(((x0 - ox) + 0.5) * 256.0 / scale) - 128;
    for (int x = x0; x < x1; ++x, sx += step) {
      const int xa = std::max(0, std::min(tw - 1, (int) (sx >> 8))), xb = std::max(0, std::min(tw - 1, (int) (sx >> 8) + 1));
      const uint32_t fx = sx < 0 ? 0 : (uint32_t) (sx & 0xFF);
      const uint32_t wa = (256 - fx) * (256 - fy) >> 8, wb = fx * (256 - fy) >> 8, wc = (256 - fx) * fy >> 8;
      const uint32_t wd = 256 - wa - wb - wc;
      const int ia = ya * stride + xa, ib = ya * stride + xb, ic = yb * stride + xa, id = yb * stride + xb;
      const uint32_t alpha =
          (alphas[ia] * wa + alphas[ib] * wb + alphas[ic] * wc + alphas[id] * wd) >> 8;
      if (!alpha) continue;
      const uint16_t c = mix4(colours[ia], colours[ib], colours[ic], colours[id], wa, wb, wc, wd);
      out[x] = blend(out[x], c, alpha);
    }
  }
}
}  // namespace sky::pixels
