#pragma once
// Numbers as text without printf's floats. Printing a float ("%f", "%g") takes some 2 KB of stack in newlib, and the
// web server's task has 4.5 KB in all: the P4 ran out of it and restarted when the page's log was downloaded. What the
// web server writes goes through these instead, and tests/test_web_stack.py holds web.cpp to it.

#include <cmath>
#include <cstdint>
#include <string>

namespace sky::numbers {

// `value` to at most `places` decimal places (0 to 6), the zeros at the end left off: "12", "0.5", "-3.25".
inline std::string text(double value, int places = 3) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return value > 0 ? "+Inf" : "-Inf";  // as Prometheus writes them
  places = places < 0 ? 0 : places > 6 ? 6 : places;
  long long scale = 1;
  for (int i = 0; i < places; ++i) scale *= 10;
  const long long scaled = std::llround(value * scale);
  const unsigned long long size = scaled < 0 ? 0ULL - (unsigned long long) scaled : (unsigned long long) scaled;
  std::string out = (scaled < 0 ? "-" : "") + std::to_string(size / scale);
  unsigned long long part = size % scale;
  if (part) {
    std::string digits = std::to_string(part);
    digits.insert(0, places - digits.size(), '0');
    while (digits.back() == '0') digits.pop_back();
    out += '.' + digits;
  }
  return out;
}

// `value` to exactly one decimal place, as "%.1f" would write it: "2.0", "12.4", "-0.5".
inline std::string tenths(double value) {
  const long long scaled = std::llround(value * 10);
  const unsigned long long size = scaled < 0 ? 0ULL - (unsigned long long) scaled : (unsigned long long) scaled;
  return (scaled < 0 ? "-" : "") + std::to_string(size / 10) + '.' + char('0' + size % 10);
}

}  // namespace sky::numbers
