#pragma once
// The firmware update as the web page sees it: from what the update entity says (its state, and whether its last
// check failed), whether an install is under way, whether one has failed, and the words the page shows. web.cpp feeds
// it on the loop; nothing here knows ESPHome, so the tests run on a PC.
#include <string>

namespace sky::firmware {

// As ESPHome's update::UpdateState.
enum State { UNKNOWN, NO_UPDATE, AVAILABLE, INSTALLING };

constexpr const char *INSTALL_FAILED = "The install failed; the screen keeps the firmware it has.";
constexpr const char *SERVER_FAILED = "The update server could not be read.";

// The firmware line of the screen's own menu, and how it is coloured.
enum Tone { QUIET, GOOD_NEWS, BAD_NEWS };
struct Line {
  std::string text;
  Tone tone;
};
// What the page's /api/firmware says, for the menu: checking (a check asked for in the menu and not answered yet),
// installing, an error, a release to install, or the latest.
inline Line menu_line(bool checking, bool known, bool available, bool installing, const std::string &latest,
                      const std::string &error) {
  if (installing) return {"Installing " + latest + "...", GOOD_NEWS};
  if (checking) return {"Checking...", QUIET};
  if (!error.empty()) return {error, BAD_NEWS};
  if (available) return {latest + " is available", GOOD_NEWS};
  if (known) return {"This is the latest release", QUIET};
  return {"Not checked yet", QUIET};
}

struct Tracker {
  bool updating = false;  // asked to install, until the screen restarts or the install fails
  bool flashing = false;  // the entity has said it is installing
  std::string error;      // for the page, "" for none
  State failed_in = UNKNOWN;  // what the entity said as the last install failed

  // An install asked for: the page's word goes, and the screen says it is updating.
  bool start(State state) {
    if (updating || state != AVAILABLE) return false;
    updating = true;
    flashing = false;
    error.clear();
    return true;
  }

  // What the entity says now. An install that began and is no longer installing failed (a good one restarts the
  // screen before saying anything else); a failed check is said until a check works. A failed install's word stays
  // until the next install starts, or until a check finds nothing to install. True when an install has just failed.
  bool seen(State state, bool check_failed) {
    const bool installing = state == INSTALLING;
    if (installing) flashing = true;
    if (updating && flashing && !installing) {
      updating = false;
      flashing = false;
      error = INSTALL_FAILED;
      failed_in = state;
      return true;
    }
    // The entity's own error after it is the install's, not the server's; once nothing is offered it no longer matters.
    if (error == INSTALL_FAILED && !(state == NO_UPDATE && failed_in != NO_UPDATE)) return false;
    error = check_failed ? SERVER_FAILED : "";
    return false;
  }
};
}  // namespace sky::firmware
