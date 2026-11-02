// The map card (card_io.h): ESP-IDF's SD/MMC host and FAT. Only the worker task calls in here, so nothing is locked.
//
// On the ESP32-P4 the card shares the SD/MMC host with the Wi-Fi chip (ESP-Hosted, slot 1): the host is started once
// for both, and a card that is not there takes down only its own slot (SDMMC_HOST_FLAG_DEINIT_ARG), never the Wi-Fi's.
#include "card_io.h"

#include "esphome/core/defines.h"

#ifdef USE_SKYSCOPE_CARD
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#endif

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace esphome::skyscope::card_io {
static const char *const TAG = "skyscope.card";

namespace {
sdmmc_card_t *card_ = nullptr;
uint32_t errors_ = 0;
// Failures in a row before the card is taken as gone.
constexpr uint32_t MOST_ERRORS = 8;

void failed(const char *what, const std::string &path) {
  ++errors_;
  ESP_LOGW(TAG, "%s %s: %s", what, path.c_str(), strerror(errno));
  if (errors_ >= MOST_ERRORS && card_) {
    ESP_LOGW(TAG, "The card no longer answers: unmounted");
    esp_vfs_fat_sdcard_unmount(sky::card::ROOT, card_);
    card_ = nullptr;
  }
}
bool read_all(int fd, uint8_t *to, size_t n) {
  while (n) {
    const ssize_t got = ::read(fd, to, n);
    if (got <= 0) return false;
    to += got;
    n -= (size_t) got;
  }
  return true;
}
bool write_all(int fd, const uint8_t *from, size_t n) {
  while (n) {
    const ssize_t put = ::write(fd, from, n);
    if (put <= 0) return false;
    from += put;
    n -= (size_t) put;
  }
  return true;
}
}  // namespace

bool mount(const Config &c, std::string &error) {
  if (card_) return true;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = c.slot;
  host.max_freq_khz = c.khz;
#ifdef CONFIG_IDF_TARGET_ESP32P4
  // The slot's pins are powered by one of the chip's own LDOs, as Waveshare's board support does it.
  static sd_pwr_ctrl_handle_t power = nullptr;
  if (c.power_ldo >= 0 && !power) {
    sd_pwr_ctrl_ldo_config_t ldo{};
    ldo.ldo_chan_id = c.power_ldo;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &power) != ESP_OK) {
      error = "its power (LDO " + std::to_string(c.power_ldo) + ") would not start";
      power = nullptr;
      return false;
    }
  }
  if (power) host.pwr_ctrl_handle = power;
#endif
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = c.width;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#ifdef SOC_SDMMC_USE_GPIO_MATRIX
  if (c.clk >= 0) {
    slot.clk = (gpio_num_t) c.clk;
    slot.cmd = (gpio_num_t) c.cmd;
    slot.d0 = (gpio_num_t) c.d0;
    if (c.width >= 4) {
      slot.d1 = (gpio_num_t) c.d1;
      slot.d2 = (gpio_num_t) c.d2;
      slot.d3 = (gpio_num_t) c.d3;
    }
  }
#endif
  if (c.hold_high >= 0) {
    gpio_config_t out{};
    out.pin_bit_mask = 1ULL << c.hold_high;
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_set_level((gpio_num_t) c.hold_high, 1);
  }
  esp_vfs_fat_sdmmc_mount_config_t m{};
  m.format_if_mount_failed = false;  // a card with something else on it is left alone
  m.max_files = 2;
  m.allocation_unit_size = 16 * 1024;
  const esp_err_t err = esp_vfs_fat_sdmmc_mount(sky::card::ROOT, &host, &slot, &m, &card_);
  if (err != ESP_OK) {
    card_ = nullptr;
    error = err == ESP_FAIL ? "the card is not FAT32 (format it so on a computer)"
          : err == ESP_ERR_TIMEOUT ? "no card in the slot" : esp_err_to_name(err);
    ESP_LOGI(TAG, "No map card: %s", error.c_str());
    return false;
  }
  errors_ = 0;
  ::mkdir((std::string(sky::card::ROOT) + "/tiles").c_str(), 0775);
  ESP_LOGI(TAG, "Map card: %s, %llu MB, %u kHz", card_->cid.name,
           (unsigned long long) ((uint64_t) card_->csd.capacity * card_->csd.sector_size / (1024 * 1024)),
           (unsigned) card_->max_freq_khz);
  return true;
}

