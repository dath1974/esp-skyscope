#pragma once
// An RTL2832U USB radio with a Rafael R820T or R820T2 tuner (the FlightAware Pro Stick and Pro Stick Plus, and most
// RTL-SDR sticks), driven straight over ESP-IDF's USB host: set up to hand over raw I/Q samples, tuned, and streamed.
// radio.cpp runs it.
//
// Neither chip has a public datasheet. The registers and their values are those the osmocom rtl-sdr project found and
// documented (librtlsdr, its r82xx tuner driver); the code that writes them is the screen's own.
#include <usb/usb_host.h>

#include <cstdint>
#include <string>

namespace esphome::skyscope::rtl {

constexpr uint16_t VENDOR = 0x0BDA;  // Realtek
inline bool is_rtl(uint16_t vid, uint16_t pid) { return vid == VENDOR && (pid == 0x2832 || pid == 0x2838); }
constexpr uint8_t STREAM_ENDPOINT = 0x81;  // bulk IN: the samples, I then Q, a byte each

class Dongle {
 public:
  // The device at `address`: opened, its interface claimed, the RTL2832U set up to hand over raw samples, and its tuner
  // found and set up. The first step that fails says why in error().
  bool open(usb_host_client_handle_t client, uint8_t address);
  // Tuned to `hz` and sampling at `rate` a second, the tuner's gain fixed at `tenths` of a dB (its own automatic gain
  // for a negative one), the crystal corrected by `ppm`; its buffer emptied, so the stream starts afresh.
  bool tune(uint32_t hz, uint32_t rate, int tenths, int ppm);
  void close();

  usb_device_handle_t device() const { return dev_; }
  const std::string &error() const { return error_; }
  const char *tuner() const { return tuner_; }
  int gain_tenths() const { return gain_; }  // the gain the tuner was set to, from its steps

 private:
  // The RTL2832U's register blocks, and its control transfers: a vendor request 0 with the register's address as wValue
  // and its block (and 0x10 for a write) in wIndex.
  enum Block : uint8_t { DEMOD = 0, USB = 1, SYS = 2, TUN = 3, ROM = 4, IR = 5, IIC = 6 };
  bool control(bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len);
  bool read_array(Block block, uint16_t addr, uint8_t *data, uint8_t len);
  bool write_array(Block block, uint16_t addr, const uint8_t *data, uint8_t len);
  bool write_reg(Block block, uint16_t addr, uint16_t value, uint8_t len);
  // The demodulator's registers, in pages: each write is followed by a read of page 0x0a, register 1, as librtlsdr's
  // is (the chip wants it).
  bool demod_write(uint8_t page, uint16_t addr, uint16_t value, uint8_t len);
  bool demod_read(uint8_t page, uint16_t addr, uint8_t &value);
  // The demodulator passes I2C through to the tuner only while its repeater is on.
  bool repeater(bool on);
  bool i2c_write(uint8_t i2c_addr, const uint8_t *data, uint8_t len);
  bool i2c_read(uint8_t i2c_addr, uint8_t *data, uint8_t len);

  bool init_baseband();
  bool set_if_freq(uint32_t hz);
  bool set_sample_rate(uint32_t rate);
  bool set_ppm(int ppm);
  bool reset_buffer();

  // The R820T: its registers 5 to 0x1f kept here as written (it reads back only from 0, bit-reversed).
  bool tuner_write(uint8_t reg, const uint8_t *data, uint8_t len);
  bool tuner_write_mask(uint8_t reg, uint8_t value, uint8_t mask);
  bool tuner_read(uint8_t *data, uint8_t len);
  bool tuner_init();
  bool tuner_set_pll(uint32_t hz, bool &locked);
  bool tuner_set_freq(uint32_t hz);
  bool tuner_set_gain(int tenths);

  bool fail(const char *what);

  usb_host_client_handle_t client_ = nullptr;
  usb_device_handle_t dev_ = nullptr;
  usb_transfer_t *ctrl_ = nullptr;
  volatile bool ctrl_done_ = false;
  bool claimed_ = false;
  std::string error_;
  const char *tuner_ = "none";
  uint8_t regs_[0x20] = {};
  uint32_t xtal_ = 28800000;  // the stick's crystal, shared by both chips, corrected by ppm
  uint32_t if_hz_ = 3570000;  // the tuner's intermediate frequency
  int gain_ = 0;
};

}  // namespace esphome::skyscope::rtl
