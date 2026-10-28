#pragma once
// Bedtime: the time zone a person sets (a POSIX TZ string, as ESPHome and every libc take them, "PST8PDT,M3.2.0,
// M11.1.0") read here into its parts, and the minutes of the day the screen goes dark and comes on again. ESPHome reads
// a zone only when the firmware is built; this reads one at any time, into the same parts its clock takes (posix_tz.h
// ParsedTimezone). Nothing here keeps the time, so the tests run on a PC.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace sky::schedule {

enum RuleType : uint8_t { NONE, MONTH_WEEK_DAY, JULIAN_NO_LEAP, DAY_OF_YEAR };
struct Rule {
  int32_t time_seconds = 7200;  // after midnight, 2:00 unless the string says
  uint16_t day = 0;              // JULIAN_NO_LEAP (1-365) and DAY_OF_YEAR (0-365)
  RuleType type = NONE;
  uint8_t month = 0, week = 0, day_of_week = 0;  // MONTH_WEEK_DAY
};
struct Zone {
  int32_t std_offset_seconds = 0;  // west of UTC is positive, as POSIX writes it (PST8 is 8 h)
  int32_t dst_offset_seconds = 0;
  Rule dst_start, dst_end;
};

namespace detail {
// A name: three or more letters, or anything but '>' between angle brackets ("<+0530>").
inline bool name(const char *&p) {
  if (*p == '<') {
    const char *q = p + 1;
    while (*q && *q != '>') ++q;
    if (*q != '>' || q - p - 1 < 1) return false;
    p = q + 1;
    return true;
  }
  const char *q = p;
  while (std::isalpha((unsigned char) *q)) ++q;
  if (q - p < 3) return false;
  p = q;
  return true;
}
// [+-]hh[:mm[:ss]], in seconds; hours up to `most`.
inline bool clock(const char *&p, int32_t &out, int most) {
  int sign = 1;
  if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
  if (!std::isdigit((unsigned char) *p)) return false;
  int parts[3] = {0, 0, 0};
  for (int i = 0; i < 3; ++i) {
    if (i && *p != ':') break;
    if (i) ++p;
    if (!std::isdigit((unsigned char) *p)) return false;
    int v = 0, digits = 0;
    while (std::isdigit((unsigned char) *p) && digits < 3) v = v * 10 + (*p++ - '0'), ++digits;
    parts[i] = v;
  }
  if (parts[0] > most || parts[1] > 59 || parts[2] > 59) return false;
  out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
  return true;
}
inline bool number(const char *&p, int &out, int low, int high) {
  if (!std::isdigit((unsigned char) *p)) return false;
  int v = 0;
  while (std::isdigit((unsigned char) *p)) {
    v = v * 10 + (*p++ - '0');
    if (v > high) return false;
  }
  if (v < low) return false;
  out = v;
  return true;
}
// Mm.w.d, Jn or n, then [/time] (which may run past a day either way, as POSIX allows).
inline bool rule(const char *&p, Rule &r) {
  int a = 0, b = 0, c = 0;
  if (*p == 'M') {
    ++p;
    if (!number(p, a, 1, 12) || *p++ != '.' || !number(p, b, 1, 5) || *p++ != '.' || !number(p, c, 0, 6)) return false;
    r.type = MONTH_WEEK_DAY;
    r.month = (uint8_t) a;
    r.week = (uint8_t) b;
    r.day_of_week = (uint8_t) c;
  } else if (*p == 'J') {
    ++p;
    if (!number(p, a, 1, 365)) return false;
    r.type = JULIAN_NO_LEAP;
    r.day = (uint16_t) a;
  } else {
    if (!number(p, a, 0, 365)) return false;
    r.type = DAY_OF_YEAR;
    r.day = (uint16_t) a;
  }
  r.time_seconds = 7200;
  if (*p == '/') {
    ++p;
    if (!clock(p, r.time_seconds, 167)) return false;
  }
  return true;
}
}  // namespace detail

// Reads `tz` into `out`; false for a string it cannot read, `out` then as it was.
inline bool parse(const std::string &tz, Zone &out) {
  Zone z;
  const char *p = tz.c_str();
  if (!detail::name(p) || !detail::clock(p, z.std_offset_seconds, 24)) return false;
  if (!*p) {
    out = z;
    return true;
  }
  if (!detail::name(p)) return false;
  // A summer time an hour ahead unless it says.
  z.dst_offset_seconds = z.std_offset_seconds - 3600;
  if (*p && *p != ',' && !detail::clock(p, z.dst_offset_seconds, 24)) return false;
  if (!*p) {
    // A summer time named without rules: the United States' (as libc takes it).
    z.dst_start = {7200, 0, MONTH_WEEK_DAY, 3, 2, 0};
    z.dst_end = {7200, 0, MONTH_WEEK_DAY, 11, 1, 0};
    out = z;
    return true;
  }
  if (*p++ != ',' || !detail::rule(p, z.dst_start) || *p++ != ',' || !detail::rule(p, z.dst_end) || *p) return false;
  out = z;
  return true;
}

// "HH:MM" into minutes of the day; -1 for anything else.
inline int minutes(const std::string &hhmm) {
  int h = 0, m = 0;
  char tail = 0;
  if (hhmm.size() < 4 || hhmm.size() > 5 || std::sscanf(hhmm.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

// Whether `now` (minutes of the day) lies between going dark at `off` and coming on at `on`, round midnight or not.
inline bool within(int now, int off, int on) {
  if (off == on) return false;
  return off < on ? now >= off && now < on : now >= off || now < on;
}
}  // namespace sky::schedule
