#pragma once
// Mode S replies at 1090 MHz, from the radio's samples to messages whose checksum holds. The radio (rtlsdr.cpp) hands
// over bytes of I and Q, two million pairs a second; here they become power, preambles are found, bits are read and
// the CRC checked. What the messages say is adsb.h. Nothing here touches the radio, so the tests run on a PC: they
// build samples of known messages and read them back.
//
// A reply is a preamble of four half-microsecond pulses, at 0, 1, 3.5 and 4.5 us, then its bits from 8 us, one a
// microsecond: a pulse in the first half for a 1, in the second for a 0 (pulse-position modulation). At two million
// samples a second each half-microsecond is one sample, so the preamble's pulses are samples 0, 2, 7 and 9 and bit i is
// samples 16 + 2i and 17 + 2i. Messages are 56 or 112 bits; the last 24 are parity (ICAO Annex 10, vol. IV).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sky::modes {

constexpr int RATE = 2000000;     // samples a second, the radio's rate
constexpr int PREAMBLE = 16;      // samples before the first bit: 8 us
constexpr int SHORT_BITS = 56, LONG_BITS = 112;
constexpr int MOST_SAMPLES = PREAMBLE + 2 * LONG_BITS;  // the longest reply

// A sample's power from its I and Q bytes (each 0 to 255 around 127.5): an eighth of (2I - 255)^2 + (2Q - 255)^2, at
// most 16256, so it fits 16 bits with room to add. Powers are only ever compared, so no square root is taken.
struct Powers {
  uint16_t square[256];
  Powers() {
    for (int v = 0; v < 256; ++v) {
      const int d = 2 * v - 255;
      square[v] = (uint16_t) (d * d / 4);
    }
  }
  void convert(const uint8_t *iq, size_t pairs, uint16_t *out) const {
    for (size_t i = 0; i < pairs; ++i) out[i] = (uint16_t) (square[iq[2 * i]] / 2 + square[iq[2 * i + 1]] / 2);
  }
};

