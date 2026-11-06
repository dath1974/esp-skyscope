#pragma once
// One row of a PNG as PNGdec hands it over (its own bytes, unpacked by nothing), into a tile's colours through the
// map's filter and how opaque each pixel is (decode.cpp). PNGdec does the inflating and the PNG filters a row at a time,
// far quicker than a pixel at a time; what it leaves is laid out here, for every kind of PNG a map server sends at up to
// 8 bits a channel (it refuses 16 bits and interlacing, which decode.cpp then gives to pngle).
//
// A palette image (OpenStreetMap's, most charts') has its palette painted once a tile, exactly, rather than each pixel;
// any other has each colour through the filter's table (tiles.h paint_table), or painted when there is none.
#include <cstdint>

#include "tiles.h"

namespace sky::pngrow {

enum Type : int { GRAY = 0, RGB = 2, INDEXED = 3, GRAY_ALPHA = 4, RGBA = 6 };

// The palette painted: each entry's colour through the filter, and how opaque it is (its tRNS, else whole).
struct Palette {
  uint16_t colour[256];
  uint8_t alpha[256];
};

// How opaque a pixel is drawn: its own alpha, times the filter's opacity (the weather radar's see-through tiles).
inline uint8_t opaque(const tiles::Filter &f, uint32_t alpha) {
  return f.opacity == 255 ? (uint8_t) alpha : (uint8_t) (alpha * f.opacity / 255);
}

// `palette` as PNGdec keeps it: 256 RGB triples, then 256 alphas (all 255 without a tRNS chunk).
inline void paint_palette(const uint8_t *palette, bool has_alpha, const tiles::Filter &f, Palette &out) {
  for (int i = 0; i < 256; ++i) {
    out.colour[i] = tiles::paint(f, palette[3 * i], palette[3 * i + 1], palette[3 * i + 2]);
    out.alpha[i] = opaque(f, has_alpha ? palette[768 + i] : 255);
  }
}

struct Row {
  const uint8_t *pixels;
  int width;
  int type;               // Type
  int bits;               // a channel's bits: 1, 2, 4 or 8 (1 to 8 for a palette or grey, 8 for colour)
  bool has_alpha;         // a tRNS chunk: a palette's alphas, or one grey or colour that is see-through
  uint32_t transparent;   // that grey (its low byte) or colour (0xRRGGBB)
};

// A sample of `bits` bits, the x'th of a packed row.
inline uint32_t sample(const uint8_t *row, int x, int bits) {
  if (bits == 8) return row[x];
  const int at = x * bits;
  return (uint32_t) (row[at >> 3] >> (8 - bits - (at & 7))) & ((1u << bits) - 1);
}

// The row into `colour` and `alpha`, `row.width` of each. `table` is the filter's table, or nullptr to paint each
// pixel; `palette` is the painted palette, for a palette image.
inline void convert(const Row &row, const tiles::Filter &f, const uint16_t *table, const Palette *palette,
                    uint16_t *colour, uint8_t *alpha) {
  auto paint = [&](int r, int g, int b) { return table ? table[tiles::rgb565(r, g, b)] : tiles::paint(f, r, g, b); };
  const uint8_t *p = row.pixels;
  const uint8_t whole = opaque(f, 255);
  switch (row.type) {
    case INDEXED:
      for (int x = 0; x < row.width; ++x) {
        const uint32_t i = sample(p, x, row.bits);
        colour[x] = palette->colour[i];
        alpha[x] = palette->alpha[i];
      }
      break;
    case GRAY: {
      const uint32_t most = (1u << row.bits) - 1;
      for (int x = 0; x < row.width; ++x) {
        const uint32_t s = sample(p, x, row.bits), v = s * 255 / most;
        colour[x] = paint((int) v, (int) v, (int) v);
        alpha[x] = row.has_alpha && s == (row.transparent & 0xFF) ? 0 : whole;
      }
      break;
    }
    case GRAY_ALPHA:
      for (int x = 0; x < row.width; ++x) {
        colour[x] = paint(p[2 * x], p[2 * x], p[2 * x]);
        alpha[x] = opaque(f, p[2 * x + 1]);
      }
      break;
    case RGB:
      for (int x = 0; x < row.width; ++x) {
        const uint8_t r = p[3 * x], g = p[3 * x + 1], b = p[3 * x + 2];
        colour[x] = paint(r, g, b);
        alpha[x] = row.has_alpha && ((uint32_t) r << 16 | (uint32_t) g << 8 | b) == row.transparent ? 0 : whole;
      }
      break;
    case RGBA:
      for (int x = 0; x < row.width; ++x) {
        const uint8_t a = p[4 * x + 3];
        // A pixel nobody sees is not painted.
        colour[x] = a ? paint(p[4 * x], p[4 * x + 1], p[4 * x + 2]) : 0;
        alpha[x] = opaque(f, a);
      }
      break;
    default:
      for (int x = 0; x < row.width; ++x) alpha[x] = 0;
      break;
  }
}
}  // namespace sky::pngrow
