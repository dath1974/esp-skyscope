#pragma once
// A screen with its own radio needs no network: off Wi-Fi it shows the planes the radio hears on the radar, with
// nothing else that would have to be fetched (no map, airspace or weather radar). The settings themselves are kept as
// they are, so the map chosen comes back with the network. Plain C++, for the host tests.
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
    offline_ = now - since_ >= AFTER_MS;
    return offline_;
  }
  bool offline() const { return offline_; }

 private:
  uint32_t since_ = 0;  // the first pass without a network, since it was last there (or since the start)
  bool lost_ = false, offline_ = false;
};

// What the screen shows off a network: the radar, its planes from the radio alone, and nothing fetched over it.
inline Settings shown(const Settings &s) {
  Settings out = s;
  out.map = RADAR;
  out.feed = RADIO;
  out.fallback = false;
  out.airspace = false;
  out.radar_overlay = false;
  out.radar_loop = false;
  return out;
}

}  // namespace sky::offline
