// A map tile's PNG or JPEG painted straight into the frame: each tile pixel lands where tiles::plan put its tile,
// enlarged (or made smaller) by a power of two, through the map's filter. No tile is ever held decoded.
#include "decode.h"

#include <JPEGDEC.h>
#include <pngle.h>

#include <esp_heap_caps.h>

#include <cmath>
#include <cstring>

namespace sky::decode {
namespace {
struct Target {
  uint16_t *frame;
  int width, height;
  int ox, oy;     // where the tile's first pixel lands
  int up;         // > 0: each tile pixel is 1 << up pixels across
  int down;       // > 0: one of every 1 << down tile pixels is kept
  tiles::Filter filter;
};

// `colour` over the frame's pixel, `alpha` of it (255: all of it): a place name's soft edges over the map under it.
inline uint16_t over(uint16_t under, uint16_t colour, uint32_t alpha) {
  if (alpha >= 255) return colour;
  const uint32_t a = alpha + (alpha >> 7);  // 0 to 256
  const uint32_t r = ((under >> 11) * (256 - a) + (colour >> 11) * a) >> 8;
  const uint32_t g = ((under >> 5 & 0x3F) * (256 - a) + (colour >> 5 & 0x3F) * a) >> 8;
  const uint32_t b = ((under & 0x1F) * (256 - a) + (colour & 0x1F) * a) >> 8;
  return (uint16_t) (r << 11 | g << 5 | b);
}

inline void put(const Target &t, int x, int y, uint16_t colour, uint32_t alpha = 255) {
  if (t.down) {
    const int mask = (1 << t.down) - 1;
    if ((x & mask) || (y & mask)) return;
    x >>= t.down;
    y >>= t.down;
    const int fx = t.ox + x, fy = t.oy + y;
    if (fx >= 0 && fy >= 0 && fx < t.width && fy < t.height) {
      uint16_t &px = t.frame[fy * t.width + fx];
      px = over(px, colour, alpha);
    }
    return;
  }
  const int n = 1 << t.up;
  const int fx0 = t.ox + x * n, fy0 = t.oy + y * n;
  if (fx0 >= t.width || fy0 >= t.height || fx0 + n <= 0 || fy0 + n <= 0) return;
  for (int dy = 0; dy < n; ++dy) {
    const int fy = fy0 + dy;
    if (fy < 0 || fy >= t.height) continue;
    uint16_t *row = t.frame + fy * t.width;
    for (int dx = 0; dx < n; ++dx) {
      const int fx = fx0 + dx;
      if (fx >= 0 && fx < t.width) row[fx] = over(row[fx], colour, alpha);
    }
  }
}

void png_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  const Target &t = *static_cast<const Target *>(pngle_get_user_data(pngle));
  // A transparent pixel (a chart's edge, a layer of names between them) leaves what is under it; a partly transparent
  // one (the soft edge of a name) is blended with it.
  if (rgba[3] == 0) return;
  const uint16_t colour = tiles::paint(t.filter, rgba[0], rgba[1], rgba[2]);
  for (uint32_t dy = 0; dy < h; ++dy)
    for (uint32_t dx = 0; dx < w; ++dx) put(t, (int) (x + dx), (int) (y + dy), colour, rgba[3]);
}

int jpeg_draw(JPEGDRAW *d) {
  const Target &t = *static_cast<const Target *>(d->pUser);
  const uint16_t *px = d->pPixels;
  for (int y = 0; y < d->iHeight; ++y)
    for (int x = 0; x < d->iWidth; ++x) {
      const uint16_t c = px[y * d->iWidth + x];
      const uint16_t colour = t.filter.kind == FILTER_NONE
                                  ? c
                                  : tiles::paint(t.filter, (c >> 8 & 0xF8) | c >> 13, (c >> 3 & 0xFC) | (c >> 9 & 0x3),
                                                 (c << 3 & 0xF8) | (c >> 2 & 0x7));
      put(t, d->x + x, d->y + y, colour);
    }
  return 1;
}
}  // namespace

bool draw(const uint8_t *data, size_t size, uint16_t *frame, int width, int height, const geo::TileOrigin &at,
          const tiles::Filter &filter, std::string &error) {
  Target t{frame, width, height, (int) std::lround(at.x), (int) std::lround(at.y), 0, 0, filter};
  const int power = (int) std::lround(std::log2(at.scale));
  if (power >= 0) t.up = power; else t.down = -power;
  switch (tiles::format_of(data, size)) {
    case tiles::PNG: {
      auto *pngle = static_cast<pngle_t *>(heap_caps_calloc(1, PNGLE_T_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!pngle) { error = "no memory for PNG"; return false; }
      pngle_reset(pngle);
      pngle_set_user_data(pngle, &t);
      pngle_set_draw_callback(pngle, png_draw);
      const int fed = pngle_feed(pngle, data, size);
      if (fed < 0) error = std::string("PNG: ") + pngle_error(pngle);
      pngle_reset(pngle);
      heap_caps_free(pngle);
      return fed >= 0;
    }
    case tiles::JPEG: {
      // JPEGDEC keeps its whole state in the object, some 18 KB: on the heap (PSRAM), not on the worker's stack.
      auto *jpeg = new JPEGDEC();
      bool ok = jpeg->openRAM(const_cast<uint8_t *>(data), (int) size, jpeg_draw);
      if (ok && jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) { ok = false; error = "progressive JPEG"; }
      if (ok) {
        jpeg->setUserPointer(&t);
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        ok = jpeg->decode(0, 0, 0);
        if (!ok) error = "JPEG error " + std::to_string(jpeg->getLastError());
      } else if (error.empty()) {
        error = "JPEG error " + std::to_string(jpeg->getLastError());
      }
      jpeg->close();
      delete jpeg;
      return ok;
    }
    default:
      error = "not a PNG or JPEG";
      return false;
  }
}
}  // namespace sky::decode
