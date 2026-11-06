#pragma once
// Paints one map tile (PNG or JPEG bytes) into an RGB565 frame of `width` x `height`, at the place and scale
// geo::tile_origin gives it, through `filter`. False, with `error` set, for bytes it cannot read.
#include <cstddef>
#include <cstdint>
#include <string>

#include "geo.h"
#include "tiles.h"

namespace sky::decode {
// How long a tile took: decoding it, and laying it into the frame (none when a JPEG went straight into it); whether it
// was a PNG, and went straight in.
struct Times {
  int64_t decode_us = 0, lay_us = 0;
  bool png = false, direct = false;
};
// The ways a tile is drawn quicker, each of which can be switched off to time the old way (the test API's benchmark,
// worker.cpp). The screen draws with them all, except `internal`.
struct Options {
  bool pngdec = true;    // a PNG by PNGdec, a row at a time; else by pngle, a pixel at a time
  bool table = true;     // the filter as a table (tiles.h paint_table); else each pixel painted
  bool lean = true;      // no clearing of a tile the image fills, no alphas for a JPEG, opacity known as it is decoded
  bool opaque = true;    // an opaque tile laid without reading its alphas or the frame under it
  bool direct = true;    // a JPEG at its own pixels straight into the frame
  bool internal = false; // the decoders' state in internal RAM rather than PSRAM
};
bool draw(const uint8_t *data, size_t size, uint16_t *frame, int width, int height, const geo::TileOrigin &at,
          const tiles::Filter &filter, std::string &error, Times *times = nullptr, const Options &options = {});
// The bytes of a decoder's state (PNGdec's, else JPEGDEC's), and letting go of any made in internal RAM.
size_t state_bytes(bool png);
void release_internal();
}  // namespace sky::decode
