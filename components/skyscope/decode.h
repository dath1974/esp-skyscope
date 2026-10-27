#pragma once
// Paints one map tile (PNG or JPEG bytes) into an RGB565 frame of `width` x `height`, at the place and scale
// geo::tile_origin gives it, through `filter`. False, with `error` set, for bytes it cannot read.
#include <cstddef>
#include <cstdint>
#include <string>

#include "geo.h"
#include "tiles.h"

namespace sky::decode {
bool draw(const uint8_t *data, size_t size, uint16_t *frame, int width, int height, const geo::TileOrigin &at,
          const tiles::Filter &filter, std::string &error);
}  // namespace sky::decode
