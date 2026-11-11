#pragma once
// Times to wait for on the screen's millisecond clock, which is 32 bits and starts again every 49.7 days. "Is it past
// `at`?" is asked as (int32_t) (now - at) >= 0, which is right while `at` is within 24.8 days of now: a time longer
// gone than that reads as yet to come, for the next 24.8 days. A screen on a wall is up that long, and a time it set
// once and has not needed since (the last touch, a layer switched off, a month asleep) is that old: taps were taken
// for too soon after a pinch, and a tapped plane's route was never asked for.
//
// So a time that may wait unasked is looked at on every pass and kept just behind the clock (keep_past), or let go
// once it has come (expire, for one where 0 says there is none).
#include <cstdint>

namespace sky::ticks {

// Whether `at` has come, for a time within 24.8 days of `now`.
inline bool past(uint32_t at, uint32_t now) { return (int32_t) (now - at) >= 0; }

// `at`, once it is over a minute gone, a minute behind `now`: still past, however long until it is next asked.
constexpr uint32_t BEHIND_MS = 60000;
inline void keep_past(uint32_t &at, uint32_t now) {
  if ((int32_t) (now - at) > (int32_t) BEHIND_MS) at = now - BEHIND_MS;
}

// A time that is 0 when there is none: none again once it has come.
inline void expire(uint32_t &at, uint32_t now) {
  if (at && past(at, now)) at = 0;
}

}  // namespace sky::ticks
