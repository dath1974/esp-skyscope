#pragma once
// The map card itself (card.cpp): mounted, read and written by the worker task alone. What goes where and how long it
// is good for is card.h.
#include <cstdint>
#include <string>

#include "card.h"
#include "http.h"

namespace esphome::skyscope::card_io {

// The slot the card is in, from the board's YAML (__init__.py). Slot 0 on the ESP32-P4 has pins of its own (IO MUX);
// any other slot takes its pins through the GPIO matrix. `hold_high` is a pin driven high before the card is started:
// on a 1-bit bus, the card's DAT3 (its SPI select), which must be high for the card to talk SD.
struct Config {
  int slot = 1, width = 1;
  int clk = -1, cmd = -1, d0 = -1, d1 = -1, d2 = -1, d3 = -1;
  int hold_high = -1;
  int power_ldo = -1;  // the P4's on-chip LDO that powers the slot, -1 for none
  int khz = 20000;
};

// Mounts the card at card::ROOT, or false with why not (no card, or one that is not FAT32). With `format`, a card that
// has no FAT32 on it (a new card in exFAT, or a blank one) is formatted FAT32 instead.
bool mount(const Config &config, std::string &error, bool format = false);
// Formats the card mounted (FAT32), erasing everything on it, and mounts it again.
bool format(std::string &error);
bool mounted();
// The last mount found a card, with no FAT32 on it: it can be formatted.
bool unformatted();
// The card's size and free bytes, and its clusters' bytes (false when it is not mounted).
bool space(uint64_t &total, uint64_t &free, uint32_t &cluster);

// A tile's header and bytes (into `body`, without the header), or false: not on the card, or not readable.
bool read(const sky::tiles::Key &k, sky::card::Header &h, sky::http::Body &body);
// Its header alone, for a download that skips what the card has.
bool read_header(const sky::tiles::Key &k, sky::card::Header &h);
// Writes a tile: to card::temp_path first, then renamed into its place, so a tile half written (the power gone) is
// never read as whole, and leaves nothing behind but that one file.
bool write(const sky::tiles::Key &k, const sky::card::Header &h, const uint8_t *data, size_t size);

// A small text file at `path` (a download's record).
bool read_text(const std::string &path, std::string &text);
bool write_text(const std::string &path, const std::string &text);
// `text` added to the end of the file at `path`, made if it is not there (the spotting log, sightings.h).
bool append_text(const std::string &path, const std::string &text);
void remove(const std::string &path);
// Moves a file or folder; makes a folder (true when it is there already).
bool rename(const std::string &from, const std::string &to);
bool make_folder(const std::string &path);
// Deletes up to `budget` files and folders under `folder`, and the folder itself once it is empty: how many it
// deleted, or -1 when the folder is not there (all gone).
int remove_some(const std::string &folder, int budget);

// Reads and writes that failed in a row: after a few the card is taken as pulled out, and unmounted.
uint32_t errors();

}  // namespace esphome::skyscope::card_io
