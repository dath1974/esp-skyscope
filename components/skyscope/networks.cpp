#include "networks.h"

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::skyscope {
static const char *const TAG = "skyscope.networks";
using namespace sky;

void Networks::setup() {
  pref_ = global_preferences->make_preference<wifi_list::Stored>(fnv1_hash("skyscope_networks"), true);
  wifi_list::Stored stored{};
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (pref_.load(&stored)) list_ = wifi_list::from(stored);
  }
  auto *wifi = wifi::global_wifi_component;
  if (!wifi) return;
  bool changed = false;
  if (wifi->has_sta()) {
    // The network ESPHome loaded: new, or with a new password, it was just given (its setup page, Improv).
    const wifi::WiFiAP ap = wifi->get_sta();
    const StringRef name = ap.get_ssid(), password = ap.get_password();
    std::lock_guard<std::mutex> guard(lock_);
    changed = list_.merge(std::string(name.c_str(), name.size()), std::string(password.c_str(), password.size()));
  }
  size_t count;
  {
    std::lock_guard<std::mutex> guard(lock_);
    count = list_.networks.size();
  }
  if (!count) return;  // none yet: ESPHome's setup network, as at a first start
  if (changed || !wifi->has_sta()) {
    // ESPHome without a network of its own (its record lost) is given the first choice, as by its setup page.
    keep_();
    return;
  }
  hand_over_();
}

void Networks::dump_config() {
  std::lock_guard<std::mutex> guard(lock_);
  ESP_LOGCONFIG(TAG, "Wi-Fi networks known: %u", (unsigned) list_.networks.size());
}

wifi_list::List Networks::list() const {
  std::lock_guard<std::mutex> guard(lock_);
  return list_;
}

bool Networks::joined(const std::string &name, const std::string &password) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!list_.add_first(name, password)) return false;
  }
  keep_();
  return true;
}

bool Networks::move(size_t i, int step) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!list_.move(i, step)) return false;
  }
  keep_();
  return true;
}

bool Networks::forget(size_t i) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!list_.forget(i)) return false;
  }
  keep_();
  return true;
}

void Networks::keep_() {
  wifi_list::Network first;
  wifi_list::Stored stored;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (list_.networks.empty()) return;
    first = list_.networks[0];
    stored = wifi_list::stored(list_);
  }
  pref_.save(&stored);
  // ESPHome's own saved network is the first choice: the one it starts with (and the one its setup page would show).
  // Saving it leaves ESPHome holding that one only, so the whole list is handed over again after.
  if (auto *wifi = wifi::global_wifi_component) wifi->save_wifi_sta(first.name, first.password);
  else global_preferences->sync();
  hand_over_();
}

void Networks::hand_over_() {
  auto *wifi = wifi::global_wifi_component;
  if (!wifi) return;
  std::lock_guard<std::mutex> guard(lock_);
  if (list_.networks.empty()) return;
  wifi->clear_sta();
  wifi->init_sta(list_.networks.size());
  for (size_t i = 0; i < list_.networks.size(); ++i) {
    wifi::WiFiAP ap;
    ap.set_ssid(list_.networks[i].name);
    ap.set_password(list_.networks[i].password);
    ap.set_priority(list_.priority(i));
    wifi->add_sta(ap);
  }
  ESP_LOGD(TAG, "%u networks handed to Wi-Fi", (unsigned) list_.networks.size());
}

}  // namespace esphome::skyscope
