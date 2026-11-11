#pragma once
// A photo of the plane picked, from Planespotters.net's photo API: its latest photo by the plane's transponder
// address. Its terms (https://www.planespotters.net/photo/api), which shape how the card shows it:
//
// - The photographer is credited in words next to the photo, and the photo leads back to its page there in one
//   obvious step: on a screen with nothing to click, a QR code of the link the API gave, shown with the photo.
// - The image is fetched straight from the address the API gave, unchanged, by the screen that shows it, and held in
//   memory only while it is shown: never written to the map card, never kept after the card closes.
// - The API's answer may be kept for up to a day; a client that is no browser sends a User-Agent that names it with a
//   way to reach its makers (__init__.py).
//
// Nothing here fetches or decodes, so the tests run on a PC.
#include <ArduinoJson.h>

#include <cctype>
#include <cstdint>
#include <string>

namespace sky::photos {

// The API's answer for a plane is kept this long (its terms allow a day), so a plane picked again asks nothing more
// than its image.
constexpr uint32_t ANSWER_KEPT_MS = 12 * 3600 * 1000;
// The most bytes of a thumbnail taken: the regular one is 200 pixels wide, some 10 KB.
constexpr size_t MOST_IMAGE_BYTES = 96 * 1024;

inline std::string url_for(const std::string &hex) {
  std::string id;
  for (char c : hex)
    if (std::isxdigit((unsigned char) c)) id += (char) std::toupper((unsigned char) c);
  return "https://api.planespotters.net/pub/photos/hex/" + id;
}

struct Photo {
  bool known = false;   // the API has a photo of it
  std::string src;      // the thumbnail's address, as given
  std::string link;     // its page at Planespotters.net, as given: the QR code's
  std::string photographer;
  int width = 0, height = 0;  // the thumbnail's, as the API says
};

// The API's answer: {"photos": [{"thumbnail": {"src", "size": {"width", "height"}}, "link", "photographer"}]}, an
// empty list for a plane with none, or {"error": "..."}. The regular thumbnail (200 pixels wide) is taken: it goes
// beside its QR code on the smallest card. A photo without its link or its photographer is none: it could not be shown
// as the terms ask.
inline bool read(const char *text, size_t length, JsonDocument &doc, Photo &out, std::string &error) {
  out = Photo{};
  if (const DeserializationError e = deserializeJson(doc, text, length)) {
    error = std::string("the photo's answer could not be read: ") + e.c_str();
    return false;
  }
  if (doc["error"].is<const char *>()) {
    error = std::string("Planespotters: ") + doc["error"].as<const char *>();
    return false;
  }
  JsonArrayConst list = doc["photos"].as<JsonArrayConst>();
  if (list.isNull()) {
    error = "the photo's answer has no list";
    return false;
  }
  if (list.size() == 0) return true;
  JsonObjectConst p = list[0];
  const char *src = p["thumbnail"]["src"] | "", *link = p["link"] | "", *by = p["photographer"] | "";
  if (std::string(src).rfind("https://", 0) != 0 || std::string(link).rfind("https://", 0) != 0 || !*by) return true;
  out.known = true;
  out.src = src;
  out.link = link;
  out.photographer = by;
  out.width = p["thumbnail"]["size"]["width"] | 0;
  out.height = p["thumbnail"]["size"]["height"] | 0;
  return true;
}

// The credit under the photo: "Photo: Thomas Noack, planespotters.net".
inline std::string credit(const Photo &p) { return "Photo: " + p.photographer + ", planespotters.net"; }

}  // namespace sky::photos
