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
#include <ff.h>
#include <diskio_sdmmc.h>  // after ff.h, whose types it uses
#include <sdmmc_cmd.h>
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#endif

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace esphome::skyscope::card_io {
static const char *const TAG = "skyscope.card";

namespace {
sdmmc_card_t *card_ = nullptr;
uint32_t errors_ = 0;
bool unformatted_ = false;
// The count under way (tally_step): FAT's folders open, the outermost first, each with what its files count as.
enum Holds : int8_t { MAPS = -1, LOG = -2 };  // or a map's tiles, by its style
struct Folder {
  FF_DIR dir;
  std::string path;
  int8_t holds;
};
std::vector<Folder> walk_;
int walk_stage_ = 0;  // 0 the tiles, 1 the log, 2 done
uint32_t walk_cluster_ = 0;
// The count let go without closing its folders: the card is gone, and FAT's own record of it with it.
void forget_walk() {
  walk_.clear();
  walk_stage_ = 2;
}
// Failures in a row before the card is taken as gone.
constexpr uint32_t MOST_ERRORS = 8;

void failed(const char *what, const std::string &path) {
  ++errors_;
  ESP_LOGW(TAG, "%s %s: %s", what, path.c_str(), strerror(errno));
  if (errors_ >= MOST_ERRORS && card_) {
    ESP_LOGW(TAG, "The card no longer answers: unmounted");
    forget_walk();
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

// Clusters of 32 KB: a tile (most are 10 to 40 KB) wastes little, and the FAT of a large card stays small enough to
// read its free space quickly.
constexpr size_t CLUSTER_BYTES = 32 * 1024;

bool mount(const Config &c, std::string &error, bool format) {
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
  m.format_if_mount_failed = format;  // else a card with something else on it is left alone
  m.max_files = 2;
  m.allocation_unit_size = CLUSTER_BYTES;
  // The bus as the board states it first. A card that answers but garbles (a checksum that fails, a reply cut short)
  // is asked again more gently: at the standard 20 MHz, then on one data line, where a long trace, a tired card or an
  // adapter still gets through. A slot with no card in it, or a card with no FAT32 on it, is not asked again: neither
  // is the bus's fault.
  struct Try {
    int width, khz;
  };
  std::vector<Try> tries{{c.width, c.khz}};
  if (c.khz > SDMMC_FREQ_DEFAULT) tries.push_back({c.width, SDMMC_FREQ_DEFAULT});
  if (c.width > 1) tries.push_back({1, SDMMC_FREQ_DEFAULT});
  esp_err_t err = ESP_FAIL;
  for (size_t i = 0; i < tries.size(); ++i) {
    host.max_freq_khz = tries[i].khz;
    slot.width = tries[i].width;
    err = esp_vfs_fat_sdmmc_mount(sky::card::ROOT, &host, &slot, &m, &card_);
    if (err == ESP_OK || err == ESP_FAIL || err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) break;
    card_ = nullptr;
    if (i + 1 < tries.size())
      ESP_LOGW(TAG, "The card did not answer cleanly on %d data line%s at %d MHz (%s): trying %d at %d MHz",
               tries[i].width, tries[i].width == 1 ? "" : "s", tries[i].khz / 1000, esp_err_to_name(err),
               tries[i + 1].width, tries[i + 1].khz / 1000);
  }
  unformatted_ = err == ESP_FAIL;
  if (err != ESP_OK) {
    card_ = nullptr;
    error = err == ESP_FAIL ? "the card is not FAT32: format it to use it"
          : err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ? "no card in the slot"
          : std::string("the card does not answer properly (") + esp_err_to_name(err) +
                "): try it in again, or another";
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

bool format(std::string &error) {
  if (!card_) {
    error = "No map card";
    return false;
  }
  esp_vfs_fat_mount_config_t m{};
  m.max_files = 2;
  m.allocation_unit_size = CLUSTER_BYTES;
  ESP_LOGI(TAG, "Formatting the map card");
  tally_start();
  forget_walk();
  const esp_err_t err = esp_vfs_fat_sdcard_format_cfg(sky::card::ROOT, card_, &m);
  if (err != ESP_OK) {
    error = std::string("the card could not be formatted (") + esp_err_to_name(err) + ")";
    ESP_LOGW(TAG, "%s", error.c_str());
    return false;
  }
  errors_ = 0;
  ::mkdir((std::string(sky::card::ROOT) + "/tiles").c_str(), 0775);
  return true;
}

bool mounted() { return card_ != nullptr; }
bool unformatted() { return unformatted_; }
uint32_t errors() { return errors_; }

bool space(uint64_t &total, uint64_t &free, uint32_t &cluster) {
  if (!card_) return false;
  if (esp_vfs_fat_info(sky::card::ROOT, &total, &free) != ESP_OK) return false;
  // The cluster from FAT itself (its free count is held from the call above, so this reads nothing more).
  const char drive[3] = {(char) ('0' + ff_diskio_get_pdrv_card(card_)), ':', 0};
  DWORD clusters = 0;
  FATFS *fs = nullptr;
  cluster = 0;
  if (f_getfree(drive, &clusters, &fs) == FR_OK && fs) {
#if FF_MAX_SS != FF_MIN_SS
    cluster = (uint32_t) fs->csize * fs->ssize;
#else
    cluster = (uint32_t) fs->csize * FF_MAX_SS;
#endif
  }
  return true;
}

bool about(sky::card::About &out) {
  if (!card_) return false;
  out = sky::card::About{};
  out.name = card_->cid.name;
  out.maker = (uint8_t) card_->cid.mfg_id;
  out.made = (uint16_t) card_->cid.date;
  out.mmc = card_->is_mmc;
  out.high_capacity = (card_->ocr & SD_OCR_SDHC_CAP) != 0;
  out.bytes = (uint64_t) card_->csd.capacity * card_->csd.sector_size;
  out.khz = card_->real_freq_khz ? (uint32_t) card_->real_freq_khz : card_->max_freq_khz;
  out.lines = 1 << card_->log_bus_width;
  return true;
}

void tally_start() {
  if (card_)
    for (Folder &f : walk_) f_closedir(&f.dir);
  walk_.clear();
  walk_stage_ = 0;
  uint64_t total = 0, free = 0;
  if (!space(total, free, walk_cluster_)) walk_cluster_ = 0;
}

int tally_step(sky::card::Tally &t, int budget) {
  if (!card_) {
    forget_walk();
    return -1;
  }
  // FAT's own paths: the card's drive, then the folder.
  const std::string drive = std::string(1, (char) ('0' + ff_diskio_get_pdrv_card(card_))) + ":";
  auto open = [&](const std::string &path, int8_t holds) {
    if (walk_.size() >= 6) return;  // deeper than the screen makes them: left uncounted
    walk_.push_back(Folder{{}, path, holds});
    if (f_opendir(&walk_.back().dir, (drive + path).c_str()) != FR_OK) walk_.pop_back();
  };
  FILINFO info;
  while (budget-- > 0) {
    if (walk_.empty()) {
      if (walk_stage_ >= 2) return 1;
      open(walk_stage_ == 0 ? "/tiles" : "/log", walk_stage_ == 0 ? MAPS : LOG);
      ++walk_stage_;
      continue;
    }
    Folder &top = walk_.back();
    if (f_readdir(&top.dir, &info) != FR_OK || !info.fname[0]) {
      f_closedir(&top.dir);
      walk_.pop_back();
      continue;
    }
    if (!std::strcmp(info.fname, ".") || !std::strcmp(info.fname, "..")) continue;
    const int8_t holds = top.holds;
    if (info.fattrib & AM_DIR) {
      uint8_t style = 0;
      if (holds != MAPS) open(top.path + "/" + info.fname, holds);
      else if (sky::card::style_folder(info.fname, style)) open(top.path + "/" + info.fname, (int8_t) style);
      continue;
    }
    const uint64_t bytes = sky::card::on_card(info.fsize, walk_cluster_);
    if (holds == LOG) {
      ++t.logs;
      t.log_bytes += bytes;
    } else if (holds >= 0) {
      ++t.tiles[holds];
      t.bytes[holds] += bytes;
    }
    // A file in the tiles folder itself (a download's record, a write under way) is no map's.
  }
  return 0;
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
  const std::string tmp = sky::card::temp_path();
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
  // As a tile is: the whole of it, or the one before.
  const std::string tmp = sky::card::temp_path();
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
  if (fd < 0) return false;
  const bool ok = write_all(fd, reinterpret_cast<const uint8_t *>(text.data()), text.size());
  if (::close(fd) != 0 || !ok) return false;
  ::unlink(path.c_str());
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

bool append_text(const std::string &path, const std::string &text) {
  if (!card_) return false;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd < 0) return false;
  const bool ok = write_all(fd, reinterpret_cast<const uint8_t *>(text.data()), text.size());
  return ::close(fd) == 0 && ok;
}

void remove(const std::string &path) {
  if (card_) ::unlink(path.c_str());
}

bool rename(const std::string &from, const std::string &to) { return card_ && ::rename(from.c_str(), to.c_str()) == 0; }
bool make_folder(const std::string &path) {
  if (!card_) return false;
  struct stat st {};
  return ::mkdir(path.c_str(), 0775) == 0 || (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
}

int remove_some(const std::string &folder, int budget) {
  if (!card_) return -1;
  DIR *d = ::opendir(folder.c_str());
  if (!d) return -1;
  // Its entries first, then deleted: a folder is not changed while it is read.
  std::vector<std::pair<std::string, bool>> entries;
  while ((int) entries.size() < budget) {
    const dirent *e = ::readdir(d);
    if (!e) break;
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    entries.push_back({folder + "/" + e->d_name, e->d_type == DT_DIR});
  }
  ::closedir(d);
  int removed = 0;
  for (const auto &[path, is_folder] : entries) {
    if (removed >= budget) break;
    if (is_folder) {
      const int n = remove_some(path, budget - removed);
      if (n > 0) removed += n;
    } else if (::unlink(path.c_str()) == 0) {
      ++removed;
    }
  }
  // Empty now (else this fails and it is left for the next turn).
  if (removed < budget && ::rmdir(folder.c_str()) == 0) ++removed;
  return removed;
}

}  // namespace esphome::skyscope::card_io

#else
// A board without a card slot: nothing is ever on the card.
namespace esphome::skyscope::card_io {
bool mount(const Config &, std::string &error, bool) {
  error = "this board has no card slot";
  return false;
}
bool format(std::string &error) {
  error = "this board has no card slot";
  return false;
}
bool rename(const std::string &, const std::string &) { return false; }
bool make_folder(const std::string &) { return false; }
bool append_text(const std::string &, const std::string &) { return false; }
bool unformatted() { return false; }
int remove_some(const std::string &, int) { return -1; }
bool mounted() { return false; }
uint32_t errors() { return 0; }
bool space(uint64_t &, uint64_t &, uint32_t &) { return false; }
bool about(sky::card::About &) { return false; }
void tally_start() {}
int tally_step(sky::card::Tally &, int) { return -1; }
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
