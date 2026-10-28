#pragma once
// Which ADS-B exchange to ask next, and when. Each exchange sets its own limits and changes them with its load, so the
// screen keeps a record of each: how long it is to be left alone, how many times in a row it failed, and the pace it
// announced. The order is the preferred exchange first, then the others as settings.h FEEDS lists them; every request
// goes to the first one free, so the screen comes back to the preferred one by itself once its time is up.
//
// How long an exchange is left alone after it failed, in this order:
//  1. what it said: the end of its quota (x-ratelimit-reset), or Retry-After;
//  2. else twice as long each time it failed in a row: from 2 minutes up to 30 for a refusal (429, 403), from 15
//     seconds up to 15 minutes for anything else (no answer, a server error, a reply it could not read).
// A good answer clears the record. A pace an exchange announces (x-ratelimit-limit) holds until the end of the quota
// it gave, else for PACE_HOLD_MS, and then the refresh asked for counts again.
//
// Nothing here fetches or keeps the time itself (millis() is handed in), so the tests run on a PC.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include "feed.h"
#include "settings.h"

namespace sky::providers {

constexpr size_t COUNT = FEEDS.size;
constexpr uint32_t REFUSED_FIRST_MS = 2 * 60000, REFUSED_MOST_MS = 30 * 60000;
constexpr uint32_t FAILED_FIRST_MS = 15000, FAILED_MOST_MS = 15 * 60000;
constexpr uint32_t MOST_WAIT_MS = 24u * 3600 * 1000;
constexpr uint32_t PACE_HOLD_MS = 10 * 60000;

// What an exchange answered: its HTTP status (0 for none) and what it said about its limits.
struct Answer {
  int status = 0;
  int retry_after_s = 0;
  int rate_reset_s = 0;
  std::string rate_limit;
  std::string error;
};

struct State {
  bool blocked = false;
  uint32_t until = 0;      // while blocked: millis() when it may be asked again
  int failures = 0;        // in a row
  std::string error;       // the last failure's, while blocked
  uint32_t paced_ms = 0;   // the fewest ms between two requests it allows, while paced
  uint32_t paced_until = 0;
};

inline bool before(uint32_t a, uint32_t b) { return (int32_t) (a - b) < 0; }

class Providers {
 public:
  void set_order(Feed preferred, bool fallback) {
    preferred_ = preferred;
    fallback_ = fallback;
  }
  Feed preferred() const { return preferred_; }
  bool fallback() const { return fallback_; }
  // The exchanges in the order they are tried.
  std::array<Feed, COUNT> order() const {
    std::array<Feed, COUNT> out{};
    size_t n = 0;
    out[n++] = preferred_;
    for (size_t i = 0; i < COUNT; ++i)
      if ((Feed) i != preferred_) out[n++] = (Feed) i;
    return out;
  }
  // The exchange to ask at `now`, or false with `wait_ms` until the first one is free.
  bool choose(uint32_t now, Feed &feed, uint32_t &wait_ms) {
    uint32_t soonest = MOST_WAIT_MS;
    for (const Feed f : order()) {
      State &s = states_[f];
      if (s.blocked && !before(now, s.until)) s.blocked = false;  // its time is up: asked again, and on failing it waits
      if (!s.blocked) {
        feed = f;
        return true;
      }
      soonest = std::min(soonest, s.until - now);
      if (!fallback_) break;
    }
    wait_ms = soonest;
    return false;
  }
  // A good answer from `f` at `now`: when to ask next, `refresh_ms` or less often while it announces a pace.
  uint32_t succeeded(Feed f, uint32_t now, uint32_t refresh_ms, const Answer &a) {
    State &s = states_[f];
    s.blocked = false;
    s.failures = 0;
    s.error.clear();
    note_pace(s, now, a);
    if (s.paced_ms && before(now, s.paced_until)) return std::max(refresh_ms, s.paced_ms);
    s.paced_ms = 0;
    return refresh_ms;
  }
  // `f` failed at `now`: how long it is left alone.
  uint32_t failed(Feed f, uint32_t now, const Answer &a) {
    State &s = states_[f];
    const bool refused = a.status == 429 || a.status == 403;
    uint32_t wait;
    if (refused && a.rate_reset_s > 0) {
      // The end of its quota, and a few seconds more so the first request does not come a moment too early.
      wait = (uint32_t) std::min<int64_t>((int64_t) a.rate_reset_s * 1000 + 5000, MOST_WAIT_MS);
    } else if (a.retry_after_s > 0) {
      wait = (uint32_t) std::min<int64_t>((int64_t) a.retry_after_s * 1000, MOST_WAIT_MS);
    } else {
      const uint32_t first = refused ? REFUSED_FIRST_MS : FAILED_FIRST_MS, most = refused ? REFUSED_MOST_MS : FAILED_MOST_MS;
      wait = std::min<uint64_t>((uint64_t) first << std::min(s.failures, 16), most);
    }
    ++s.failures;
    s.blocked = true;
    s.until = now + wait;
    s.error = refused ? (a.rate_reset_s > 0 ? "limit reached" : "too many requests") : a.error;
    note_pace(s, now, a);
    return wait;
  }
  const State &state(Feed f) const { return states_[f]; }
  // Whether every exchange that may be asked is waiting.
  bool all_blocked(uint32_t now) const {
    for (const Feed f : order()) {
      const State &s = states_[f];
      if (!s.blocked || !before(now, s.until)) return false;
      if (!fallback_) break;
    }
    return true;
  }

 private:
  static void note_pace(State &s, uint32_t now, const Answer &a) {
    const uint32_t pace = feed::min_interval_ms(a.rate_limit);
    if (!pace) return;
    s.paced_ms = pace;
    s.paced_until = now + (a.rate_reset_s > 0 ? (uint32_t) std::min<int64_t>((int64_t) a.rate_reset_s * 1000, MOST_WAIT_MS)
                                              : PACE_HOLD_MS);
  }
  Feed preferred_ = ADSB_FI;
  bool fallback_ = true;
  std::array<State, COUNT> states_{};
};
}  // namespace sky::providers
