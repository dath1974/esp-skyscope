#pragma once
// The keyboard the menu types with (editor.cpp draws it): which keys each page has, and what pressing one does to the
// text being typed. A key's width is in twentieths of its row, so a row of ten letters is ten keys of 2. The fonts have
// only letters and punctuation (no symbols), so the keys that are not characters say what they do in words, and the
// cursor's keys are drawn as arrows.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sky::keyboard {

// How a value is typed: whole numbers, numbers with a sign and a point (degrees), times of day, or anything.
enum Pad : uint8_t { NONE, WHOLE, DECIMAL, CLOCK, WORDS };
// The pages of WORDS; the other pads have one page of digits.
enum Page : uint8_t { LETTERS, NUMBERS, SYMBOLS, DIGITS };
enum Action : uint8_t { TYPE, ERASE, LEFT, RIGHT, SHIFT, TO_LETTERS, TO_NUMBERS, TO_SYMBOLS };
struct Key {
  const char *text;  // what TYPE puts in (a letter goes in as a capital with the shift on), or the key's face
  Action action = TYPE;
  uint8_t width = 2;
};
using Row = std::vector<Key>;
constexpr int ROW_WIDTH = 20;

inline bool is_letter(const char *s) { return s && s[0] >= 'a' && s[0] <= 'z' && !s[1]; }

// Every printable ASCII character as a string of its own, for a key's text.
inline const char *one(char c) {
  static const struct Table {
    char s[128][2] = {};
    Table() {
      for (int i = 32; i < 127; ++i) s[i][0] = (char) i;
    }
  } table;
  return table.s[(uint8_t) c & 0x7F];
}

inline std::vector<Row> layout(Pad pad, Page page) {
  auto keys = [](const char *chars, uint8_t width = 2) {
    Row row;
    for (const char *c = chars; *c; ++c) row.push_back({one(*c), TYPE, width});
    return row;
  };
  auto join = [](Row a, const Row &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
  };
  if (pad == WORDS) {
    const Row bottom_letters = {{"123", TO_NUMBERS, 4}, {"<", LEFT, 3}, {" ", TYPE, 10}, {">", RIGHT, 3}};
    const Row bottom_others = {{"abc", TO_LETTERS, 4}, {"<", LEFT, 3}, {" ", TYPE, 10}, {">", RIGHT, 3}};
    if (page == NUMBERS)
      return {keys("1234567890"), keys("-/:;()$&@\""),
              join(join({{"#+=", TO_SYMBOLS, 3}}, keys(".,?!'", 2)), {{"Del", ERASE, 7}}), bottom_others};
    if (page == SYMBOLS)
      return {keys("[]{}#%^*+="), keys("_\\|~<>`"),
              join(join({{"123", TO_NUMBERS, 3}}, keys(".,?!'", 2)), {{"Del", ERASE, 7}}), bottom_others};
    return {keys("qwertyuiop"), keys("asdfghjkl"), join(join({{"Aa", SHIFT, 3}}, keys("zxcvbnm")), {{"Del", ERASE, 3}}),
            bottom_letters};
  }
  // Digits, five to a row, then the cursor, whatever else the value takes, and Del.
  Row last = {{"<", LEFT, 4}};
  if (pad == DECIMAL) last = join(last, {{"-", TYPE, 4}, {".", TYPE, 4}});
  if (pad == CLOCK) last = join(last, {{":", TYPE, 4}});
  last.push_back({">", RIGHT, 4});
  int used = 0;
  for (const auto &k : last) used += k.width;
  last.push_back({"Del", ERASE, (uint8_t) (ROW_WIDTH - used)});
  return {keys("12345", 4), keys("67890", 4), last};
}

// The length of the UTF-8 character starting at `c` (1 for a stray continuation byte, so nothing is skipped forever).
inline size_t char_length(unsigned char c) {
  if (c >= 0xF0) return 4;
  if (c >= 0xE0) return 3;
  if (c >= 0xC0) return 2;
  return 1;
}
// How many characters the first `bytes` bytes of `s` hold: LVGL places its letters by character, not by byte.
inline size_t chars_in(const std::string &s, size_t bytes) {
  size_t n = 0;
  for (size_t i = 0; i < bytes && i < s.size(); i += char_length((unsigned char) s[i])) ++n;
  return n;
}
// `s` cut to at most `most` bytes, between two characters.
inline std::string clipped(const std::string &s, size_t most) {
  size_t end = 0;
  while (end < s.size()) {
    const size_t next = end + char_length((unsigned char) s[end]);
    if (next > most || next > s.size()) break;
    end = next;
  }
  return s.substr(0, end);
}

inline std::string trimmed(const std::string &s) {
  const size_t from = s.find_first_not_of(" \t\r\n"), to = s.find_last_not_of(" \t\r\n");
  return from == std::string::npos ? "" : s.substr(from, to - from + 1);
}

// The text being typed and where its cursor is, a byte offset always between two characters.
struct Editor {
  std::string text;
  size_t cursor = 0;
  size_t most = 64;  // bytes

  void start(const std::string &t, size_t most_bytes) {
    most = most_bytes;
    text = clipped(t, most);
    cursor = text.size();
  }
  bool type(const char *s) {
    const std::string add(s);
    if (add.empty() || text.size() + add.size() > most) return false;
    text.insert(cursor, add);
    cursor += add.size();
    return true;
  }
  // The character before the cursor.
  bool erase() {
    if (cursor == 0) return false;
    size_t from = cursor - 1;
    while (from > 0 && ((unsigned char) text[from] & 0xC0) == 0x80) --from;
    text.erase(from, cursor - from);
    cursor = from;
    return true;
  }
  bool left() {
    if (cursor == 0) return false;
    do --cursor;
    while (cursor > 0 && ((unsigned char) text[cursor] & 0xC0) == 0x80);
    return true;
  }
  bool right() {
    if (cursor >= text.size()) return false;
    cursor = std::min(text.size(), cursor + char_length((unsigned char) text[cursor]));
    return true;
  }
};

// A keyboard as a finger uses it: its pad, the page showing, the shift, and the text.
struct Board {
  Pad pad = WORDS;
  Page page = LETTERS;
  bool caps = false;
  Editor editor;

  void start(Pad p, const std::string &text, size_t most) {
    pad = p;
    page = p == WORDS ? LETTERS : DIGITS;
    caps = false;
    editor.start(text, most);
  }
  std::vector<Row> keys() const { return layout(pad, page); }
  // What a key shows: a letter as it would go in.
  std::string face(const Key &k) const {
    if (k.action == TYPE && caps && is_letter(k.text)) return std::string(1, (char) (k.text[0] - 32));
    if (k.action == TYPE && k.text[0] == ' ') return "space";
    return k.text;
  }
  // A key pressed. True when the keys change (another page, or the shift).
  bool press(const Key &k) {
    switch (k.action) {
      case TYPE:
        editor.type(face(k) == "space" ? " " : face(k).c_str());
        return false;
      case ERASE: editor.erase(); return false;
      case LEFT: editor.left(); return false;
      case RIGHT: editor.right(); return false;
      case SHIFT: caps = !caps; return true;
      case TO_LETTERS: page = LETTERS; return true;
      case TO_NUMBERS: page = NUMBERS; return true;
      case TO_SYMBOLS: page = SYMBOLS; return true;
    }
    return false;
  }
};
}  // namespace sky::keyboard
