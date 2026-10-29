#pragma once
// Every colour the screen draws, in one table. The screen is dark-only: an AMOLED shows black as off, and a map at night
// should not light the room.
#include <cstdint>

namespace sky::colours {
constexpr uint32_t BACKGROUND = 0x000000;
constexpr uint32_t CARD = 0x15191E;     // behind labels and panels
constexpr uint32_t INK = 0xF2F4F7;
constexpr uint32_t MUTED = 0x9AA3AD;
constexpr uint32_t ACCENT = 0x33B5FF;
constexpr uint32_t LIGHT_ACCENT = 0x0A7CC2;  // the accent on the light appearance's paper
constexpr uint32_t WARNING = 0xFFB000;
constexpr uint32_t ERROR = 0xFF5A4E;
constexpr uint32_t EMERGENCY = 0xFF2D2D;

// The screen's own colours over the map, by the appearance (settings.h Theme): the ground where no map is, the panels
// and pills (status, labels, a plane's card), their ink and their quieter ink.
struct Palette { uint32_t ground, card, ink, muted; };
inline constexpr Palette DARK_PALETTE{BACKGROUND, CARD, INK, MUTED};
inline constexpr Palette LIGHT_PALETTE{0xE9EBEE, 0xFAFBFC, 0x1B1F24, 0x5D6670};
inline const Palette &palette(bool light) { return light ? LIGHT_PALETTE : DARK_PALETTE; }

// By band of altitude (overlay.h BANDS): on the ground, low, middle, high, top.
inline constexpr uint32_t BANDS[] = {0x9AA2AA, 0xFF9533, 0x4CC764, 0x5A9BEF, 0xB27BEB};
// Watched planes, in the order of settings.h WATCH_COLOURS.
inline constexpr uint32_t WATCH[] = {0xF44336, 0xFF9800, 0xFFC107, 0x4CAF50, 0x2196F3, 0x926BC7};

// The FAA's airspace (airspace.h) over a map, as its charts colour it: Class B and D blue (D dashed), C magenta.
struct AirspaceInk { uint32_t b, c, d; };
inline constexpr AirspaceInk DARK_AIRSPACE{0x5A9BEF, 0xD86BD4, 0x5A9BEF};
inline constexpr AirspaceInk LIGHT_AIRSPACE{0x1F5FBF, 0xA0309C, 0x1F5FBF};
inline const AirspaceInk &airspace_ink(bool light) { return light ? LIGHT_AIRSPACE : DARK_AIRSPACE; }

// A radar scope's phosphor: its ground, its glow and its hottest light, in the order of settings.h TINTS.
struct Tint { uint32_t ground, glow, hot; };
inline constexpr Tint TINTS[] = {
    {0x04140A, 0x1AFF80, 0xB8FFD6},  // green
    {0x140C02, 0xFFB000, 0xFFE4A0},  // amber
    {0x030E16, 0x33CCFF, 0xC4F0FF},  // blue
    {0x0C0E0E, 0xE6F2EE, 0xFFFFFF},  // white
};
constexpr int TINT_COUNT = sizeof(TINTS) / sizeof(TINTS[0]);
inline const Tint &tint(int i) { return TINTS[i >= 0 && i < TINT_COUNT ? i : 0]; }

// A blend of two colours: `amount` (0 to 255) of `first`, the rest of `second`, channel by channel, rounded.
inline uint32_t mix(uint32_t first, uint32_t second, uint8_t amount) {
  auto channel = [&](int shift) {
    const int a = (int) (first >> shift & 0xFF), b = (int) (second >> shift & 0xFF);
    return (uint32_t) ((a * amount + b * (255 - amount) + 127) / 255) << shift;
  };
  return channel(16) | channel(8) | channel(0);
}
// How much of the glow each part of a scope is, mixed into its ground.
namespace share {
constexpr float GRID = 0.16f, RING = 0.42f, ROSE = 0.6f, LABEL = 0.5f;
// The airspace on the scope, all in its phosphor as a controller's map is in one colour: Class B the brightest.
constexpr float CLASS_B = 0.55f, CLASS_C = 0.45f, CLASS_D = 0.36f, RUNWAY = 0.85f, CENTERLINE = 0.4f;
}
inline uint32_t glow_share(const Tint &t, float s) { return mix(t.glow, t.ground, (uint8_t) (s * 255 + 0.5f)); }
}  // namespace sky::colours
