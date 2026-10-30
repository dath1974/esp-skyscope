#pragma once
// The Wi-Fi networks the screen knows, first choice first, so it can be moved between places and join whichever is in
// reach (networks.cpp hands them to ESPHome, which picks the best match of a scan by their order, then by signal).
// ESPHome itself keeps one network, the one its setup page or Improv was last given; that one joins the list as its
// first choice when it is new. There is always at least one network, and passwords never leave the screen.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sky::wifi_list {

constexpr size_t MOST = 8;
constexpr size_t NAME_BYTES = 33, PASSWORD_BYTES = 65;  // as ESPHome keeps them, with their terminators

struct Network {
  std::string name, password;
};

struct List {
  std::vector<Network> networks;

  int find(const std::string &name) const {
    for (size_t i = 0; i < networks.size(); ++i)
      if (networks[i].name == name) return (int) i;
    return -1;
  }
  // A network joined: first choice from now on, with this password; the last choice goes when there are MOST.
  bool add_first(const std::string &name, const std::string &password) {
    if (name.empty() || name.size() >= NAME_BYTES || password.size() >= PASSWORD_BYTES) return false;
    const int at = find(name);
    if (at == 0 && networks[0].password == password) return false;
    if (at >= 0) networks.erase(networks.begin() + at);
    networks.insert(networks.begin(), {name, password});
    if (networks.size() > MOST) networks.resize(MOST);
    return true;
  }
  // One place earlier (`step` -1) or later (1).
  bool move(size_t i, int step) {
    const long to = (long) i + step;
    if (i >= networks.size() || to < 0 || to >= (long) networks.size()) return false;
    std::swap(networks[i], networks[(size_t) to]);
    return true;
  }
  // Never the last: a screen with no network would have no way back but its setup network.
  bool forget(size_t i) {
    if (i >= networks.size() || networks.size() <= 1) return false;
    networks.erase(networks.begin() + i);
    return true;
  }
  // The network ESPHome started with. Unknown, or known with another password, it was just given one (its setup page,
  // Improv): first choice. Known as it is, the order stays as it was put.
  bool merge(const std::string &name, const std::string &password) {
    if (name.empty()) return false;
    const int at = find(name);
    if (at >= 0 && networks[at].password == password) return false;
    return add_first(name, password);
  }
  // ESPHome's priority for the network at `i`: the earlier, the higher.
  int8_t priority(size_t i) const { return (int8_t) (networks.size() - i); }
};

// As the preferences keep it: plain bytes, and a check so bytes of something else are never read as networks.
struct Stored {
  uint32_t check;
  uint8_t count;
  char names[MOST][NAME_BYTES];
  char passwords[MOST][PASSWORD_BYTES];
};
constexpr uint32_t CHECK = 0x534B5731;  // "SKW1"

inline Stored stored(const List &list) {
  Stored s;
  std::memset(&s, 0, sizeof(s));
  s.check = CHECK;
  s.count = (uint8_t) std::min(list.networks.size(), MOST);
  for (size_t i = 0; i < s.count; ++i) {
    std::strncpy(s.names[i], list.networks[i].name.c_str(), NAME_BYTES - 1);
    std::strncpy(s.passwords[i], list.networks[i].password.c_str(), PASSWORD_BYTES - 1);
  }
  return s;
}
inline List from(const Stored &s) {
  List list;
  if (s.check != CHECK) return list;
  for (size_t i = 0; i < s.count && i < MOST; ++i) {
    const std::string name(s.names[i], strnlen(s.names[i], NAME_BYTES - 1));
    if (name.empty() || list.find(name) >= 0) continue;
    list.networks.push_back({name, std::string(s.passwords[i], strnlen(s.passwords[i], PASSWORD_BYTES - 1))});
  }
  return list;
}
}  // namespace sky::wifi_list
