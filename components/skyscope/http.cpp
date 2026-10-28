#include "http.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <map>

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

namespace {
// One client a server, kept open between requests: a new TLS connection costs some 4 seconds of the worker's core on an
// ESP32-S3 (its handshake), where a request on an open one costs a few hundred milliseconds. The feed is asked every few
// seconds and a map's tiles come one after another from one server, so nearly every request finds its connection open.
struct Fill {
  Body *body;
  size_t most;
  bool too_large = false, no_memory = false;
  int retry_after_s = 0;
  std::string rate_limit;
  int rate_reset_s = 0;
};
esp_err_t on_event(esp_http_client_event_t *e) {
  auto *fill = static_cast<Fill *>(e->user_data);
  if (fill && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
      strcasecmp(e->header_key, "Retry-After") == 0) {
    // Seconds; a date is taken as a minute.
    const int seconds = std::atoi(e->header_value);
    fill->retry_after_s = seconds > 0 ? std::min(seconds, 3600) : 60;
    return ESP_OK;
  }
  if (fill && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
    if (strcasecmp(e->header_key, "x-ratelimit-limit") == 0) fill->rate_limit = std::string(e->header_value).substr(0, 120);
    if (strcasecmp(e->header_key, "x-ratelimit-reset") == 0) fill->rate_reset_s = std::max(0, std::atoi(e->header_value));
    return ESP_OK;
  }
  if (!fill || !fill->body || e->event_id != HTTP_EVENT_ON_DATA || e->data_len <= 0) return ESP_OK;
  Body &b = *fill->body;
  if (b.size + e->data_len > fill->most) {
    fill->too_large = true;
    return ESP_OK;
  }
  if (b.size + e->data_len + 1 > b.room &&
      !b.reserve(std::min(fill->most + 1, std::max(b.room * 2, b.size + e->data_len + 1)))) {
    fill->no_memory = true;
    return ESP_OK;
  }
  std::memcpy(b.data + b.size, e->data, e->data_len);
  b.size += e->data_len;
  return ESP_OK;
}

std::string server_of(const std::string &url) {
  const size_t start = url.find("://");
  const size_t end = start == std::string::npos ? std::string::npos : url.find('/', start + 3);
  return url.substr(0, end);
}

struct Client {
  esp_http_client_handle_t handle = nullptr;
  Fill fill{nullptr, 0};
};
std::map<std::string, Client> clients;
constexpr size_t MOST_CLIENTS = 3;

void drop(const std::string &server) {
  auto found = clients.find(server);
  if (found == clients.end()) return;
  esp_http_client_cleanup(found->second.handle);
  clients.erase(found);
}

Client *client_for(const std::string &url, const std::string &agent) {
  const std::string server = server_of(url);
  auto found = clients.find(server);
  if (found != clients.end()) return &found->second;
  // Room for the feed's server, a map's and one more; past that one goes.
  if (clients.size() >= MOST_CLIENTS) drop(clients.begin()->first);
  Client &c = clients[server];
  esp_http_client_config_t config{};
  config.url = url.c_str();
  config.method = HTTP_METHOD_GET;
  config.timeout_ms = 12000;
  config.user_agent = agent.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.buffer_size = 4096;
  config.buffer_size_tx = 1024;
  config.max_redirection_count = 3;
  config.keep_alive_enable = true;
  config.event_handler = on_event;
  config.user_data = &c.fill;
  c.handle = esp_http_client_init(&config);
  if (!c.handle) {
    clients.erase(server);
    return nullptr;
  }
  return &c;
}
}  // namespace

Result get(const std::string &url, Body &body, size_t most, const std::string &agent, const char *accept) {
  Result r;
  for (int attempt = 0; attempt < 2; ++attempt) {
    body.clear();
    Client *c = client_for(url, agent);
    if (!c) {
      r.error = "no HTTP client";
      return r;
    }
    c->fill = Fill{&body, most};
    esp_http_client_set_url(c->handle, url.c_str());
    esp_http_client_set_header(c->handle, "Accept", accept ? accept : "*/*");
    const esp_err_t err = esp_http_client_perform(c->handle);
    c->fill.body = nullptr;
    if (err != ESP_OK) {
      // The server may have closed the connection while it sat idle: once more, on a new one.
      r.error = esp_err_to_name(err);
      r.status = 0;
      drop(server_of(url));
      continue;
    }
    r.status = esp_http_client_get_status_code(c->handle);
    r.retry_after_s = c->fill.retry_after_s;
    r.rate_limit = c->fill.rate_limit;
    r.rate_reset_s = c->fill.rate_reset_s;
    r.error.clear();
    if (c->fill.too_large) r.error = "too large";
    if (c->fill.no_memory) r.error = "no memory";
    if (!r.error.empty()) {
      r.status = 0;
      // What is left of a reply cut short would be read as the next one.
      drop(server_of(url));
    }
    break;
  }
  if (body.reserve(body.size + 1)) body.data[body.size] = 0;
  if (r.status != 200 && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
  return r;
}
}  // namespace sky::http
