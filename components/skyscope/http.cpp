#include "http.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstring>

namespace sky::http {
Body::~Body() { heap_caps_free(data); }

void Body::release() {
  heap_caps_free(data);
  data = nullptr;
  size = room = 0;
}

bool Body::reserve(size_t bytes) {
  if (bytes <= room) return true;
  auto *grown = static_cast<uint8_t *>(heap_caps_realloc(data, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!grown) return false;
  data = grown;
  room = bytes;
  return true;
}

Result get(const std::string &url, Body &body, size_t most, const std::string &agent, const char *accept) {
  Result r;
  body.clear();
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 12000;
  config.user_agent = agent.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 4096;
  config.buffer_size_tx = 1024;
  config.disable_auto_redirect = false;
  config.max_redirection_count = 3;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    r.error = "no HTTP client";
    return r;
  }
  if (accept) esp_http_client_set_header(client, "Accept", accept);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    r.error = esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return r;
  }
  const int64_t length = esp_http_client_fetch_headers(client);
  r.status = esp_http_client_get_status_code(client);
  if (length > (int64_t) most) {
    r.error = "too large";
    r.status = 0;
  } else if (body.reserve(length > 0 ? (size_t) length + 1 : 32 * 1024)) {
    while (true) {
      if (body.size + 4096 + 1 > body.room && !body.reserve(std::min(most + 1, body.room * 2))) {
        r.error = "no memory";
        r.status = 0;
        break;
      }
      const size_t room = std::min<size_t>(body.room - body.size - 1, 4096);
      if (room == 0) {
        r.error = "too large";
        r.status = 0;
        break;
      }
      const int got = esp_http_client_read(client, reinterpret_cast<char *>(body.data + body.size), room);
      if (got < 0) {
        r.error = "read failed";
        r.status = 0;
        break;
      }
      if (got == 0) {
        if (esp_http_client_is_complete_data_received(client)) break;
        // A chunked body that stalled: what came is all there is.
        break;
      }
      body.size += got;
    }
    if (body.data) body.data[body.size] = 0;
  } else {
    r.error = "no memory";
    r.status = 0;
  }
  if (r.status != 200 && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return r;
}
}  // namespace sky::http
