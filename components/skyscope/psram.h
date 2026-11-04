#pragma once
// An allocator for the standard containers that keeps their contents in PSRAM: for records that grow with the sky (a
// plane's trail), each too small for ESP-IDF to put there by itself (under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), which
// together take more internal RAM than the screen can spare. A plain malloc on a PC, where the host tests build it.
#include <cstddef>
#include <cstdlib>

#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#endif

namespace sky {
template<typename T> struct InPsram {
  using value_type = T;
  InPsram() = default;
  template<typename U> InPsram(const InPsram<U> &) {}
  T *allocate(size_t n) {
    void *p = nullptr;
#if __has_include(<esp_heap_caps.h>)
    p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!p) p = std::malloc(n * sizeof(T));  // no PSRAM to be had: internal RAM, as without this
    if (!p) std::abort();                     // as operator new does with no exceptions
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) {
#if __has_include(<esp_heap_caps.h>)
    heap_caps_free(p);  // either heap's
#else
    std::free(p);
#endif
  }
  template<typename U> bool operator==(const InPsram<U> &) const { return true; }
  template<typename U> bool operator!=(const InPsram<U> &) const { return false; }
};
}  // namespace sky
