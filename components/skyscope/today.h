#pragma once
// The day's tally in words, for its page on the glass (todaypage.cpp): what the screen has seen in its range since
// midnight (sightings.h), as the web page's Today in range tells it. Plain C++, so the tests read it on a PC.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sightings.h"

namespace sky::today {

// A line of the page: a heading over a part, an ordinary line, or a quieter one.
enum Tone : uint8_t { HEADING, PLAIN, QUIET };
struct Line {
  Tone tone;
  std::string text;
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

// The page's lines under the chart of the hours: the day's records, its commonest types and operators, and the days
// before. `metric` gives kilometers and meters for nautical miles, knots and feet.
inline std::vector<Line> lines(const sightings::Book &book, bool metric) {
  std::vector<Line> out;
  const sightings::Day &d = book.today();
  if (!d.date) return out;
  char text[96];
  {
    std::string line = "Most at once " + std::to_string(d.busiest) + ", " + count(d.visits, "visit", "visits");
    const int hour = busiest_hour(d);
    if (hour >= 0) {
      std::snprintf(text, sizeof(text), ", busiest hour %d:00 (%u)", hour, (unsigned) d.hours[hour]);
      line += text;
    }
    out.push_back({PLAIN, line});
    if (d.watched || d.emergencies)
      out.push_back({PLAIN, count(d.watched, "watched plane", "watched planes") + ", " +
                                count(d.emergencies, "emergency", "emergencies")});
  }
  if (d.closest.set || d.highest.set || d.fastest.set) out.push_back({HEADING, "The day's records"});
  if (d.closest.set) {
    const double value = metric ? d.closest.value / 1000.0 : d.closest.value / 1852.0;
    std::snprintf(text, sizeof(text), "Nearest  %s, %.1f %s", d.closest.name, value, metric ? "km" : "NM");
    out.push_back({PLAIN, text});
  }
  if (d.highest.set) {
    std::snprintf(text, sizeof(text), "Highest  %s, %.0f %s", d.highest.name,
                  metric ? d.highest.value * 0.3048 : (double) d.highest.value, metric ? "m" : "ft");
    out.push_back({PLAIN, text});
  }
  if (d.fastest.set) {
    std::snprintf(text, sizeof(text), "Fastest  %s, %.0f %s", d.fastest.name,
                  metric ? d.fastest.value * 1.852 : (double) d.fastest.value, metric ? "km/h" : "kt");
    out.push_back({PLAIN, text});
  }
  auto top = [&](const char *heading, const std::vector<sightings::Count> &list) {
    if (list.empty()) return;
    out.push_back({HEADING, heading});
    for (const sightings::Count &c : list) out.push_back({PLAIN, std::to_string(c.n) + "  " + c.name});
  };
  top("Commonest types", book.top_types());
  top("Commonest operators", book.top_operators());
  // The days before, the latest first; today's is the last of the list.
  const auto &days = book.days();
  if (days.size() > 1) {
    out.push_back({HEADING, "The days before"});
    for (size_t i = days.size() - 1; i-- > 0;)
      out.push_back({QUIET, day_name(days[i].date) + "  " + count(days[i].planes, "plane", "planes")});
  }
  return out;
}

}  // namespace sky::today
