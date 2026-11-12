#pragma once
// HTTPS GETs (and the odd POST) from the worker task: the whole body into PSRAM, checked against the certificate
// bundle.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace sky::http {
// A body in PSRAM, grown as it comes.
struct Body {
  uint8_t *data = nullptr;
  size_t size = 0, room = 0;
  Body() = default;
  Body(const Body &) = delete;
  Body &operator=(const Body &) = delete;
  ~Body();
  bool reserve(size_t bytes);
  void clear() { size = 0; }
  void release();
};
struct Result {
  int status = 0;       // the HTTP status; 0 when no answer came
  std::string error;    // why not, when status is not 200
  int retry_after_s = 0;  // the server's Retry-After, in seconds, when it sent one
  // A server's announced limits (x-ratelimit-limit, as Envoy writes it: "500, 4;w=1, 60;w=60, 500;w=86400") and the
  // seconds until its quota comes back (x-ratelimit-reset); 0 when it sent none.
  std::string rate_limit;
  int rate_reset_s = 0;
  // How long the server lets its reply be kept (Cache-Control max-age), in seconds: 0 for no-store or no-cache, -1
  // when it said nothing.
  int max_age_s = -1;
};

// The seconds a Cache-Control header lets a reply be kept: its max-age (s-maxage is for shared caches, not this one), 0
// for no-store or no-cache, -1 for neither.
inline int max_age(const char *header) {
  if (!header) return -1;
  int out = -1;
  const char *p = header;
  while (*p) {
    while (*p == ' ' || *p == ',') ++p;
    const char *start = p;
    while (*p && *p != ',') ++p;
    const std::string part(start, p - start);
    if (part.rfind("no-store", 0) == 0 || part.rfind("no-cache", 0) == 0) return 0;
    if (part.rfind("max-age=", 0) == 0) {
      long v = std::strtol(part.c_str() + 8, nullptr, 10);
      out = (int) std::max<long>(0, std::min<long>(v, 365L * 86400));
    }
  }
  return out;
}

// A test's mock in place of every server (the test API's /api/test {"divert": origin}): `url` asked of `origin`
// ("http://192.168.1.5:8095") instead, its server and path kept as the path, so one mock answers for every service and
// can tell them apart: https://tile.openstreetmap.org/9/150/192.png is asked as
// http://192.168.1.5:8095/tile.openstreetmap.org/9/150/192.png. Every public service is HTTPS; a plain http:// address
// (a receiver on the network, the mock itself, one that cannot be found) is asked as it is.
inline std::string diverted(const std::string &url, const std::string &origin) {
  static constexpr char HTTPS[] = "https://";
  if (origin.empty() || url.rfind(HTTPS, 0) != 0) return url;
  return origin + "/" + url.substr(sizeof(HTTPS) - 1);
}
// Every request to `origin` from now on ("" for none). Set as the screen starts, before the worker asks for anything.
void divert_to(const std::string &origin);

// Closes the connections no request used for `idle_ms`: the feed's stays open (it is asked every few seconds), a tile
// server's goes once the map is drawn.
void close_idle(uint32_t now, uint32_t idle_ms);
// GETs `url` into `body`, at most `most` bytes, sending `agent` as its User-Agent.
Result get(const std::string &url, Body &body, size_t most, const std::string &agent, const char *accept = nullptr);
// POSTs `json` to `url`, its answer into `body` as get() does.
Result post_json(const std::string &url, const std::string &json, Body &body, size_t most, const std::string &agent);
}  // namespace sky::http
