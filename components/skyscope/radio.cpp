#ifdef USE_SKYSCOPE_RADIO
#include "radio.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <cstring>

namespace esphome::skyscope {

static const char *const TAG = "skyscope.radio";

namespace {
constexpr uint32_t FREQUENCY = 1090000000;
constexpr int TRANSFERS = 8;               // bulk transfers in flight
constexpr size_t TRANSFER_BYTES = 16384;   // each: 4 ms of samples, a whole number of 512-byte packets
constexpr size_t STREAM_BYTES = 2u << 20;  // half a second of samples, in PSRAM
constexpr size_t BLOCK_BYTES = 65536;      // what the demodulator takes at a time: 16 ms
constexpr uint32_t STALL_MS = 3000;        // no samples for this long: the stick is opened afresh
constexpr int CORE = 0;                    // the worker's
constexpr UBaseType_t USB_PRIORITY = 6, RADIO_PRIORITY = 5, DEMOD_PRIORITY = 4;  // the worker runs at 3
}  // namespace

void Radio::start(const Config &config) {
  config_ = config;
  stream_storage_ = static_cast<uint8_t *>(heap_caps_malloc(STREAM_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!stream_storage_) {
    set_state("No PSRAM for the radio's samples");
    return;
  }
  stream_ = xStreamBufferCreateStatic(STREAM_BYTES, BLOCK_BYTES / 4, stream_storage_, &stream_struct_);
  usb_host_config_t host{};
  host.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&host) != ESP_OK) {
    set_state("The USB host did not start");
    return;
  }
  xTaskCreatePinnedToCore(usb_main, "usb", 4096, this, USB_PRIORITY, nullptr, CORE);
  xTaskCreatePinnedToCore(radio_main, "radio", 6144, this, RADIO_PRIORITY, nullptr, CORE);
  xTaskCreatePinnedToCore(demod_main, "demod", 6144, this, DEMOD_PRIORITY, nullptr, CORE);
}

void Radio::set_state(const std::string &words) {
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (state_ == words) return;
    state_ = words;
  }
  ESP_LOGI(TAG, "%s", words.c_str());
}

void Radio::usb_main(void *arg) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    // With no client and no device left the library would like to be freed; the radio's client stays, so it never is.
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void Radio::on_client_event(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<Radio *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) self->new_address_ = msg->new_dev.address;
  else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) self->gone_ = true;
}

void Radio::radio_main(void *arg) {
  auto *self = static_cast<Radio *>(arg);
  usb_host_client_config_t config{};
  config.is_synchronous = false;
  config.max_num_event_msg = 5;
  config.async.client_event_callback = on_client_event;
  config.async.callback_arg = self;
  if (usb_host_client_register(&config, &self->client_) != ESP_OK) {
    self->set_state("The USB host refused the radio");
    vTaskDelete(nullptr);
    return;
  }
  self->set_state("No radio: plug in the stick");
  for (;;) {
    usb_host_client_handle_events(self->client_, pdMS_TO_TICKS(500));
    const int address = self->new_address_.exchange(0);
    if (address) self->run_stick((uint8_t) address);
  }
}

// A device plugged in at `address`: if it is a stick, opened, tuned and streamed until it is pulled out or stalls.
void Radio::run_stick(uint8_t address) {
  rtl::Dongle stick;
  gone_ = false;
  // Its descriptor first: anything else on the port is left alone.
  usb_device_handle_t dev;
  if (usb_host_device_open(client_, address, &dev) != ESP_OK) return;
  const usb_device_desc_t *desc = nullptr;
  usb_host_get_device_descriptor(dev, &desc);
  const uint16_t vid = desc ? desc->idVendor : 0, pid = desc ? desc->idProduct : 0;
  usb_device_info_t info{};
  usb_host_device_info(dev, &info);
  usb_host_device_close(client_, dev);
  if (!rtl::is_rtl(vid, pid)) {
    char words[80];
    std::snprintf(words, sizeof(words), "Not a radio on the USB port (%04x:%04x)", vid, pid);
    set_state(words);
    return;
  }
  if (info.speed != USB_SPEED_HIGH) {
    set_state("The radio is not on a high-speed USB port: it cannot keep up");
    return;
  }
  set_state("Opening the radio");
  if (!stick.open(client_, address) || !stick.tune(FREQUENCY, sky::modes::RATE, config_.gain_tenths, config_.ppm)) {
    set_state(stick.error());
    stick.close();
    return;
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    tuner_ = stick.tuner();
    gain_ = stick.gain_tenths();
  }
  xStreamBufferReset(stream_);

  // The stream: every transfer in flight at once, each sent again as it comes back (on_stream).
  usb_transfer_t *transfers[TRANSFERS] = {};
  in_flight_ = 0;
  for (auto &t : transfers) {
    if (usb_host_transfer_alloc(TRANSFER_BYTES, 0, &t) != ESP_OK) {
      t = nullptr;
      break;
    }
    t->device_handle = stick.device();
    t->bEndpointAddress = rtl::STREAM_ENDPOINT;
    t->num_bytes = TRANSFER_BYTES;
    t->callback = on_stream;
    t->context = this;
    t->timeout_ms = 0;
    if (usb_host_transfer_submit(t) == ESP_OK) ++in_flight_;
  }
  if (in_flight_ == 0) {
    set_state("The radio's stream would not start");
  } else {
    streaming_ = true;
    set_state("Listening at 1090 MHz");
    last_bytes_at_ = millis();
    uint64_t seen = bytes_;
    while (!gone_) {
      usb_host_client_handle_events(client_, pdMS_TO_TICKS(100));
      const uint64_t now_bytes = bytes_;
      if (now_bytes != seen) {
        seen = now_bytes;
        last_bytes_at_ = millis();
      } else if (millis() - last_bytes_at_ > STALL_MS) {
        set_state("The radio stopped sending samples: opening it again");
        break;
      }
      if (in_flight_ == 0) break;
    }
    streaming_ = false;
    if (gone_) set_state("The radio was unplugged: plug it in again");
  }
  // The transfers back before they are freed: a halt cancels them, a stick pulled out ends them.
  if (!gone_) {
    usb_host_endpoint_halt(stick.device(), rtl::STREAM_ENDPOINT);
    usb_host_endpoint_flush(stick.device(), rtl::STREAM_ENDPOINT);
  }
  const uint32_t waiting = millis();
  while (in_flight_ > 0 && millis() - waiting < 2000) usb_host_client_handle_events(client_, pdMS_TO_TICKS(50));
  if (!gone_) usb_host_endpoint_clear(stick.device(), rtl::STREAM_ENDPOINT);
  for (auto *t : transfers)
    if (t) usb_host_transfer_free(t);
  stick.close();
  // A stall rather than a pull: the same stick is opened again at once.
  if (!gone_) new_address_ = address;
}

