#pragma once
// The screen's own radio: an RTL-SDR stick on the board's USB port (rtlsdr.h), listening at 1090 MHz. Three tasks on
// the worker's core (core 0), all above the worker so a tile decode cannot make the radio fall behind:
//
// - "usb": ESP-IDF's USB host library, which finds the stick when it is plugged in.
// - "radio": opens and tunes the stick, then keeps its bulk transfers coming, each one's bytes copied into a stream
//   buffer in PSRAM (half a second of samples) and the transfer sent again. A stick pulled out is waited for again.
// - "demod": takes the samples from the buffer, finds the messages in them (modes.h) and adds them to the planes
//   (adsb.h).
//
// The worker asks for the planes as it would ask an exchange (settings.h RADIO), and the web page for how the radio is
// doing (stats()). Neither waits on the radio for more than the copy.
#ifdef USE_SKYSCOPE_RADIO
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <usb/usb_host.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "adsb.h"
#include "feed.h"
#include "modes.h"
#include "rtlsdr.h"

namespace esphome::skyscope {

class Radio {
 public:
  struct Config {
    int gain_tenths = 496;  // the tuner's gain in tenths of a dB; negative for its own automatic gain
    int ppm = 0;            // the stick's crystal, off by this many parts a million
  };
  // How the radio is doing, for /api/status.
  struct Stats {
    std::string state;      // what it is doing, in words
    bool streaming = false;
    const char *tuner = "none";
    int gain_tenths = 0;
    float samples_per_s = 0, messages_per_s = 0;
    uint32_t messages = 0, mended = 0, rejected = 0, unknown = 0;
    uint32_t positions = 0, bad_positions = 0;
    uint64_t dropped_bytes = 0;  // samples lost because the demodulator fell behind
    uint32_t transfer_errors = 0;
    uint32_t noise = 0;          // the demodulator's noise floor, in modes::Powers units
    uint32_t planes = 0, placed = 0;
  };

  // The tasks started; the stick is opened whenever one is plugged in.
  void start(const Config &config);
  // Before a restart: the stream stopped, the stick closed and the port switched off, waited for a moment. A restart
  // with the stick's transfers still in flight hung until the chip's watchdog reset it, and the firmware just
  // installed was thrown away for it.
  void stop();
  bool streaming() const { return streaming_; }
  std::string state() {
    std::lock_guard<std::mutex> guard(lock_);
    return state_;
  }
  // The planes heard with a place in the last minute.
  std::vector<sky::feed::Flight> flights(uint32_t now);
  // The receiver's place (the screen's): surface positions are worked out from it, and far-off ones thrown away.
  void set_reference(double lat, double lon);
  Stats stats();

 private:
  static void usb_main(void *arg);
  static void radio_main(void *arg);
  static void demod_main(void *arg);
  static void on_client_event(const usb_host_client_event_msg_t *msg, void *arg);
  static void on_stream(usb_transfer_t *t);

  void set_state(const std::string &words);
  void run_stick(uint8_t address);

  Config config_;
  usb_host_client_handle_t client_ = nullptr;
  StreamBufferHandle_t stream_ = nullptr;
  StaticStreamBuffer_t stream_struct_{};
  uint8_t *stream_storage_ = nullptr;

  std::atomic<int> new_address_{0};  // a device just plugged in, or 0
  std::atomic<bool> gone_{false};    // the stick open now was pulled out
  std::atomic<bool> streaming_{false};
  std::atomic<bool> stopping_{false}, stopped_{false};  // a restart coming (stop()), and the radio ready for it
  bool streamed_ = false;  // run_stick got the stick streaming (the radio task's own)
  std::atomic<int> in_flight_{0};
  std::atomic<uint64_t> bytes_{0}, dropped_{0};
  std::atomic<uint32_t> transfer_errors_{0};
  uint32_t last_bytes_at_ = 0;

  std::mutex lock_;  // the tracker, the counts and the state words
  sky::adsb::Tracker tracker_;
  sky::modes::Counts demod_counts_;
  uint32_t noise_ = 0;
  std::string state_ = "Starting";
  const char *tuner_ = "none";
  int gain_ = 0;
  // For the rates in stats(): the counts the last time it was asked.
  uint32_t rate_at_ = 0;
  uint64_t rate_samples_ = 0;
  uint32_t rate_messages_ = 0;
  float samples_per_s_ = 0, messages_per_s_ = 0;
};

}  // namespace esphome::skyscope
#endif
