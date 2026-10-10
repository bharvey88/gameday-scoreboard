#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/runtime_image/runtime_image.h"

namespace esphome::gameday {

// PSRAM first, internal RAM only if PSRAM is full. Aborts on failure, as
// operator new does with exceptions off.
inline void *psram_alloc(size_t size) {
  void *p = heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
  if (p == nullptr)
    abort();
  return p;
}

template<class T> struct PsramAllocator {
  using value_type = T;
  PsramAllocator() = default;
  template<class U> PsramAllocator(const PsramAllocator<U> &) {}
  T *allocate(size_t n) { return static_cast<T *>(psram_alloc(n * sizeof(T))); }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template<class U> bool operator==(const PsramAllocator<U> &) const { return true; }
  template<class U> bool operator!=(const PsramAllocator<U> &) const { return false; }
};

// Team logos, downloaded and decoded once, then kept in PSRAM until the
// panel restarts. Downloads run on their own task so a slow logo server can
// only delay a logo, never the main loop. Decoding stays on the main loop
// because ESPHome's PNG decoder feeds the main loop's watchdog.
//
// Everything a logo allocates goes to PSRAM, not just its pixels. Plain new
// and malloc only use internal RAM in this build, so the PNG download and
// each entry's small objects used to land there, and a full cache broke
// internal RAM into pieces too small for the fetch task's 16 KB stack.
class LogoCache {
 public:
  void set_http(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void setup();

  // The rest is main loop only.

  // The decoded logo, or nullptr while it is still missing; a miss starts a download.
  image::Image *get(const std::string &url);
  // The two URLs about to be shown. They and the two before them are never
  // dropped, so nothing LVGL may still point at is freed.
  void pin(const std::string &team, const std::string &opponent);
  // Decodes finished downloads and retries failed logos that are on screen.
  // True when at least one logo became ready.
  bool loop();

 protected:
  using Bytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;
  using Url = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;

  enum class State : uint8_t { QUEUED, LOADING, DOWNLOADED, READY, FAILED };
  struct Entry {
    static void *operator new(size_t size) { return psram_alloc(size); }
    static void operator delete(void *p) { heap_caps_free(p); }

    Url url;
    Bytes body;  // the PNG, from the task to the main loop
    std::optional<runtime_image::RuntimeImage> image;
    State state{State::QUEUED};
    uint32_t used_ms{0};
    uint32_t failed_ms{0};
  };

  static void task_(void *arg);
  void run_();
  bool download_(const std::string &url, Bytes &body);
  bool decode_(std::optional<runtime_image::RuntimeImage> &image, const Bytes &body);
  void make_room_();
  void start_task_();
  bool pinned_url_(std::string_view url) const;
  void retry_pinned_(uint32_t now);

  http_request::HttpRequestComponent *http_{nullptr};
  // Guards entries_ and every Entry's state and body.
  SemaphoreHandle_t lock_{nullptr};
  std::vector<std::unique_ptr<Entry>> entries_;
  std::string pinned_[4];  // current pair, then the pair before
  uint32_t retry_check_ms_{0};
  bool task_running_{false};
  std::atomic<bool> downloaded_{false};
};

}  // namespace esphome::gameday
