#pragma once
// The Wi-Fi networks the screen knows, first choice first (wifi_list.h), kept in the preferences and handed to
// ESPHome as its networks, each with a priority by its place, so the screen joins whichever is in reach when it starts
// or loses the one it is on. ESPHome's own saved network is kept as the first choice, so its setup page and Improv go
// on as they did: a network given there joins the list first.
//
// It starts just after ESPHome's Wi-Fi does: ESPHome has loaded its saved network and started its first scan, which
// is matched against the networks it holds when the scan ends. Changes are kept at once, and handed to ESPHome too, so
// a network lost before a restart is looked for among them all.
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <mutex>
#include <string>

#include "wifi_list.h"

namespace esphome::skyscope {

class Networks : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }
  void setup() override;
  void dump_config() override;

  // A copy, for any task.
  sky::wifi_list::List list() const;
  // On the loop: a network joined (first choice), moved, forgotten. False when nothing changed.
  bool joined(const std::string &name, const std::string &password);
  bool move(size_t i, int step);
  bool forget(size_t i);

 protected:
  void keep_();       // stored, ESPHome's own network the first choice, and ESPHome's list
  void hand_over_();  // the list to ESPHome

  ESPPreferenceObject pref_;
  mutable std::mutex lock_;
  sky::wifi_list::List list_;
};

}  // namespace esphome::skyscope
