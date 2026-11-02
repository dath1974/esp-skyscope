#ifdef USE_SKYSCOPE_RADIO
#include "rtlsdr.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

namespace esphome::skyscope::rtl {

static const char *const TAG = "skyscope.rtl";

namespace {
constexpr uint32_t CTRL_TIMEOUT_MS = 300;
constexpr size_t CTRL_DATA = 64;  // the most a control transfer here carries; the stick's control packets are 64 bytes

// The RTL2832U's USB and system registers.
constexpr uint16_t USB_SYSCTL = 0x2000, USB_EPA_CTL = 0x2148, USB_EPA_MAXPKT = 0x2158;
constexpr uint16_t DEMOD_CTL = 0x3000, DEMOD_CTL_1 = 0x300b;

// The demodulator's low-pass filter for raw samples (librtlsdr's default): 8 coefficients of 8 bits, then 8 of 12.
constexpr int FIR[16] = {-54, -36, -41, -40, -32, -14, 14, 53, 101, 156, 215, 273, 327, 372, 404, 421};

// The R820T, on the stick's I2C bus; its register 0 reads this.
constexpr uint8_t R820T_ADDR = 0x34, R820T_ID = 0x69;
constexpr uint8_t R820T_FIRST = 0x05;
// Registers 5 to 0x1f as the tuner starts.
constexpr uint8_t R820T_INIT[] = {0x83, 0x32, 0x75, 0xc0, 0x40, 0xd6, 0x6c, 0xf5, 0x63, 0x75, 0x68, 0x6c, 0x83, 0x80,
                                  0x00, 0x0f, 0x00, 0xc0, 0x30, 0x48, 0xcc, 0x60, 0x00, 0x54, 0xae, 0x4a, 0xc0};
// Its LNA and mixer gain steps, in tenths of a dB, taken alternately up to the gain asked for.
constexpr int LNA_STEPS[] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
constexpr int MIXER_STEPS[] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};

uint8_t reversed(uint8_t b) {
  b = (uint8_t) ((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = (uint8_t) ((b & 0xCC) >> 2 | (b & 0x33) << 2);
  return (uint8_t) ((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
}  // namespace

bool Dongle::fail(const char *what) {
  error_ = what;
  ESP_LOGE(TAG, "%s", what);
  return false;
}

// ---------------------------------------------------------------- USB

bool Dongle::control(bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len) {
  if (len > CTRL_DATA) return false;
  auto *setup = reinterpret_cast<usb_setup_packet_t *>(ctrl_->data_buffer);
  setup->bmRequestType = in ? 0xC0 : 0x40;  // vendor, device; in or out
  setup->bRequest = 0;
  setup->wValue = value;
  setup->wIndex = index;
  setup->wLength = len;
  if (!in && len) std::memcpy(ctrl_->data_buffer + sizeof(usb_setup_packet_t), data, len);
  // An IN data stage is asked for in whole packets.
  ctrl_->num_bytes = sizeof(usb_setup_packet_t) + (in ? usb_round_up_to_mps(len, 64) : len);
  ctrl_->device_handle = dev_;
  ctrl_->bEndpointAddress = 0;
  ctrl_->timeout_ms = CTRL_TIMEOUT_MS;
  ctrl_->context = this;
  ctrl_->callback = [](usb_transfer_t *t) { static_cast<Dongle *>(t->context)->ctrl_done_ = true; };
  ctrl_done_ = false;
  if (usb_host_transfer_submit_control(client_, ctrl_) != ESP_OK) return false;
  // The transfer finishes in this client's event handling, so it is pumped here until then.
  const uint32_t start = millis();
  while (!ctrl_done_) {
    usb_host_client_handle_events(client_, pdMS_TO_TICKS(10));
    if (millis() - start > CTRL_TIMEOUT_MS + 200) return false;
  }
  if (ctrl_->status != USB_TRANSFER_STATUS_COMPLETED) return false;
  if (in) {
    if (ctrl_->actual_num_bytes < (int) (sizeof(usb_setup_packet_t) + len)) return false;
    std::memcpy(data, ctrl_->data_buffer + sizeof(usb_setup_packet_t), len);
  }
  return true;
}

bool Dongle::read_array(Block block, uint16_t addr, uint8_t *data, uint8_t len) {
  return control(true, addr, (uint16_t) (block << 8), data, len);
}
bool Dongle::write_array(Block block, uint16_t addr, const uint8_t *data, uint8_t len) {
  return control(false, addr, (uint16_t) (block << 8 | 0x10), const_cast<uint8_t *>(data), len);
}
bool Dongle::write_reg(Block block, uint16_t addr, uint16_t value, uint8_t len) {
  uint8_t data[2];
  if (len == 1) {
    data[0] = (uint8_t) value;
  } else {
    data[0] = (uint8_t) (value >> 8);
    data[1] = (uint8_t) value;
  }
  return write_array(block, addr, data, len);
}
bool Dongle::demod_write(uint8_t page, uint16_t addr, uint16_t value, uint8_t len) {
  uint8_t data[2];
  if (len == 1) {
    data[0] = (uint8_t) value;
  } else {
    data[0] = (uint8_t) (value >> 8);
    data[1] = (uint8_t) value;
  }
  if (!control(false, (uint16_t) (addr << 8 | 0x20), (uint16_t) (0x10 | page), data, len)) return false;
  uint8_t dummy;
  return demod_read(0x0a, 0x01, dummy);
}
bool Dongle::demod_read(uint8_t page, uint16_t addr, uint8_t &value) {
  return control(true, (uint16_t) (addr << 8 | 0x20), page, &value, 1);
}
bool Dongle::repeater(bool on) { return demod_write(1, 0x01, on ? 0x18 : 0x10, 1); }
bool Dongle::i2c_write(uint8_t i2c_addr, const uint8_t *data, uint8_t len) {
  return write_array(IIC, i2c_addr, data, len);
}
bool Dongle::i2c_read(uint8_t i2c_addr, uint8_t *data, uint8_t len) { return read_array(IIC, i2c_addr, data, len); }

// ---------------------------------------------------------------- the RTL2832U

bool Dongle::init_baseband() {
  // USB: the bulk endpoint's packets 512 bytes, its FIFO held in reset until the stream starts.
  if (!write_reg(USB, USB_SYSCTL, 0x09, 1)) return fail("No answer from the radio's USB registers");
  if (!write_reg(USB, USB_EPA_MAXPKT, 0x0002, 2) || !write_reg(USB, USB_EPA_CTL, 0x1002, 2))
    return fail("The radio refused its USB setup");
  // Power the demodulator on, then reset it.
  if (!write_reg(SYS, DEMOD_CTL_1, 0x22, 1) || !write_reg(SYS, DEMOD_CTL, 0xe8, 1))
    return fail("The radio's demodulator did not power on");
  bool ok = demod_write(1, 0x01, 0x14, 1) && demod_write(1, 0x01, 0x10, 1);
  // No spectrum inversion or adjacent channel rejection yet; no DDC shift or IF frequency.
  ok = ok && demod_write(1, 0x15, 0x00, 1) && demod_write(1, 0x16, 0x0000, 2);
  for (uint16_t i = 0; i < 6 && ok; ++i) ok = demod_write(1, 0x16 + i, 0x00, 1);
  // The low-pass filter: eight 8-bit coefficients, then eight 12-bit ones packed two to three bytes.
  uint8_t fir[20];
  for (int i = 0; i < 8; ++i) fir[i] = (uint8_t) (int8_t) FIR[i];
  for (int i = 0; i < 8; i += 2) {
    const int a = FIR[8 + i], b = FIR[8 + i + 1];
    fir[8 + i * 3 / 2] = (uint8_t) (a >> 4);
    fir[8 + i * 3 / 2 + 1] = (uint8_t) ((a << 4) | ((b >> 8) & 0x0f));
    fir[8 + i * 3 / 2 + 2] = (uint8_t) b;
  }
  for (int i = 0; i < 20 && ok; ++i) ok = demod_write(1, (uint16_t) (0x1c + i), fir[i], 1);
  // SDR mode with digital AGC off; the FSM state registers; RF and IF AGC loops off; no PID filter; the default ADC
  // datapath; zero-IF with DC and IQ correction (the R820T turns zero-IF off again); no 4.096 MHz clock out.
  ok = ok && demod_write(0, 0x19, 0x05, 1) && demod_write(1, 0x93, 0xf0, 1) && demod_write(1, 0x94, 0x0f, 1) &&
       demod_write(1, 0x11, 0x00, 1) && demod_write(1, 0x04, 0x00, 1) && demod_write(0, 0x61, 0x60, 1) &&
       demod_write(0, 0x06, 0x80, 1) && demod_write(1, 0xb1, 0x1b, 1) && demod_write(0, 0x0d, 0x83, 1);
  return ok || fail("The radio's demodulator refused its setup");
}

// The demodulator's IF, a negative fraction of the crystal in 22 bits.
bool Dongle::set_if_freq(uint32_t hz) {
  const int32_t f = (int32_t) -((double) hz * (1 << 22) / xtal_);
  return demod_write(1, 0x19, (uint16_t) ((f >> 16) & 0x3f), 1) &&
         demod_write(1, 0x1a, (uint16_t) ((f >> 8) & 0xff), 1) &&
         demod_write(1, 0x1b, (uint16_t) (f & 0xff), 1);
}

// The resampler's ratio of the crystal to the rate, in 22 fractional bits; then a reset.
bool Dongle::set_sample_rate(uint32_t rate) {
  uint32_t ratio = (uint32_t) (((uint64_t) 28800000 << 22) / rate);
  ratio &= 0x0ffffffc;
  return demod_write(1, 0x9f, (uint16_t) (ratio >> 16), 2) && demod_write(1, 0xa1, (uint16_t) (ratio & 0xffff), 2) &&
         demod_write(1, 0x01, 0x14, 1) && demod_write(1, 0x01, 0x10, 1);
}

// The resampler's correction for a crystal off by `ppm`, in 2^-24 steps.
bool Dongle::set_ppm(int ppm) {
  const int32_t offs = (int32_t) ((int64_t) -ppm * (1 << 24) / 1000000);
  return demod_write(1, 0x3f, (uint16_t) (offs & 0xff), 1) && demod_write(1, 0x3e, (uint16_t) ((offs >> 8) & 0x3f), 1);
}

bool Dongle::reset_buffer() {
  return write_reg(USB, USB_EPA_CTL, 0x1002, 2) && write_reg(USB, USB_EPA_CTL, 0x0000, 2);
}

// ---------------------------------------------------------------- the R820T

bool Dongle::tuner_write(uint8_t reg, const uint8_t *data, uint8_t len) {
  // Seven registers at a time: an I2C message here is at most eight bytes with the first register's number.
  for (uint8_t at = 0; at < len;) {
    const uint8_t n = (uint8_t) std::min<int>(7, len - at);
    uint8_t buf[8];
    buf[0] = (uint8_t) (reg + at);
    std::memcpy(buf + 1, data + at, n);
    if (!i2c_write(R820T_ADDR, buf, (uint8_t) (n + 1))) return false;
    for (uint8_t i = 0; i < n; ++i)
      if (reg + at + i < (int) sizeof(regs_)) regs_[reg + at + i] = data[at + i];
    at = (uint8_t) (at + n);
  }
  return true;
}
bool Dongle::tuner_write_mask(uint8_t reg, uint8_t value, uint8_t mask) {
  const uint8_t v = (uint8_t) ((regs_[reg] & ~mask) | (value & mask));
  return tuner_write(reg, &v, 1);
}
// The tuner reads back from register 0 only, each byte with its bits reversed.
bool Dongle::tuner_read(uint8_t *data, uint8_t len) {
  const uint8_t zero = 0;
  if (!i2c_write(R820T_ADDR, &zero, 1) || !i2c_read(R820T_ADDR, data, len)) return false;
  for (uint8_t i = 0; i < len; ++i) data[i] = reversed(data[i]);
  return true;
}

// The PLL to `hz`: the mixer's divider that puts the VCO in its range, then the integer and fractional (sigma-delta)
// parts of the multiple of the crystal; `locked` if it locked.
bool Dongle::tuner_set_pll(uint32_t hz, bool &locked) {
  locked = false;
  const uint32_t vco_min = 1770000, vco_max = vco_min * 2;  // kHz
  const uint32_t khz = (hz + 500) / 1000, ref = xtal_, ref_khz = (xtal_ + 500) / 1000;
  bool ok = tuner_write_mask(0x10, 0x00, 0x10) &&  // the crystal not divided by 2
            tuner_write_mask(0x1a, 0x00, 0x0c) &&  // PLL autotune 128 kHz
            tuner_write_mask(0x12, 0x80, 0xe0);    // VCO current 100
  if (!ok) return false;
  uint8_t mix_div = 2, div_num = 0;
  while (mix_div <= 64) {
    if (khz * mix_div >= vco_min && khz * mix_div < vco_max) {
      for (uint8_t d = mix_div; d > 2; d >>= 1) ++div_num;
      break;
    }
    mix_div <<= 1;
  }
  uint8_t data[5];
  if (!tuner_read(data, sizeof(data))) return false;
  const uint8_t fine = (data[4] & 0x30) >> 4, power_ref = 2;
  if (fine > power_ref) --div_num;
  else if (fine < power_ref) ++div_num;
  if (!tuner_write_mask(0x10, (uint8_t) (div_num << 5), 0xe0)) return false;

  const uint64_t vco = (uint64_t) hz * mix_div;
  const uint8_t nint = (uint8_t) (vco / (2 * (uint64_t) ref));
  uint32_t vco_fra = (uint32_t) ((vco - 2 * (uint64_t) ref * nint) / 1000);
  if (nint > 128 / power_ref - 1) return false;
  const uint8_t ni = (uint8_t) ((nint - 13) / 4), si = (uint8_t) (nint - 4 * ni - 13);
  const uint8_t n = (uint8_t) (ni + (si << 6));
  if (!tuner_write(0x14, &n, 1) || !tuner_write_mask(0x12, vco_fra ? 0x00 : 0x08, 0x08)) return false;
  uint16_t sdm = 0, n_sdm = 2;
  while (vco_fra > 1) {
    if (vco_fra > 2 * ref_khz / n_sdm) {
      sdm = (uint16_t) (sdm + 32768 / (n_sdm / 2));
      vco_fra -= 2 * ref_khz / n_sdm;
      if (n_sdm >= 0x8000) break;
    }
    n_sdm = (uint16_t) (n_sdm << 1);
  }
  const uint8_t hi = (uint8_t) (sdm >> 8), lo = (uint8_t) sdm;
  if (!tuner_write(0x16, &hi, 1) || !tuner_write(0x15, &lo, 1)) return false;
  for (int i = 0; i < 2; ++i) {
    vTaskDelay(pdMS_TO_TICKS(1));
    if (!tuner_read(data, 3)) return false;
    if (data[2] & 0x40) break;
    // Not locked: more VCO current, once.
    if (i == 0 && !tuner_write_mask(0x12, 0x60, 0xe0)) return false;
  }
  locked = data[2] & 0x40;
  return !locked || tuner_write_mask(0x1a, 0x08, 0x08);  // PLL autotune 8 kHz
}

// Set up as librtlsdr sets it for a 6 MHz channel at a 3.57 MHz IF: the registers, the IF filter calibrated against
// the PLL at 56 MHz, then the LNA and mixer for digital TV with the LNA's top settled.
bool Dongle::tuner_init() {
  std::memcpy(regs_ + R820T_FIRST, R820T_INIT, sizeof(R820T_INIT));
  if (!tuner_write(R820T_FIRST, R820T_INIT, sizeof(R820T_INIT))) return false;
  bool ok = tuner_write_mask(0x0c, 0x00, 0x0f) && tuner_write_mask(0x13, 49, 0x3f) &&
            tuner_write_mask(0x1d, 0x00, 0x38);
  if (!ok) return false;
  vTaskDelay(pdMS_TO_TICKS(1));
  const uint8_t filt_q = 0x10, hp_cor = 0x6b;
  uint8_t cal = 0;
  for (int i = 0; i < 2; ++i) {
    bool locked;
    ok = tuner_write_mask(0x0b, hp_cor, 0x60) && tuner_write_mask(0x0f, 0x04, 0x04) &&
         tuner_write_mask(0x10, 0x00, 0x03) &&
         tuner_set_pll(56000000, locked);
    if (!ok || !locked) return false;
    ok = tuner_write_mask(0x0b, 0x10, 0x10);  // start the calibration
    vTaskDelay(pdMS_TO_TICKS(1));
    uint8_t data[5];
    ok = ok && tuner_write_mask(0x0b, 0x00, 0x10) && tuner_write_mask(0x0f, 0x00, 0x04) &&
         tuner_read(data, sizeof(data));
    if (!ok) return false;
    cal = data[4] & 0x0f;
    if (cal && cal != 0x0f) break;
  }
  if (cal == 0x0f) cal = 0;
  ok = tuner_write_mask(0x0a, filt_q | cal, 0x1f) && tuner_write_mask(0x0b, hp_cor, 0xef) &&
       tuner_write_mask(0x07, 0x00, 0x80) && tuner_write_mask(0x06, 0x10, 0x30) && tuner_write_mask(0x1e, 0x60, 0x60) &&
       tuner_write_mask(0x05, 0x01, 0x80) && tuner_write_mask(0x1f, 0x00, 0x80) && tuner_write_mask(0x0f, 0x00, 0x80) &&
       tuner_write_mask(0x19, 0x60, 0x60);
  if (!ok) return false;
  // Digital TV (DVB-T): LNA and mixer tops and thresholds, charge pump, divider buffer and filter currents.
  const uint8_t mixer_top = 0x24, lna_top = 0xe5, lna_vth = 0x53, mixer_vth = 0x75;
  ok = tuner_write_mask(0x1d, lna_top, 0xc7) && tuner_write_mask(0x1c, mixer_top, 0xf8) &&
       tuner_write(0x0d, &lna_vth, 1) &&
       tuner_write(0x0e, &mixer_vth, 1) && tuner_write_mask(0x05, 0x00, 0x60) && tuner_write_mask(0x06, 0x00, 0x08) &&
       tuner_write_mask(0x11, 0x38, 0x38) && tuner_write_mask(0x17, 0x30, 0x30) && tuner_write_mask(0x0a, 0x40, 0x60);
  // The LNA's top: lowest while the AGC clock runs fast, then 3.
  ok = ok && tuner_write_mask(0x1d, 0x00, 0x38) && tuner_write_mask(0x1c, 0x00, 0x04) &&
       tuner_write_mask(0x06, 0x00, 0x40) &&
       tuner_write_mask(0x1a, 0x30, 0x30);
  if (!ok) return false;
  vTaskDelay(pdMS_TO_TICKS(250));
  return tuner_write_mask(0x1d, 0x18, 0x38) && tuner_write_mask(0x1c, mixer_top, 0x04) &&
         tuner_write_mask(0x1e, 14, 0x1f) && tuner_write_mask(0x1a, 0x20, 0x30);
}

// The LO at `hz` plus the IF: above 650 MHz, as 1090 is, the input's band settings are one set.
bool Dongle::tuner_set_freq(uint32_t hz) {
  const uint32_t lo = hz + if_hz_;
  if (hz < 650000000) return false;
  bool locked;
  if (!(tuner_write_mask(0x17, 0x00, 0x08) &&       // open drain
        tuner_write_mask(0x1a, 0x40, 0xc3))) {      // RF mux, poly mux
    return false;
  }
  const uint8_t tf = 0x00;
  return tuner_write(0x1b, &tf, 1) &&                // tracking filter band
         tuner_write_mask(0x10, 0x00, 0x0b) &&       // crystal cap: high, 0 pF
         tuner_write_mask(0x08, 0x00, 0x3f) && tuner_write_mask(0x09, 0x00, 0x3f) && tuner_set_pll(lo, locked) &&
             locked;
}

// A fixed gain: the VGA at 16.3 dB, then LNA and mixer steps taken in turn until the sum reaches `tenths`. Negative:
// the tuner's own LNA and mixer AGC, with the VGA at 26.5 dB.
bool Dongle::tuner_set_gain(int tenths) {
  if (tenths < 0) {
    gain_ = -1;
    return tuner_write_mask(0x05, 0x00, 0x10) && tuner_write_mask(0x07, 0x10, 0x10) &&
           tuner_write_mask(0x0c, 0x0b, 0x9f);
  }
  if (!(tuner_write_mask(0x05, 0x10, 0x10) && tuner_write_mask(0x07, 0x00, 0x10) && tuner_write_mask(0x0c, 0x08, 0x9f)))
    return false;
  int total = 0;
  uint8_t lna = 0, mix = 0;
  for (int i = 0; i < 15; ++i) {
    if (total >= tenths) break;
    total += LNA_STEPS[++lna];
    if (total >= tenths) break;
    total += MIXER_STEPS[++mix];
  }
  gain_ = total;
  return tuner_write_mask(0x05, lna, 0x0f) && tuner_write_mask(0x07, mix, 0x0f);
}

// ---------------------------------------------------------------- the stick

bool Dongle::open(usb_host_client_handle_t client, uint8_t address) {
  client_ = client;
  error_.clear();
  if (usb_host_device_open(client, address, &dev_) != ESP_OK) {
    dev_ = nullptr;
    return fail("The radio could not be opened");
  }
  if (usb_host_interface_claim(client, dev_, 0, 0) != ESP_OK) return fail("The radio's interface could not be claimed");
  claimed_ = true;
  if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CTRL_DATA, 0, &ctrl_) != ESP_OK)
    return fail("No memory for the radio's control transfers");
  if (!init_baseband()) return false;
  // The tuner, through the demodulator's I2C repeater.
  if (!repeater(true)) return fail("The radio's I2C repeater did not turn on");
  uint8_t id = 0;
  // Register 0 of the R820T, read raw as librtlsdr's probe reads it (not bit-reversed, as tuner_read does): 0x69.
  const uint8_t zero = 0;
  if (!i2c_write(R820T_ADDR, &zero, 1) || !i2c_read(R820T_ADDR, &id, 1) || id != R820T_ID) {
    repeater(false);
    char why[96];
    std::snprintf(why, sizeof(why), "No R820T tuner on the radio (read 0x%02x): only R820T and R820T2 sticks are known",
                  id);
    return fail(why);
  }
  tuner_ = "R820T";
  // Raw samples through the R820T's low IF: no zero-IF, only the in-phase ADC, its IF, and the spectrum inverted.
  bool ok = demod_write(1, 0xb1, 0x1a, 1) && demod_write(0, 0x08, 0x4d, 1) && set_if_freq(if_hz_) &&
            demod_write(1, 0x15, 0x01, 1);
  ok = ok && tuner_init();
  repeater(false);
  if (!ok) return fail("The R820T tuner refused its setup");
  ESP_LOGI(TAG, "RTL2832U with %s tuner ready", tuner_);
  return true;
}

bool Dongle::tune(uint32_t hz, uint32_t rate, int tenths, int ppm) {
  xtal_ = (uint32_t) ((int64_t) 28800000 + (int64_t) 28800000 * ppm / 1000000);
  if (!set_sample_rate(rate) || !set_ppm(ppm)) return fail("The radio refused its sample rate");
  if (!repeater(true)) return fail("The radio's I2C repeater did not turn on");
  const bool tuned = tuner_set_freq(hz);
  const bool gained = tuned && tuner_set_gain(tenths);
  repeater(false);
  if (!tuned) return fail("The tuner's PLL did not lock at 1090 MHz");
  if (!gained) return fail("The tuner refused its gain");
  if (!reset_buffer()) return fail("The radio's buffer could not be reset");
  ESP_LOGI(TAG, "Tuned to %.3f MHz, %u samples a second, gain %s%.1f dB", hz / 1e6, (unsigned) rate,
           gain_ < 0 ? "automatic " : "", gain_ < 0 ? 0.0 : gain_ / 10.0);
  return true;
}

void Dongle::close() {
  if (ctrl_) usb_host_transfer_free(ctrl_);
  ctrl_ = nullptr;
  if (dev_ && claimed_) usb_host_interface_release(client_, dev_, 0);
  claimed_ = false;
  if (dev_) usb_host_device_close(client_, dev_);
  dev_ = nullptr;
}

}  // namespace esphome::skyscope::rtl
#endif
