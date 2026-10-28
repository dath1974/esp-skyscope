#pragma once
// A decoded map tile laid into the frame (decode.cpp): at its own pixels, made smaller (the tile pixel under each frame
// pixel's middle), or enlarged smoothly (each frame pixel a blend of the four tile pixels nearest it), a pixel only
// partly opaque blended over what is already there. RGB565 throughout; nothing here decodes, so the tests run on a PC.
#include <algorithm>
#include <cmath>
#include <cstdint>

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