bool mounted() { return card_ != nullptr; }
uint32_t errors() { return errors_; }

bool space(uint64_t &total, uint64_t &free) {
  if (!card_) return false;
  return esp_vfs_fat_info(sky::card::ROOT, &total, &free) == ESP_OK;
}

bool read(const sky::tiles::Key &k, sky::card::Header &h, sky::http::Body &body) {
  body.clear();
  if (!card_) return false;
  const std::string p = sky::card::path(k);
  const int fd = ::open(p.c_str(), O_RDONLY);
  if (fd < 0) return false;  // not on the card: no error
  struct stat st {};
  uint8_t head[sky::card::HEADER_BYTES];
  bool ok = fstat(fd, &st) == 0 && st.st_size >= (off_t) sizeof(head) && read_all(fd, head, sizeof(head)) &&
            sky::card::decode(head, sizeof(head), h);
  const size_t size = ok ? (size_t) st.st_size - sizeof(head) : 0;
  if (ok && size) ok = body.reserve(size + 1) && read_all(fd, body.data, size);
  ::close(fd);
  if (!ok) {
    body.clear();
    failed("Reading", p);
    return false;
  }
  body.size = size;
  errors_ = 0;
  return true;
}

bool read_header(const sky::tiles::Key &k, sky::card::Header &h) {
  if (!card_) return false;
  const int fd = ::open(sky::card::path(k).c_str(), O_RDONLY);
  if (fd < 0) return false;
  uint8_t head[sky::card::HEADER_BYTES];
  const bool ok = read_all(fd, head, sizeof(head)) && sky::card::decode(head, sizeof(head), h);
  ::close(fd);
  return ok;
}

bool write(const sky::tiles::Key &k, const sky::card::Header &h, const uint8_t *data, size_t size) {
  if (!card_) return false;
  const std::string p = sky::card::path(k);
  for (const std::string &folder : sky::card::folders(k)) ::mkdir(folder.c_str(), 0775);
  std::string tmp = p;
  tmp.back() = 'n';  // 779.t written as 779.n first
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0) {
    failed("Opening", tmp);
    return false;
  }
  uint8_t head[sky::card::HEADER_BYTES];
  sky::card::encode(h, head);
  const bool ok = write_all(fd, head, sizeof(head)) && (!size || write_all(fd, data, size));
  if (::close(fd) != 0 || !ok) {
    ::unlink(tmp.c_str());
    failed("Writing", tmp);
    return false;
  }
  ::unlink(p.c_str());  // FAT renames only onto nothing
  if (::rename(tmp.c_str(), p.c_str()) != 0) {
    ::unlink(tmp.c_str());
    failed("Renaming", tmp);
    return false;
  }
  errors_ = 0;
  return true;
}

bool read_text(const std::string &path, std::string &text) {
  text.clear();
  if (!card_) return false;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  char buffer[128];
  ssize_t got;
  while ((got = ::read(fd, buffer, sizeof(buffer))) > 0 && text.size() < 1024) text.append(buffer, (size_t) got);
  ::close(fd);
  return !text.empty();
}

bool write_text(const std::string &path, const std::string &text) {
  if (!card_) return false;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0) return false;
  const bool ok = write_all(fd, reinterpret_cast<const uint8_t *>(text.data()), text.size());
  return ::close(fd) == 0 && ok;
}

void remove(const std::string &path) {
  if (card_) ::unlink(path.c_str());
}

}  // namespace esphome::skyscope::card_io

#else
// A board without a card slot: nothing is ever on the card.
namespace esphome::skyscope::card_io {
bool mount(const Config &, std::string &error) {
  error = "this board has no card slot";
  return false;
}
bool mounted() { return false; }
uint32_t errors() { return 0; }
bool space(uint64_t &, uint64_t &) { return false; }
bool read(const sky::tiles::Key &, sky::card::Header &, sky::http::Body &body) {
  body.clear();
  return false;
}
bool read_header(const sky::tiles::Key &, sky::card::Header &) { return false; }
bool write(const sky::tiles::Key &, const sky::card::Header &, const uint8_t *, size_t) { return false; }
bool read_text(const std::string &, std::string &text) {
  text.clear();
  return false;
}
bool write_text(const std::string &, const std::string &) { return false; }
void remove(const std::string &) {}
}  // namespace esphome::skyscope::card_io
#endif
