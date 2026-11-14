#pragma once
// The tour: at a new screen's first map, what a finger does here, one thing at a time (tour.cpp draws it). Each step
// asks for one touch and waits for it to be made: it knows the touch by what it leaves on the glass (a plane's card,
// the brightness level, the weather page), says what was just seen, and goes on once that is put away again. Nothing
// is held up meanwhile: the map, the planes and every touch work as they always do, and the card's keys go on to the
// next step or end the tour. Plain C++, so the tests walk it on a PC.
#include <cstdint>

#include "settings.h"

namespace sky::tour {

// The steps, in the order they are shown: a welcome, a touch each, and a goodbye.
enum Step : uint8_t { WELCOME, PLANE, ADDRESS, BRIGHTNESS, ZOOM, MAPS, WEATHER, TALLY, MENU, FINISH, STEPS };
inline const char *const NAMES[STEPS] = {"welcome", "plane",   "address", "brightness", "zoom",
                                         "maps",    "weather", "tally",   "menu",       "finish"};

// What the glass shows now, as far as the tour looks.
struct Seen {
  bool planes = false;      // there is a plane to tap
  bool can_swipe = true;    // there is another map to swipe to (off a network without a map card there is not)
  bool card = false;        // a plane's card
  bool address = false;     // the screen's address in the status line
  bool level = false;       // the brightness level
  bool zooming = false;     // a view two fingers chose, with its Save and Cancel
  bool swiping = false;     // the name of a map swiped to
  bool weather = false;     // the weather page
  bool tally = false;       // the day's tally
  bool menu = false;        // the menu
};

// What a step says: its heading, what to do, and what to say once it was done ("" to leave the words as they are).
struct Words {
  const char *title, *ask, *done;
};
inline Words words(Step step, const Seen &seen) {
  switch (step) {
    case WELCOME:
      return {"Welcome to Skyscope", "A short tour of what a finger does here. Try each touch as it comes.", ""};
    case PLANE:
      return {"A plane's card",
              seen.planes ? "Tap a plane." : "No plane is on the glass yet. When one is, a tap on it opens its card.",
              "Its type, height, speed and route. Tap the card to put it away."};
    case ADDRESS:
      return {"The screen's address", "Tap the map, away from the planes.",
              "At the top: the address of its web page, where every setting is too."};
    case BRIGHTNESS:
      // On a screen with a speaker, the left edge is the volume's (sound.cpp).
      return {"Brightness", "Drag a finger up or down along the right edge.",
              speaker_fitted ? "Up is brighter. The left edge sets the volume the same way." : "Up is brighter."};
    case ZOOM:
      return {"Zoom and pan", "Spread or pinch two fingers, or drag one across the map.",
              "Save keeps this view. Cancel goes back."};
    case MAPS:
      return {"Another map", "Swipe left or right along the bottom edge.", "A swipe the other way goes back."};
    case WEATHER:
      return {"The weather", "Swipe up from the bottom edge.", ""};
    case TALLY:
      return {"The day's planes", "Swipe down from the top edge.", ""};
    case MENU:
      return {"The menu", "Hold a finger still on the map for every setting.", ""};
    default:
      return {"That is all", "The tour is in the menu, under About, to take again.", ""};
  }
}

// Whether a step can be taken at all: there is no map to swipe to on a screen that has only its radar.
inline bool offered(Step step, const Seen &seen) { return step != MAPS || seen.can_swipe; }

// Whether the touch a step asks for has been made: what it leaves is on the glass.
inline bool made(Step step, const Seen &seen) {
  switch (step) {
    case PLANE: return seen.card;
    case ADDRESS: return seen.address;
    case BRIGHTNESS: return seen.level;
    case ZOOM: return seen.zooming;
    case MAPS: return seen.swiping;
    case WEATHER: return seen.weather;
    case TALLY: return seen.tally;
    case MENU: return seen.menu;
    default: return false;  // the welcome and the goodbye wait for their key
  }
}

// Whether what a step's touch opened is still open: the next step waits until it is put away. The address goes by
// itself and is in nobody's way.
inline bool open(Step step, const Seen &seen) { return step != ADDRESS && made(step, seen); }

// The step after `step` that can be taken; FINISH has none (STEPS).
inline Step after(Step step, const Seen &seen) {
  Step next = step;
  do next = (Step) (next + 1);
  while (next < STEPS && !offered(next, seen));
  return next;
}

// The steps that ask for a touch, counted for the card's row of dots: how many there are for this screen, and which of
// them `step` is (from 0; the welcome is before the first and the goodbye after the last).
inline int touches(const Seen &seen) {
  int n = 0;
  for (int s = PLANE; s < FINISH; ++s) n += offered((Step) s, seen);
  return n;
}
inline int touch_index(Step step, const Seen &seen) {
  int n = 0;
  for (int s = PLANE; s < step && s < FINISH; ++s) n += offered((Step) s, seen);
  return step == WELCOME ? -1 : n;
}

// Where the tour is. look() is called as often as the glass may have changed; it and next() say true when the card's
// words must be set again.
class Guide {
 public:
  // After a step's touch was made and put away, its words stay this long before the next step's.
  static constexpr uint32_t HOLD_MS = 2500;

  void start() {
    step_ = WELCOME;
    done_ = idle_ = false;
  }
  Step step() const { return step_; }
  bool done() const { return done_; }  // this step's touch was made
  bool over() const { return step_ >= STEPS; }

  bool look(const Seen &seen, uint32_t now) {
    if (over()) return false;
    bool changed = false;
    if (!done_ && made(step_, seen)) {
      done_ = true;
      idle_ = false;
      changed = true;
    }
    if (!done_) return changed;
    if (open(step_, seen)) {
      idle_ = false;
    } else if (!idle_) {
      idle_ = true;
      idle_since_ = now;
    } else if (now - idle_since_ >= HOLD_MS) {
      go_on_(seen);
      changed = true;
    }
    return changed;
  }

  // The card's key: on to the next step whether or not this one's touch was made. After the goodbye the tour is over.
  bool next(const Seen &seen) {
    if (over()) return false;
    go_on_(seen);
    return true;
  }

 private:
  void go_on_(const Seen &seen) {
    step_ = after(step_, seen);
    done_ = idle_ = false;
  }

  Step step_ = WELCOME;
  bool done_ = false;
  // The step's touch was made and nothing of it is left on the glass, and since when.
  bool idle_ = false;
  uint32_t idle_since_ = 0;
};

}  // namespace sky::tour
