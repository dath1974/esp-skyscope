#pragma once
// The screen off (Settings sleep_min, bedtime, Home Assistant): how often it still asks for planes, which alerts turn
// it on again, and when one that did goes dark again. Nobody is looking, so the exchanges are asked less often; the
// screen's own radio and a receiver on the network cost nothing to ask, and keep their pace. The rest (the glass, the
// worker) is in skyscope.cpp and worker.cpp; this is what the tests can hold on a PC.
#include <algorithm>
#include <cstdint>
#include <string>

#include "settings.h"
#include "sightings.h"
#include "sound.h"

namespace sky::asleep {

// Seconds between reports from `source`: with the screen off, an exchange's at sleep_refresh_s, and never more often
// than with it on.
inline int refresh_s(const Settings &s, Feed source, bool asleep) {
  if (!asleep || (source != ADSB_FI && source != ADSB_LOL)) return s.refresh_s;
  return std::max(s.refresh_s, s.sleep_refresh_s);
}

// Whether the planes are asked for: always with the screen on, and with it off where the person asked for it.
inline bool polls(const Settings &s, bool asleep) { return !asleep || s.sleep_poll; }

// Whether an alert turns the screen on: a watched plane or an emergency (never one close by), with alert_wake on, and
// not in bedtime's quiet (sound.h quiet), but for a hijack or distress where those sound through it.
inline bool wakes(sightings::Kind kind, const std::string &squawk, const Settings &s, int minute, bool bed_dark) {
  if (!s.sleep_poll || !s.alert_wake || kind == sightings::OVERHEAD) return false;
  if (!sound::quiet(s, minute, bed_dark)) return true;
  if (kind != sightings::EMERGENCY || !s.distress_always) return false;
  const sound::Cue cue = sound::emergency_cue(squawk);
  return cue == sound::HIJACK || cue == sound::DISTRESS;
}

// How long a report's watched planes and emergencies count as still there: three reports' worth and a minute more, so a
// report late or refused does not let the screen go dark under one.
inline uint32_t present_ms(const Settings &s, uint32_t every_ms) {
  return 3 * std::max(every_ms, (uint32_t) s.refresh_s * 1000) + 60000;
}

// A screen an alert turned on stays on this long at least, as the alert along its top does, and longer while the plane
// keeps it on (alert_awake); then it goes dark again, unless a finger has touched it since.
constexpr uint32_t WOKEN_MS = 20000;

}  // namespace sky::asleep
