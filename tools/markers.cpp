// A sheet of every kind of aircraft as the screen draws it (shapes.h, sprites.h), to look at without a screen:
//
//     make test-markers       (writes tests/build/markers.png)
//
// A row a kind: at the screen's size and a watched plane's, then large at eight headings. Dark on light, as on a map.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../components/skyscope/sprites.h"

using namespace sky;

static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}

// A grey PNG, its pixels stored in deflate's blocks that are not compressed: no zlib needed.
static bool write_png(const char *path, int w, int h, const std::vector<uint8_t> &grey) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), grey.begin() + (size_t) y * w, grey.begin() + (size_t) (y + 1) * w);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
  for (size_t at = 0; at < raw.size(); at += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size());
    z.push_back(n & 0xFF), z.push_back(n >> 8), z.push_back(~n & 0xFF), z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
  }
  const uint32_t adler = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((adler >> s) & 0xFF);
  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  auto be = [&](uint32_t v) { const uint8_t q[4] = {(uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v}; std::fwrite(q, 1, 4, f); };
  auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
    be((uint32_t) data.size());
    std::vector<uint8_t> all(type, type + 4);
    all.insert(all.end(), data.begin(), data.end());
    std::fwrite(all.data(), 1, all.size(), f);
    be(crc(all.data(), all.size()) ^ 0xFFFFFFFFu);
  };
  std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  chunk("IHDR", {(uint8_t) (w >> 24), (uint8_t) (w >> 16), (uint8_t) (w >> 8), (uint8_t) w, (uint8_t) (h >> 24),
                 (uint8_t) (h >> 16), (uint8_t) (h >> 8), (uint8_t) h, 8, 0, 0, 0, 0});
  chunk("IDAT", z);
  chunk("IEND", {});
  return std::fclose(f) == 0;
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "markers.png";
  const int cell = 72, small[] = {20, 27}, headings[] = {0, 45, 90, 135, 180, 225, 270, 315};
  const int columns = 2 + 8, w = columns * cell, h = shapes::KINDS * cell;
  std::vector<uint8_t> sheet((size_t) w * h, 235);
  auto stamp = [&](int col, int row, float length, int heading, shapes::Kind kind) {
    const sprites::Mask m = sprites::render(length * shapes::SCALE[kind], sprites::heading_step(heading), kind);
    const int x0 = col * cell + (cell - m.size) / 2, y0 = row * cell + (cell - m.size) / 2;
    for (int y = 0; y < m.size; ++y)
      for (int x = 0; x < m.size; ++x) {
        uint8_t &px = sheet[(size_t) (y0 + y) * w + x0 + x];
        px = (uint8_t) (px - (px - 30) * m.alpha[(size_t) y * m.size + x] / 255);
      }
  };
  for (int k = 0; k < shapes::KINDS; ++k) {
    stamp(0, k, small[0], 45, (shapes::Kind) k);
    stamp(1, k, small[1], 45, (shapes::Kind) k);
    for (int i = 0; i < 8; ++i) stamp(2 + i, k, 56, headings[i], (shapes::Kind) k);
    std::printf("row %2d: %s\n", k + 1, shapes::NAMES[k]);
  }
  if (!write_png(out, w, h, sheet)) return 1;
  std::printf("%s: %d x %d\n", out, w, h);
  return 0;
}
