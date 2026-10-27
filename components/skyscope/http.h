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
};
// GETs `url` into `body`, at most `most` bytes, sending `agent` as its User-Agent.
Result get(const std::string &url, Body &body, size_t most, const std::string &agent, const char *accept = nullptr);
}  // namespace sky::http
