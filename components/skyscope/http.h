#pragma once
// HTTPS GETs from the worker task: the whole body into PSRAM, checked against the certificate bundle.
#include <cstddef>
#include <cstdint>
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
};

// Closes the connections no request used for `idle_ms`: the feed's stays open (it is asked every few seconds), a tile
// server's goes once the map is drawn.
void close_idle(uint32_t now, uint32_t idle_ms);
// GETs `url` into `body`, at most `most` bytes, sending `agent` as its User-Agent.
Result get(const std::string &url, Body &body, size_t most, const std::string &agent, const char *accept = nullptr);
}  // namespace sky::http