// The Mode S checksum: CRC-24 with generator 0x1FFF409 over all but the last 24 bits, which carry it.
class Crc {
 public:
  Crc() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i << 16;
      for (int b = 0; b < 8; ++b) c = (c & 0x800000) ? (c << 1) ^ 0xFFF409 : c << 1;
      table_[i] = c & 0xFFFFFF;
    }
    // What flipping each one bit of a long message does to its residue, for the one-bit fix.
    uint8_t m[LONG_BITS / 8];
    for (int bit = 0; bit < LONG_BITS; ++bit) {
      std::memset(m, 0, sizeof(m));
      m[bit / 8] = (uint8_t) (0x80 >> (bit % 8));
      single_[bit] = residue(m, LONG_BITS);
    }
  }
  // The checksum of the data bits, XORed with the parity the message carries: 0 for a message whose parity is the
  // checksum itself (DF11's with interrogator 0, DF17, DF18); the sender's address for one whose parity carries it
  // (DF0, 4, 5, 16, 20, 21).
  uint32_t residue(const uint8_t *m, int bits) const {
    const int bytes = bits / 8 - 3;
    uint32_t c = 0;
    for (int i = 0; i < bytes; ++i) c = ((c << 8) ^ table_[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    return c ^ ((uint32_t) m[bytes] << 16 | (uint32_t) m[bytes + 1] << 8 | m[bytes + 2]);
  }
  // The bit of a long message whose flip gives this residue, or -1: a message with one bit wrong is mended by it.
  int single_bit(uint32_t r) const {
    for (int bit = 0; bit < LONG_BITS; ++bit)
      if (single_[bit] == r) return bit;
    return -1;
  }

 private:
  uint32_t table_[256];
  uint32_t single_[LONG_BITS];
};

inline int df_of(const uint8_t *m) { return m[0] >> 3; }
inline int bits_of(int df) { return df >= 16 ? LONG_BITS : SHORT_BITS; }
inline uint32_t address_field(const uint8_t *m) { return (uint32_t) m[1] << 16 | (uint32_t) m[2] << 8 | m[3]; }

// A message the checksum let through.
struct Message {
  uint8_t data[LONG_BITS / 8] = {};
  int bits = 0;
  int df = 0;
  uint32_t address = 0;   // the sender's ICAO address
  // Its address came out of the parity (DF0, 4, 5, 16, 20, 21), which any noise can do: such a message counts only for
  // a plane heard by its own address lately (adsb.h Tracker::known).
  bool overlaid = false;
  bool mended = false;    // one bit was wrong and has been put right
  uint16_t signal = 0;    // the preamble's pulses, in the units of Powers
};

// What the demodulator found, since it started.
struct Counts {
  uint64_t samples = 0;
  uint32_t preambles = 0;   // looked like one and were read
  uint32_t accepted = 0;    // messages handed on
  uint32_t mended = 0;      // of those, with one bit put right
  uint32_t rejected = 0;    // read, and failed the checksum
};

// Finds messages in a stream of powers handed over a block at a time. A message may straddle two blocks: the end of
// each block is kept and read again in front of the next.
class Demodulator {
 public:
  // Called for each message: true if it counts (an overlaid address that is known), so the stream moves past it.
  template<typename F> void feed(const uint16_t *p, size_t n, F &&on_message) {
    window_.insert(window_.end(), p, p + n);
    counts_.samples += n;
    if (window_.size() < (size_t) MOST_SAMPLES + 1) return;
    // The noise: the mean power of the samples under this block's mean (so the replies in it count for little),
    // followed slowly. A preamble's pulses must stand well above it.
    if (n) {
      uint64_t sum = 0, under_sum = 0;
      size_t under = 0;
      for (size_t i = 0; i < n; ++i) sum += p[i];
      const uint32_t mean = (uint32_t) (sum / n);
      for (size_t i = 0; i < n; ++i)
        if (p[i] <= mean) {
          under_sum += p[i];
          ++under;
        }
      const uint32_t quiet = under ? (uint32_t) (under_sum / under) : mean;
      noise_ = noise_ ? (noise_ * 7 + quiet) / 8 : quiet;
    }
    const uint32_t floor = std::max<uint32_t>(noise_ * PULSE_OVER_NOISE, MIN_PULSE);

    const size_t end = window_.size() - MOST_SAMPLES;
    size_t j = 0;
    while (j < end) {
      const uint16_t *s = window_.data() + j;
      Message m;
      if (!preamble(s, floor, m.signal)) {
        ++j;
        continue;
      }
      ++counts_.preambles;
      if (!read(s, m) || !on_message(m)) {
        ++j;
        continue;
      }
      ++counts_.accepted;
      if (m.mended) ++counts_.mended;
      j += PREAMBLE + 2 * m.bits;
    }
    window_.erase(window_.begin(), window_.begin() + (ptrdiff_t) std::min(j, window_.size()));
  }
  const Counts &counts() const { return counts_; }
  uint32_t noise() const { return noise_; }
  const Crc &crc() const { return crc_; }

  // A pulse must be this many times the noise's power (9 dB over the quieter half of the samples, about 5 dB over the
  // noise's own mean), and never under MIN_PULSE.
  static constexpr uint32_t PULSE_OVER_NOISE = 8, MIN_PULSE = 40;

 private:
  // The preamble at s[0]: its four pulses all above `floor`, each above the samples around it, and the quiet samples
  // (between the pulse pairs and before the first bit) well below the weakest pulse. Sample 1, 3, 6, 8 and 10 may hold
  // some of a pulse when the reply does not start on a sample, so they are only compared, never required to be quiet.
  static bool preamble(const uint16_t *s, uint32_t floor, uint16_t &signal) {
    if (!(s[0] > s[1] && s[2] > s[1] && s[2] > s[3] && s[7] > s[6] && s[9] > s[10])) return false;
    const uint32_t high = std::min(std::min(s[0], s[2]), std::min(s[7], s[9]));
    if (high < floor) return false;
    const uint32_t quiet = std::max({s[4], s[5], s[11], s[12], s[13], s[14]});
    if (high < 2 * quiet) return false;
    signal = (uint16_t) ((s[0] + s[2] + s[7] + s[9]) / 4);
    return true;
  }
  // The bits after the preamble at s[0], and whether the checksum lets them through.
  bool read(const uint16_t *s, Message &m) {
    const uint16_t *b = s + PREAMBLE;
    // The first byte says how long it is.
    uint8_t first = 0;
    for (int i = 0; i < 8; ++i) first = (uint8_t) (first << 1 | (b[2 * i] > b[2 * i + 1]));
    m.df = first >> 3;
    if (!wanted(m.df)) return false;
    m.bits = bits_of(m.df);
    for (int i = 0; i < m.bits; ++i)
      if (b[2 * i] > b[2 * i + 1]) m.data[i / 8] |= (uint8_t) (0x80 >> (i % 8));
    const uint32_t r = crc_.residue(m.data, m.bits);
    switch (m.df) {
      case 17:
      case 18: {
        if (r != 0) {
          // One bit wrong may be put right, but never in the first five: that would make it another kind.
          const int bit = crc_.single_bit(r);
          if (bit < 5) return reject();
          m.data[bit / 8] ^= (uint8_t) (0x80 >> (bit % 8));
          m.mended = true;
        }
        m.address = address_field(m.data);
        return true;
      }
      case 11:
        // The parity carries the interrogator's code (7 bits) over the checksum; 0 for a squitter.
        if (r & ~0x7Fu) return reject();
        m.address = address_field(m.data);
        return true;
      default:
        // The parity carries the sender's address: whether that is a plane heard lately is for the caller.
        m.address = r;
        m.overlaid = true;
        return true;
    }
  }
  bool reject() {
    ++counts_.rejected;
    return false;
  }
  // The kinds read: altitude and identity replies (0, 4, 5, 16, 20, 21), all-call replies (11) and extended squitters
  // (17, 18). The rest (19, 22, 24 and the unassigned) are let go before their checksum is worked out.
  static bool wanted(int df) {
    return df == 0 || df == 4 || df == 5 || df == 11 || df == 16 || df == 17 || df == 18 || df == 20 || df == 21;
  }

  Crc crc_;
  std::vector<uint16_t> window_;
  Counts counts_;
  uint32_t noise_ = 0;
};

// The samples of a message as a radio would hand them over: for the tests, and for checking the demodulator on a PC.
// `low` and `high` are the I/Q bytes' distance from the middle in quiet and in a pulse.
inline std::vector<uint8_t> samples_of(const uint8_t *m, int bits, int low = 2, int high = 60, int lead = 40) {
  std::vector<int> on(lead + MOST_SAMPLES + 40, 0);
  for (int at : {0, 2, 7, 9}) on[lead + at] = 1;
  for (int i = 0; i < bits; ++i) {
    const bool one = m[i / 8] & (0x80 >> (i % 8));
    on[lead + PREAMBLE + 2 * i + (one ? 0 : 1)] = 1;
  }
  std::vector<uint8_t> iq(on.size() * 2);
  for (size_t i = 0; i < on.size(); ++i) {
    // The pulses' phase turns from sample to sample, as a real carrier off by a few kHz does.
    const int a = on[i] ? high : low;
    const int k = (int) (i % 4);
    const int ii = k == 0 ? a : k == 2 ? -a : 0, qq = k == 1 ? a : k == 3 ? -a : 0;
    iq[2 * i] = (uint8_t) (128 + ii);
    iq[2 * i + 1] = (uint8_t) (128 + qq);
  }
  return iq;
}

}  // namespace sky::modes
