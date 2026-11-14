#pragma once
// The screen's sounds, on a board with a speaker (the P4 4C's, through its ES8311): a chime for each kind of alert, a
// click at a tap, and a short note to hear the volume by. Each is a few notes of a soft bell (a tone and two partials
// that fade), made here as samples, so the tests hear them on a PC; sound.cpp hands them to the speaker.
//
// Also here: when the speaker is quiet (bedtime), and how the volume setting becomes the codec's. The volume is one
// for the whole speaker: Home Assistant's media player sets the same, and its volume_min and volume_max in the board's
// YAML are CODEC_LOW and CODEC_HIGH, so a percentage is the same loudness from either side.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "psram.h"
#include "schedule.h"
#include "settings.h"

namespace sky::sound {

// An emergency sounds by its squawk: HIJACK for 7500 (unlawful interference), a klaxon; LOST_RADIO for 7600, a falling
// pair of notes; DISTRESS for 7700 and any other emergency a plane reports (with no squawk of the three), the sweep of
// an emergency locator transmitter as heard on 121.5 MHz.
enum Cue : uint8_t { WATCHED, LOST_RADIO, HIJACK, DISTRESS, OVERHEAD, CLICK, LEVEL, CUES };
inline constexpr const char *const CUE_NAMES[CUES] = {"watched", "lost_radio", "hijack", "distress",
                                                      "overhead", "click", "level"};
inline int cue_named(const char *name) {
  for (int i = 0; name && i < CUES; ++i)
    if (std::string(name) == CUE_NAMES[i]) return i;
  return -1;
}
// An emergency's cue by its squawk.
inline Cue emergency_cue(const std::string &squawk) {
  if (squawk == "7500") return HIJACK;
  if (squawk == "7600") return LOST_RADIO;
  return DISTRESS;
}
// A cue cuts short one playing that is less pressing; a click never cuts anything short.
inline int rank(Cue c) {
  switch (c) {
    case HIJACK:
    case DISTRESS: return 5;
    case LOST_RADIO: return 4;
    case WATCHED: return 3;
    case OVERHEAD: return 2;
    case LEVEL: return 1;
    default: return 0;
  }
}

// Samples a second, as the mixer and the codec run (the board's YAML says the same).
constexpr int RATE = 48000;

// The codec's volume (0 to 1, the ES8311's register in steps of half a decibel, 0.75 its own level) for a volume
// setting: none at 0, else from CODEC_LOW (-32 dB, quiet in a quiet room) to CODEC_HIGH (+6 dB) in even decibels.
constexpr float CODEC_LOW = 0.5f, CODEC_HIGH = 0.8f;
inline float codec_volume(int percent) {
  if (percent <= 0) return 0;
  return CODEC_LOW + (CODEC_HIGH - CODEC_LOW) * std::min(percent, 100) / 100.0f;
}
// The media player's volume (0 to 1) as a setting, and back.
inline int percent_of(float media_volume) {
  if (!std::isfinite(media_volume)) return 0;
  return (int) std::lround(std::max(0.0f, std::min(1.0f, media_volume)) * 100);
}

// Whether the speaker keeps quiet at bedtime: with bedtime on and quiet_bedtime, from Off at to On at, or (with no time
// to come on again) while bedtime has the screen dark. `minute` is the time of day in minutes, -1 before the clock is
// set; `bed_dark` whether bedtime turned the screen off and nothing has turned it on since.
inline bool quiet(const Settings &s, int minute, bool bed_dark) {
  if (!s.bedtime || !s.quiet_bedtime) return false;
  if (!s.bed_wake) return bed_dark;
  const int off = schedule::minutes(s.bed_off), on = schedule::minutes(s.bed_on);
  if (minute < 0 || off < 0 || on < 0) return bed_dark;
  return schedule::within(minute, off, on);
}

// Whether a cue sounds: the speaker is turned up, the cue is wanted, and it is not bedtime; a hijack or distress may be,
// but not a lost radio.
inline bool sounds(Cue c, const Settings &s, int minute, bool bed_dark) {
  if (!speaker_fitted || s.volume <= 0) return false;
  bool wanted = true;
  switch (c) {
    case WATCHED: wanted = s.sound_watched; break;
    case LOST_RADIO:
    case HIJACK:
    case DISTRESS: wanted = s.sound_emergency; break;
    case OVERHEAD: wanted = s.sound_overhead; break;
    case CLICK: wanted = s.clicks; break;
    default: break;
  }
  if (!wanted) return false;
  if (!quiet(s, minute, bed_dark)) return true;
  return (c == HIJACK || c == DISTRESS) && s.distress_always;
}

// How a note sounds: a soft bell that fades; a horn's rasp (a sawtooth's partials), held; or a clear tone, held, as a
// receiver plays a beacon's sweep.
enum Voice : uint8_t { BELL, HORN, TONE };

// One note: its pitch, when it starts and how long it rings (ms), and how loud (0 to 1). A note may glide to `to_hz`
// over `glide_ms`, and hold there, or (with `repeat`) start the glide again every `glide_ms`.
struct Note {
  float hz;
  int at_ms, ring_ms;
  float level;
  float to_hz = 0;
  int glide_ms = 0;
  bool repeat = false;
  Voice voice = BELL;
};

// Each cue's notes. A watched plane: two notes up, a doorbell's. Lost radio (7600): a falling pair, three times.
// A hijack (7500): a klaxon, its pitch climbing at the start of each blast, three blasts. Distress (7700): an ELT's
// tone sweeping down from 1600 to 400 Hz three times a second, as ICAO Annex 10 has it (down over at least 700 Hz
// between 1600 and 300 Hz, two to four times a second), for three seconds; any other emergency too. Close by: one low note, soft. A click: a
// tick too short to hear as a pitch (full level, as it is over so soon). The level: one short note.
inline std::vector<Note> notes(Cue c) {
  switch (c) {
    case WATCHED: return {{659.3f, 0, 900, 0.9f}, {987.8f, 220, 1100, 0.9f}};
    case LOST_RADIO: {
      std::vector<Note> out;
      for (int i = 0; i < 3; ++i) {
        out.push_back({1046.5f, i * 520, 320, 1.0f});
        out.push_back({784.0f, i * 520 + 220, 320, 1.0f});
      }
      return out;
    }
    case HIJACK: {
      std::vector<Note> out;
      for (int i = 0; i < 3; ++i) out.push_back({220.0f, i * 850, 650, 1.0f, 440.0f, 260, false, HORN});
      return out;
    }
    case DISTRESS: return {{1600.0f, 0, 3000, 0.9f, 400.0f, 333, true, TONE}};
    case OVERHEAD: return {{523.3f, 0, 1000, 0.7f}};
    case CLICK: return {{2400.0f, 0, 15, 1.0f}};
    case LEVEL: return {{880.0f, 0, 260, 0.8f}};
    default: return {};
  }
}

using Samples = std::vector<int16_t, InPsram<int16_t>>;

// A note's pitch `ms` into it.
inline float pitch_at(const Note &n, float ms) {
  if (n.to_hz <= 0 || n.glide_ms <= 0) return n.hz;
  const float into = n.repeat ? std::fmod(ms, (float) n.glide_ms) : std::min(ms, (float) n.glide_ms);
  return n.hz + (n.to_hz - n.hz) * into / n.glide_ms;
}

// A cue as 16-bit mono samples at `rate`. A bell rises in 4 ms (an eighth of a short one's ring), so it does not start
// with a crack of its own, then fades away over its ring; its partials (twice and three times the pitch) fade faster,
// as a bell's do. A horn or a tone holds, rising in 5 ms and falling in 15 at its end. A gliding pitch is followed
// sample by sample, so a sweep has no jumps but its own. The loudest is half of full scale, so notes that overlap do not
// clip; the codec's volume does the rest.
inline Samples render(Cue c, int rate = RATE) {
  const std::vector<Note> list = notes(c);
  int end_ms = 0;
  for (const Note &n : list) end_ms = std::max(end_ms, n.at_ms + n.ring_ms);
  Samples out((size_t) end_ms * rate / 1000, 0);
  std::vector<float> mix(out.size(), 0.0f);
  constexpr float TWO_PI = 6.28318530718f;
  // A sawtooth's first ten partials, scaled so their sum stays within 1.
  constexpr int HORN_PARTIALS = 10;
  constexpr float HORN_SCALE = 1 / 1.9f;
  for (const Note &n : list) {
    const size_t from = (size_t) n.at_ms * rate / 1000, count = (size_t) n.ring_ms * rate / 1000;
    // Down to a thousandth (-60 dB) by the end of its ring.
    const float fade = std::log(1000.0f) / std::max<size_t>(1, count);
    const float attack = n.voice == BELL ? std::max(1.0f, std::min(0.004f * rate, count / 8.0f)) : 0.005f * rate;
    const float release = 0.015f * rate;
    float phase = 0;  // in turns
    for (size_t i = 0; i < count && from + i < mix.size(); ++i) {
      const float rise = std::min(1.0f, i / attack);
      float tone;
      if (n.voice == BELL) {
        const float a = TWO_PI * phase;
        tone = std::exp(-fade * i) * (std::sin(a) + 0.35f * std::exp(-fade * 2 * i) * std::sin(2 * a) +
                                      0.15f * std::exp(-fade * 3 * i) * std::sin(3 * a)) / 1.5f;
      } else {
        const float fall = std::min(1.0f, (count - i) / release);
        const float a = TWO_PI * phase;
        float wave = 0;
        if (n.voice == HORN) {
          for (int k = 1; k <= HORN_PARTIALS; ++k) wave += std::sin(k * a) / k;
          wave *= HORN_SCALE;
        } else {
          wave = (std::sin(a) + 0.2f * std::sin(2 * a)) / 1.2f;
        }
        tone = fall * wave;
      }
      mix[from + i] += n.level * rise * tone;
      phase += pitch_at(n, i * 1000.0f / rate) / rate;
      phase -= std::floor(phase);
    }
  }
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = (int16_t) std::lround(std::max(-1.0f, std::min(1.0f, mix[i] * 0.5f)) * 32767);
  return out;
}

}  // namespace sky::sound
