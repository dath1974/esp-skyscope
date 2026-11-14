#pragma once
// The day's tally in words, for its page on the glass (todaypage.cpp): what the screen has seen in its range since
// midnight (sightings.h), as the web page's Today in range tells it. Plain C++, so the tests read it on a PC.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sightings.h"

namespace sky::today {

// A line of the page: a heading over a part, an ordinary line, or a quieter one. `text` is all of it in words. A line
// that is a row of a list has its parts too, for the page to set in columns: what it is (`label`, as in "Nearest"),
// whose it is (`name`) and how much (`value`), and `share`, its count against the longest of its list (0 to 1, for a
// bar behind the row; -1 for none).
enum Tone : uint8_t { HEADING, PLAIN, QUIET };
struct Line {
  Tone tone;
  std::string text;
  std::string label, name, value;
  float share = -1;
};

// A number of the day and what it counts, for the tiles at the top of the page.
struct Stat {
  std::string value, caption;
};

inline std::string count(uint32_t n, const char *one, const char *many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

// The day in a line, for under the title: "42 planes today, 5 in range now".
inline std::string summary(const sightings::Book &book) {
  const sightings::Day &d = book.today();
  if (!d.date) return "Nothing seen yet: the day's count starts once the clock is set";
  return count(d.planes, "plane", "planes") + " today, " + std::to_string(book.in_range()) + " in range now";
}

// A date as the page writes it: 20261110 as "Nov 10".
inline std::string day_name(uint32_t date) {
  static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const uint32_t month = date / 100 % 100;
  if (month < 1 || month > 12) return "";
  return std::string(MONTHS[month - 1]) + " " + std::to_string(date % 100);
}

// The hour of the day with the most planes first seen in it, -1 while there are none; and that most.
inline int busiest_hour(const sightings::Day &d) {
  int best = -1;
  for (int h = 0; h < 24; ++h)
    if (d.hours[h] && (best < 0 || d.hours[h] > d.hours[best])) best = h;
  return best;
}

// The day's three numbers: the planes seen, those in range now, and the most at once. None before the clock is set.
inline std::vector<Stat> stats(const sightings::Book &book) {
  const sightings::Day &d = book.today();
  if (!d.date) return {};
  return {{std::to_string(d.planes), d.planes == 1 ? "plane today" : "planes today"},
          {std::to_string(book.in_range()), "in range now"},
          {std::to_string(d.busiest), "most at once"}};
}

// The page's lines under the chart of the hours: today's records, its most common types and operators, and the
// previous daily totals. `metric` gives kilometers and meters for nautical miles, knots and feet.
inline std::vector<Line> lines(const sightings::Book &book, bool metric) {
  std::vector<Line> out;
  const sightings::Day &d = book.today();
  if (!d.date) return out;
  char text[96], value[32];
  {
    std::string line = count(d.visits, "visit", "visits");
    const int hour = busiest_hour(d);
    if (hour >= 0) {
      std::snprintf(text, sizeof(text), ", busiest hour %d:00 (%u)", hour, (unsigned) d.hours[hour]);
      line += text;
    }
    out.push_back({QUIET, line});
    if (d.watched || d.emergencies)
      out.push_back({PLAIN, count(d.watched, "watched plane", "watched planes") + ", " +
                                count(d.emergencies, "emergency", "emergencies")});
  }
  auto record = [&](const char *label, const sightings::Record &r, const char *format, double amount,
                    const char *unit) {
    if (!r.set) return;
    std::snprintf(value, sizeof(value), format, amount, unit);
    out.push_back({PLAIN, std::string(label) + "  " + r.name + ", " + value, label, r.name, value});
  };
  if (d.closest.set || d.highest.set || d.fastest.set) out.push_back({HEADING, "Records today"});
  record("Nearest", d.closest, "%.1f %s", metric ? d.closest.value / 1000.0 : d.closest.value / 1852.0,
         metric ? "km" : "NM");
  record("Highest", d.highest, "%.0f %s", metric ? d.highest.value * 0.3048 : (double) d.highest.value,
         metric ? "m" : "ft");
  record("Fastest", d.fastest, "%.0f %s", metric ? d.fastest.value * 1.852 : (double) d.fastest.value,
         metric ? "km/h" : "kt");
  auto top = [&](const char *heading, const std::vector<sightings::Count> &list) {
    if (list.empty()) return;
    out.push_back({HEADING, heading});
    uint32_t most = 1;
    for (const sightings::Count &c : list) most = std::max<uint32_t>(most, c.n);
    for (const sightings::Count &c : list)
      out.push_back({PLAIN, std::to_string(c.n) + "  " + c.name, "", c.name, std::to_string(c.n), (float) c.n / most});
  };
  top("Most common types", book.top_types());
  top("Most common operators", book.top_operators());
  // The previous daily totals, the latest first; today's is the last of the list.
  const auto &days = book.days();
  if (days.size() > 1) {
    out.push_back({HEADING, "Previous daily totals"});
    uint32_t most = 1;
    for (size_t i = 0; i + 1 < days.size(); ++i) most = std::max<uint32_t>(most, days[i].planes);
    for (size_t i = days.size() - 1; i-- > 0;)
      out.push_back({QUIET, day_name(days[i].date) + "  " + count(days[i].planes, "plane", "planes"), "",
                     day_name(days[i].date), std::to_string(days[i].planes), (float) days[i].planes / most});
  }
  return out;
}

}  // namespace sky::today
