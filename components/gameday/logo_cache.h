#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/runtime_image/runtime_image.h"

namespace esphome::gameday {

// Team logos, downloaded and decoded once, then kept in PSRAM until the
// panel restarts. Downloads run on their own task so a slow logo server can
// only delay a logo, never the main loop. Decoding stays on the main loop
// because ESPHome's PNG decoder feeds the main loop's watchdog.
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
  enum class State : uint8_t { QUEUED, LOADING, DOWNLOADED, READY, FAILED };
  struct Entry {
    std::string url;
    std::vector<uint8_t> body;  // the PNG, from the task to the main loop
    std::unique_ptr<runtime_image::RuntimeImage> image;
    State state{State::QUEUED};
    uint32_t used_ms{0};
    uint32_t failed_ms{0};
  };

  static void task_(void *arg);
  void run_();
  bool download_(const std::string &url, std::vector<uint8_t> &body);
  std::unique_ptr<runtime_image::RuntimeImage> decode_(std::vector<uint8_t> &body);
  void make_room_();
  void start_task_();
  bool pinned_url_(const std::string &url) const;
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
