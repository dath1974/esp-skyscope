#pragma once
// A screen with its own radio needs no network: off Wi-Fi it shows the planes the radio hears, over the map from its
// map card (card.h) or else on the radar, with nothing that would have to be fetched (no airspace or weather radar).
// The settings themselves are kept as they are, so everything chosen comes back with the network. Plain C++, for the
// host tests.
#include <cstdint>

#include "settings.h"

namespace sky::offline {

// A connection lost for less than this is a blip (a Wi-Fi network roaming, a router restarting): the map stays. At
// start, the screen tries its network for this long before giving up on it.
constexpr uint32_t AFTER_MS = 20000;

// Whether the screen runs on its radio alone: it has one, and has been off a network for AFTER_MS. Told the time and
// whether it is on a network on every pass of the loop; back on one, it is online again at once.
class Tracker {
 public:
  bool update(bool radio, bool connected, uint32_t now) {
    if (connected || !radio) {
      lost_ = false;
      offline_ = false;
      return false;
    }
    if (!lost_) {
      lost_ = true;
      since_ = now;
    }
    // Once offline it stays so until a network is back: asked again each pass, the screen's clock (32 bits of
    // milliseconds, ticks.h) would turn over after seven weeks off a network and say it had only just lost it.
    if (!offline_) offline_ = now - since_ >= AFTER_MS;
    return offline_;
  }
  bool offline() const { return offline_; }

 private:
  uint32_t since_ = 0;  // the first pass without a network, since it was last there (or since the start)
  bool lost_ = false, offline_ = false;
};

// What the screen shows off a network: its planes from the radio alone, and nothing fetched over it. The map chosen
// stays where there is a map card (card.h) to draw it from (a custom server's tiles are not kept there); else the
// radar.
inline Settings shown(const Settings &s, bool card = false) {
  Settings out = s;
  if (!card || s.map == CUSTOM) out.map = RADAR;
  out.feed = RADIO;
  out.fallback = false;
  out.airspace = false;
  out.radar_overlay = false;
  out.radar_loop = false;
  return out;
}

}  // namespace sky::offline
