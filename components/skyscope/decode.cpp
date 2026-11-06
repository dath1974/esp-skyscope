// A map tile's PNG or JPEG painted into the frame: decoded whole into a tile of its own (its colours through the map's
// filter, and how opaque each pixel is), then laid where tiles::plan put it: at its own pixels, enlarged smoothly
// (each pixel of the frame a blend of the four tile pixels nearest it, so a chart's words and lines keep their shape
// rather than turning into steps), or made smaller. A pixel only partly opaque (a place name's edge, a chart's border)
// is blended over what is already there. A JPEG at its own pixels (a chart's) goes straight into the frame instead.
//
// Every way it was made quicker can be switched off (Options), so the test API's benchmark (bench, below) times the
// old way and the new on the same tiles.
#include "decode.h"
#include "pixels.h"
#include "pngrow.h"

#include <JPEGDEC.h>
#include <PNGdec.h>
#include <pngle.h>

#include <new>

#include <esp_heap_caps.h>
#include <esp_timer.h>

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

// The map's filter as a table (tiles.h paint_table), in PSRAM, built again only when the filter changes; nullptr for
// a filter that changes nothing, or no memory for it (each pixel is then painted as it comes).
struct Table {
  uint16_t *colours = nullptr;
  tiles::Filter filter;
  bool built = false;
  const uint16_t *for_filter(const tiles::Filter &f) {
    if (tiles::plain(f)) return nullptr;
    if (!colours)
      colours = static_cast<uint16_t *>(heap_caps_malloc(tiles::PAINT_TABLE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!colours) return nullptr;
    if (!built || !(filter == f)) {
      tiles::paint_table(f, colours);
      filter = f;
      built = true;
    }
    return colours;
  }
};
Table table;

// The decoders' state, made once and kept: PNGdec's some 45 KB (most of it the inflater's window), JPEGDEC's some
// 18 KB. In PSRAM, or (Options internal) in the faster internal RAM, which is scarce: there only while asked for.
template <typename T> struct Kept {
  T *psram = nullptr, *internal = nullptr;
  T *get(bool in_internal) {
    T *&p = in_internal ? internal : psram;
    if (!p) {
      void *room = heap_caps_malloc(sizeof(T), (in_internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT);
      if (room) p = new (room) T();
    }
    return p;
  }
  void release_internal() {
    if (!internal) return;
    internal->~T();
    heap_caps_free(internal);
    internal = nullptr;
  }
};
Kept<PNG> pngs;
Kept<JPEGDEC> jpegs;

struct Decoding {
  Options options;
  tiles::Filter filter;
  const uint16_t *table = nullptr;  // the filter's table, or nullptr
  int width = 0, height = 0;        // what the image says it is, at most TILE a side
  bool opaque = true;               // every pixel written so far wholly opaque (Options lean)
  // A JPEG straight into the frame (Options direct): the frame, and where the tile's corner is on it.
  uint16_t *frame = nullptr;
  int frame_w = 0, frame_h = 0, ox = 0, oy = 0;
};

struct PngRows {
  Decoding *d;
  PNG *png;
  pngrow::Palette palette;
  bool painted = false;  // the palette, once the first row brings it
};
PngRows png_rows;

int png_row(PNGDRAW *row) {
  PngRows &job = *static_cast<PngRows *>(row->pUser);
  if (row->y < 0 || row->y >= TILE) return 1;
  if (row->iPixelType == pngrow::INDEXED && !job.painted) {
    pngrow::paint_palette(row->pPalette, row->iHasAlpha, job.d->filter, job.palette);
    job.painted = true;
  }
  const int w = std::min(row->iWidth, TILE);
  const pngrow::Row r{row->pPixels, w, row->iPixelType, row->iBpp, row->iHasAlpha != 0, job.png->getTransparentColor()};
  uint8_t *alpha = scratch.alpha + row->y * TILE;
  pngrow::convert(r, job.d->filter, job.d->table, &job.palette, scratch.colour + row->y * TILE, alpha);
  // Whether it is see-through anywhere, looked at while the row is still at hand.
  if (job.d->options.lean && job.d->opaque) job.d->opaque = std::all_of(alpha, alpha + w, [](uint8_t v) { return v == 255; });
  return 1;
}

inline uint16_t painted(const Decoding &d, int r, int g, int b) {
  if (d.table) return d.table[tiles::rgb565(r, g, b)];
  return tiles::paint(d.filter, r, g, b);
}

void pngle_pixel(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  Decoding &d = *static_cast<Decoding *>(pngle_get_user_data(pngle));
  const uint16_t colour = painted(d, rgba[0], rgba[1], rgba[2]);
  const uint8_t a = pngrow::opaque(d.filter, rgba[3]);
  if (a != 255) d.opaque = false;
  for (uint32_t dy = 0; dy < h; ++dy)
    for (uint32_t dx = 0; dx < w; ++dx) {
      const uint32_t px = x + dx, py = y + dy;
      if (px >= TILE || py >= TILE) continue;
      scratch.colour[py * TILE + px] = colour;
      scratch.alpha[py * TILE + px] = a;
    }
}

// A block of JPEG pixels: through the filter (its table, or painting each), into the tile's scratch (with each pixel's
// opacity, unless it is known to be whole) or straight into the frame.
int jpeg_block(JPEGDRAW *j) {
  const Decoding &d = *static_cast<const Decoding *>(j->pUser);
  const bool plain = tiles::plain(d.filter);
  auto colour_of = [&](uint16_t c) {
    return d.table ? d.table[c] : plain ? c : tiles::paint(d.filter, tiles::red_of(c), tiles::green_of(c), tiles::blue_of(c));
  };
  if (d.frame) {
    // Clipped to the tile and to the frame; a row of a plain map copied whole.
    const int x0 = std::max({0, j->x, -d.ox}), x1 = std::min({TILE, j->x + j->iWidth, d.frame_w - d.ox});
    if (x0 >= x1) return 1;
    for (int y = 0; y < j->iHeight; ++y) {
      const int py = j->y + y, fy = d.oy + py;
      if (py < 0 || py >= TILE || fy < 0 || fy >= d.frame_h) continue;
      const uint16_t *in = j->pPixels + y * j->iWidth + (x0 - j->x);
      uint16_t *out = d.frame + fy * d.frame_w + d.ox + x0;
      if (!d.table && plain) std::memcpy(out, in, (x1 - x0) * 2);
      else for (int x = 0; x < x1 - x0; ++x) out[x] = colour_of(in[x]);
    }
    return 1;
  }
  const bool alphas = !d.options.lean || d.filter.opacity != 255;
  for (int y = 0; y < j->iHeight; ++y) {
    const int py = j->y + y;
    if (py < 0 || py >= TILE) continue;
    for (int x = 0; x < j->iWidth; ++x) {
      const int px = j->x + x;
      if (px < 0 || px >= TILE) continue;
      scratch.colour[py * TILE + px] = colour_of(j->pPixels[y * j->iWidth + x]);
      if (alphas) scratch.alpha[py * TILE + px] = d.filter.opacity;
    }
  }
  return 1;
}

}  // namespace

size_t state_bytes(bool png) { return png ? sizeof(PNG) : sizeof(JPEGDEC); }
void release_internal() {
  pngs.release_internal();
  jpegs.release_internal();
}

bool draw(const uint8_t *data, size_t size, uint16_t *frame, int width, int height, const geo::TileOrigin &at,
          const tiles::Filter &filter, std::string &error, Times *times, const Options &options) {
  const int64_t started = esp_timer_get_time();
  if (!scratch.ready()) {
    error = "no memory for a tile";
    return false;
  }
  Decoding d;
  d.options = options;
  d.filter = filter;
  d.table = options.table ? table.for_filter(filter) : nullptr;
  d.width = d.height = TILE;
  d.opaque = filter.opacity == 255;
  const int ox = (int) std::lround(at.x), oy = (int) std::lround(at.y);
  // Nothing is drawn where the image has no pixel: the alphas cleared first, unless the image fills the whole tile.
  auto clear_unless_whole = [&]() {
    if (!options.lean || d.width < TILE || d.height < TILE) std::memset(scratch.alpha, 0, TILE * TILE);
  };
  bool ok = false, direct = false;
  const tiles::Format format = tiles::format_of(data, size);
  switch (format) {
    case tiles::PNG: {
      PNG *png = options.pngdec ? pngs.get(options.internal) : nullptr;
      if (png && png->openRAM(const_cast<uint8_t *>(data), (int) size, png_row) == PNG_SUCCESS) {
        d.width = std::min(TILE, png->getWidth());
        d.height = std::min(TILE, png->getHeight());
        clear_unless_whole();
        png_rows.d = &d;
        png_rows.png = png;
        png_rows.painted = false;
        const int r = png->decode(&png_rows, 0);
        png->close();
        ok = r == PNG_SUCCESS;
        if (!ok) error = "PNG error " + std::to_string(png->getLastError());
        break;
      }
      if (png) png->close();
      // A PNG PNGdec will not take (interlaced, 16 bits a channel), or the old way asked for: pngle, a pixel at a time.
      std::memset(scratch.alpha, 0, TILE * TILE);
      auto *pngle = static_cast<pngle_t *>(heap_caps_calloc(1, PNGLE_T_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!pngle) { error = "no memory for PNG"; return false; }
      pngle_reset(pngle);
      pngle_set_user_data(pngle, &d);
      pngle_set_draw_callback(pngle, pngle_pixel);
      const int fed = pngle_feed(pngle, data, size);
      if (fed < 0) {
        error = std::string("PNG: ") + pngle_error(pngle);
      } else {
        d.width = std::min<int>(TILE, (int) pngle_get_width(pngle));
        d.height = std::min<int>(TILE, (int) pngle_get_height(pngle));
        if (d.width < TILE || d.height < TILE) d.opaque = false;
      }
      pngle_reset(pngle);
      heap_caps_free(pngle);
      ok = fed >= 0;
      break;
    }
    case tiles::JPEG: {
      JPEGDEC *jpeg = jpegs.get(options.internal);
      if (!jpeg) { error = "no memory for JPEG"; return false; }
      ok = jpeg->openRAM(const_cast<uint8_t *>(data), (int) size, jpeg_block);
      if (ok && jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE) { ok = false; error = "progressive JPEG"; }
      if (ok) {
        jpeg->setUserPointer(&d);
        jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
        d.width = std::min(TILE, jpeg->getWidth());
        d.height = std::min(TILE, jpeg->getHeight());
        // At its own pixels and wholly opaque: straight into the frame, with no tile between.
        direct = options.direct && at.scale == 1.0 && filter.opacity == 255;
        if (direct) {
          d.frame = frame;
          d.frame_w = width;
          d.frame_h = height;
          d.ox = ox;
          d.oy = oy;
        } else {
          clear_unless_whole();
          if (d.width < TILE || d.height < TILE) d.opaque = false;
        }
        ok = jpeg->decode(0, 0, 0);
        if (!ok) error = "JPEG error " + std::to_string(jpeg->getLastError());
      } else if (error.empty()) {
        error = "JPEG error " + std::to_string(jpeg->getLastError());
      }
      jpeg->close();
      break;
    }
    default:
      error = "not a PNG or JPEG";
      return false;
  }
  if (!ok) return false;
  const int64_t decoded = esp_timer_get_time();
  if (!direct) {
    // Wholly opaque (a JPEG, a PNG with no see-through pixel): laid without its alphas. Known as it was decoded
    // (Options lean), or else looked for over the whole tile.
    bool opaque = false;
    if (options.opaque) {
      if (options.lean) {
        opaque = d.opaque;
      } else {
        opaque = filter.opacity == 255;
        for (int y = 0; opaque && y < d.height; ++y) {
          const uint8_t *a = scratch.alpha + y * TILE;
          opaque = std::all_of(a, a + d.width, [](uint8_t v) { return v == 255; });
        }
      }
    }
    pixels::lay(frame, width, height, ox, oy, at.scale, {scratch.colour, scratch.alpha, d.width, d.height, TILE, opaque});
  }
  if (times) {
    times->decode_us = decoded - started;
    times->lay_us = esp_timer_get_time() - decoded;
    times->png = format == tiles::PNG;
    times->direct = direct;
  }
  return true;
}
}  // namespace sky::decode