void Radio::on_stream(usb_transfer_t *t) {
  auto *self = static_cast<Radio *>(t->context);
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    // Whole I/Q pairs only, so the buffer never falls out of step.
    const size_t n = (size_t) t->actual_num_bytes & ~(size_t) 1;
    self->bytes_ += n;
    const size_t sent = n ? xStreamBufferSend(self->stream_, t->data_buffer, n, 0) : 0;
    if (sent < n) self->dropped_ += n - sent;
  } else if (t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) {
    --self->in_flight_;
    return;
  } else {
    ++self->transfer_errors_;
  }
  if (self->gone_ || !self->streaming_ || usb_host_transfer_submit(t) != ESP_OK) --self->in_flight_;
}

void Radio::demod_main(void *arg) {
  auto *self = static_cast<Radio *>(arg);
  static const sky::modes::Powers powers;
  sky::modes::Demodulator demod;
  auto *block = static_cast<uint8_t *>(heap_caps_malloc(BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  auto *power = static_cast<uint16_t *>(heap_caps_malloc(BLOCK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!block || !power) {
    self->set_state("No PSRAM for the radio's demodulator");
    vTaskDelete(nullptr);
    return;
  }
  uint32_t expired_at = millis();
  for (;;) {
    const size_t n = xStreamBufferReceive(self->stream_, block, BLOCK_BYTES, pdMS_TO_TICKS(200)) & ~(size_t) 1;
    const uint32_t now = millis();
    if (n) {
      powers.convert(block, n / 2, power);
      demod.feed(power, n / 2, [&](const sky::modes::Message &m) {
        std::lock_guard<std::mutex> guard(self->lock_);
        return self->tracker_.add(m, now);
      });
    }
    std::lock_guard<std::mutex> guard(self->lock_);
    self->demod_counts_ = demod.counts();
    self->noise_ = demod.noise();
    if (now - expired_at >= 1000) {
      expired_at = now;
      self->tracker_.expire(now);
    }
  }
}

std::vector<sky::feed::Flight> Radio::flights(uint32_t now) {
  std::lock_guard<std::mutex> guard(lock_);
  return tracker_.flights(now);
}

void Radio::set_reference(double lat, double lon) {
  std::lock_guard<std::mutex> guard(lock_);
  tracker_.set_reference(lat, lon);
}

Radio::Stats Radio::stats() {
  std::lock_guard<std::mutex> guard(lock_);
  Stats s;
  s.state = state_;
  s.streaming = streaming_;
  s.tuner = tuner_;
  s.gain_tenths = gain_;
  s.messages = demod_counts_.accepted;
  s.mended = demod_counts_.mended;
  s.rejected = demod_counts_.rejected;
  s.unknown = tracker_.counts().unknown;
  s.positions = tracker_.counts().positions;
  s.bad_positions = tracker_.counts().bad_positions;
  s.dropped_bytes = dropped_;
  s.transfer_errors = transfer_errors_;
  s.noise = noise_;
  const uint32_t now = millis();
  s.planes = (uint32_t) tracker_.size();
  s.placed = (uint32_t) tracker_.flights(now).size();
  // The rates over the time since the last ask, at least two seconds.
  if (now - rate_at_ >= 2000) {
    const float seconds = (now - rate_at_) / 1000.0f;
    if (rate_at_) {
      samples_per_s_ = (demod_counts_.samples - rate_samples_) / seconds;
      messages_per_s_ = (demod_counts_.accepted - rate_messages_) / seconds;
    }
    rate_at_ = now;
    rate_samples_ = demod_counts_.samples;
    rate_messages_ = demod_counts_.accepted;
  }
  s.samples_per_s = samples_per_s_;
  s.messages_per_s = messages_per_s_;
  return s;
}

}  // namespace esphome::skyscope
#endif
