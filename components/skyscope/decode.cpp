// A map tile's PNG or JPEG painted into the frame: decoded whole into a tile of its own (its colours through the map's
// filter, and how opaque each pixel is), then laid where tiles::plan put it: at its own pixels, enlarged smoothly
// (each pixel of the frame a blend of the four tile pixels nearest it, so a chart's words and lines keep their shape
// rather than turning into steps), or made smaller. A pixel only partly opaque (a place name's edge, a chart's border)
// is blended over what is already there.
#include "decode.h"
#include "pixels.h"

#include <JPEGDEC.h>
#include <pngle.h>

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sky::decode {
namespace {
constexpr int TILE = 256;

// The tile as decoded: RGB565 and opacity (0 to 255), in PSRAM, kept for the worker's whole life (it alone decodes).
struct Scratch {
  uint16_t *colour = nullptr;
  uint8_t *alpha = nullptr;
  bool ready() {
    if (!colour) colour = static_cast<uint16_t *>(heap_caps_malloc(TILE * TILE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!alpha) alpha = static_cast<uint8_t *>(heap_caps_malloc(TILE * TILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return colour && alpha;
  }
};
Scratch scratch;

struct Decoding {
  tiles::Filter filter;
  int width = 0, height = 0;  // what the image says it is, at most TILE a side
};

void png_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  const Decoding &d = *static_cast<const Decoding *>(pngle_get_user_data(pngle));
  const uint16_t colour = tiles::paint(d.filter, rgba[0], rgba[1], rgba[2]);
  for (uint32_t dy = 0; dy < h; ++dy)
    for (uint32_t dx = 0; dx < w; ++dx) {
      const uint32_t px = x + dx, py = y + dy;
      if (px >= TILE || py >= TILE) continue;
      scratch.colour[py * TILE + px] = colour;
      scratch.alpha[py * TILE + px] = rgba[3];
    }
}

int jpeg_draw(JPEGDRAW *j) {
  const Decoding &d = *static_cast<const Decoding *>(j->pUser);
  for (int y = 0; y < j->iHeight; ++y) {
    const int py = j->y + y;
    if (py < 0 || py >= TILE) continue;
    for (int x = 0; x < j->iWidth; ++x) {
      const int px = j->x + x;
      if (px < 0 || px >= TILE) continue;
      const uint16_t c = j->pPixels[y * j->iWidth + x];
      scratch.colour[py * TILE + px] =
          d.filter.night == tiles::NIGHT_NONE && d.filter.keep >= 256
              ? c
              : tiles::paint(d.filter, (c >> 8 & 0xF8) | c >> 13, (c >> 3 & 0xFC) | (c >> 9 & 0x3), (c << 3 & 0xF8) | (c >> 2 & 0x7));
      scratch.alpha[py * TILE + px] = 255;
    }
  }
  return 1;
}

}  // namespace

bool draw(const uint8_t *data, size_t size, uint16_t *frame, int width, int height, const geo::TileOrigin &at,
          const tiles::Filter &filter, std::string &error) {
  if (!scratch.ready()) {
    error = "no memory for a tile";
    return false;
  }
  // Nothing is drawn where the image has no pixel.
  std::memset(scratch.alpha, 0, TILE * TILE);
  Decoding d{filter, TILE, TILE};
  bool ok = false;
  switch (tiles::format_of(data, size)) {
    case tiles::PNG: {
      auto *pngle = static_cast<pngle_t *>(heap_caps_calloc(1, PNGLE_T_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!pngle) { error = "no memory for PNG"; return false; }
      pngle_reset(pngle);
      pngle_set_user_data(pngle, &d);
      pngle_set_draw_callback(pngle, png_draw);
      const int fed = pngle_feed(pngle, data, size);
      if (fed < 0) {
        error = std::string("PNG: ") + pngle_error(pngle);
      } else {
        d.width = std::min<int>(TILE, (int) pngle_get_width(pngle));
        d.height = std::min<int>(TILE, (int) pngle_get_height(pngle));
      }
      pngle_reset(pngle);
      heap_caps_free(pngle);
      ok = fed >= 0;
      break;
    }
    case tiles::JPEG: {
      // JPEGDEC keeps its whole state in the object, some 18 KB: on the heap (PSRAM), not on the worker's stack.
      auto *jpeg = new JPEGDEC();
      ok = jpeg->openRAM(const_cast<uint8_t *>(data), (int) size, jpeg_draw);
      if (ok && jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) { ok = false; error = "progressive JPEG"; }
      if (ok) {
        jpeg->setUserPointer(&d);
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        d.width = std::min(TILE, jpeg->getWidth());
        d.height = std::min(TILE, jpeg->getHeight());
        ok = jpeg->decode(0, 0, 0);
        if (!ok) error = "JPEG error " + std::to_string(jpeg->getLastError());
      } else if (error.empty()) {
        error = "JPEG error " + std::to_string(jpeg->getLastError());
      }
      jpeg->close();
      delete jpeg;
      break;
    }
    default:
      error = "not a PNG or JPEG";
      return false;
  }
  if (!ok) return false;
  pixels::lay(frame, width, height, (int) std::lround(at.x), (int) std::lround(at.y), at.scale,
              {scratch.colour, scratch.alpha, d.width, d.height, TILE});
  return true;
}
}  // namespace sky::decode
