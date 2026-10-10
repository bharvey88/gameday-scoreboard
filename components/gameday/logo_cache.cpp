#include "logo_cache.h"

#include "esphome/components/runtime_image/image_decoder.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "logo_lru.h"

namespace esphome::gameday {

static const char *const TAG = "gameday.logo";
static const char *const USER_AGENT = "curl/8.0 gameday-scoreboard";
// About four logos per team on screen at once in the busiest playlist, times
// a full favorites list plus live games, with room to spare. Each is ~3 KB.
static const size_t MAX_LOGOS = 48;
// ESPN's PNGs are a few KB; anything far bigger is not a logo.
static const size_t MAX_BODY = 128 * 1024;
static const int LOGO_SIZE = 32;

class Locked {
 public:
  explicit Locked(SemaphoreHandle_t lock) : lock_(lock) { xSemaphoreTake(lock_, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock_); }

 private:
  SemaphoreHandle_t lock_;
};

void LogoCache::setup() {
  this->lock_ = xSemaphoreCreateMutex();
  this->entries_.reserve(MAX_LOGOS);
}

image::Image *LogoCache::get(const std::string &url) {
  if (url.empty() || this->lock_ == nullptr)
    return nullptr;
  Locked guard(this->lock_);
  uint32_t now = millis();
  for (auto &e : this->entries_) {
    if (std::string_view(e->url) != url)
      continue;
    e->used_ms = now;
    if (e->state == State::READY)
      return &*e->image;
    if (e->state == State::FAILED && ::espn::logo_retry_due(e->failed_ms, now)) {
      e->state = State::QUEUED;
      this->start_task_();
    }
    return nullptr;
  }
  this->make_room_();
  auto e = std::make_unique<Entry>();
  e->url.assign(url.data(), url.size());
  e->used_ms = now;
  this->entries_.push_back(std::move(e));
  this->start_task_();
  return nullptr;
}

void LogoCache::pin(const std::string &team, const std::string &opponent) {
  if (this->lock_ == nullptr || (team == this->pinned_[0] && opponent == this->pinned_[1]))
    return;
  Locked guard(this->lock_);
  this->pinned_[2] = std::move(this->pinned_[0]);
  this->pinned_[3] = std::move(this->pinned_[1]);
  this->pinned_[0] = team;
  this->pinned_[1] = opponent;
}

bool LogoCache::pinned_url_(std::string_view url) const {
  for (const auto &p : this->pinned_) {
    if (!p.empty() && p == url)
      return true;
  }
  return false;
}

// Main loop. A failed logo still on screen is asked for again on its own, so
// it does not wait for the next board update, which can be hours away.
void LogoCache::retry_pinned_(uint32_t now) {
  if (now - this->retry_check_ms_ < 1000)
    return;
  this->retry_check_ms_ = now;
  Locked guard(this->lock_);
  for (auto &e : this->entries_) {
    if (e->state == State::FAILED && this->pinned_url_(e->url) && ::espn::logo_retry_due(e->failed_ms, now)) {
      e->state = State::QUEUED;
      this->start_task_();
    }
  }
}

// Caller holds the lock. Drops the least recently used logo that is idle and
// off screen, so the image it frees is not one LVGL is drawing.
void LogoCache::make_room_() {
  if (this->entries_.size() < MAX_LOGOS)
    return;
  std::vector<::espn::LogoSlot> slots;
  slots.reserve(this->entries_.size());
  for (auto &e : this->entries_) {
    bool busy = e->state == State::QUEUED || e->state == State::LOADING || e->state == State::DOWNLOADED;
    bool pinned = this->pinned_url_(e->url);
    slots.push_back({e->used_ms, busy, pinned});
  }
  int i = ::espn::pick_evict(slots);
  if (i >= 0)
    this->entries_.erase(this->entries_.begin() + i);
}

// Caller holds the lock.
void LogoCache::start_task_() {
  if (this->task_running_)
    return;
  this->task_running_ = true;
  // Same stack as the ESPN worker: the TLS handshake needs it.
  if (xTaskCreate(&LogoCache::task_, "gameday_logo", 16384, this, 1, nullptr) != pdPASS) {
    ESP_LOGW(TAG, "Could not start the logo task (largest free internal block %u bytes)",
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    this->task_running_ = false;
  }
}

void LogoCache::task_(void *arg) {
  static_cast<LogoCache *>(arg)->run_();
  vTaskDelete(nullptr);
}

// Logo task: works through queued logos one at a time, then exits. The task
// only touches an entry while it is LOADING, and nothing drops a busy entry.
void LogoCache::run_() {
  while (true) {
    Entry *job = nullptr;
    std::string url;
    {
      Locked guard(this->lock_);
      for (auto &e : this->entries_) {
        if (e->state == State::QUEUED) {
          job = e.get();
          break;
        }
      }
      if (job == nullptr) {
        this->task_running_ = false;  // under the lock, so get() starts a new task for anything queued after this
        return;
      }
      job->state = State::LOADING;
      url.assign(job->url.data(), job->url.size());
    }
    Bytes body;
    uint32_t start = millis();
    bool ok = this->download_(url, body);
    size_t bytes = body.size();
    {
      Locked guard(this->lock_);
      if (ok) {
        job->body = std::move(body);
        job->state = State::DOWNLOADED;
      } else {
        job->state = State::FAILED;
        job->failed_ms = millis();
      }
    }
    if (ok) {
      ESP_LOGD(TAG, "Downloaded %s (%u bytes, %u ms)", url.c_str(), (unsigned) bytes, (unsigned) (millis() - start));
    } else {
      ESP_LOGW(TAG, "Logo download failed after %u ms: %s", (unsigned) (millis() - start), url.c_str());
    }
    this->downloaded_ = true;
  }
}

bool LogoCache::download_(const std::string &url, Bytes &body) {
  std::vector<http_request::Header> headers = {
      {"User-Agent", USER_AGENT},
      {"Accept", "image/png,*/*;q=0.8"},
  };
  auto container = this->http_->get(url, headers);
  if (container == nullptr)
    return false;
  if (container->status_code != 200) {
    ESP_LOGW(TAG, "HTTP %d for %s", container->status_code, url.c_str());
    container->end();
    return false;
  }
  if (container->content_length > 0 && container->content_length <= MAX_BODY)
    body.reserve(container->content_length);
  uint8_t buf[1024];
  while (body.size() < MAX_BODY) {
    int n = container->read(buf, sizeof(buf));
    if (n <= 0)
      break;
    body.insert(body.end(), buf, buf + n);
  }
  container->end();
  if (body.empty() || (container->content_length > 0 && body.size() < container->content_length))
    return false;
  return true;
}

// Decodes into the entry itself, so the image object lives in PSRAM with it.
// Safe without the lock: the entry is DOWNLOADED until the caller marks it
// READY, so nothing draws it and nothing drops it.
bool LogoCache::decode_(std::optional<runtime_image::RuntimeImage> &image, const Bytes &body) {
  image.emplace(runtime_image::PNG, image::IMAGE_TYPE_RGB565, image::TRANSPARENCY_ALPHA_CHANNEL, nullptr, false,
                LOGO_SIZE, LOGO_SIZE);
  bool ok = image->begin_decode(body.size());
  size_t offset = 0;
  while (ok && offset < body.size()) {
    // feed_data takes a non-const pointer but only reads it.
    int used = image->feed_data(const_cast<uint8_t *>(body.data()) + offset, body.size() - offset);
    if (used < 0) {
      ESP_LOGW(TAG, "Logo decode error: %s", runtime_image::decode_error_to_string(used));
      ok = false;
    }
    if (used <= 0)
      break;
    offset += (size_t) used;
  }
  if (ok)
    ok = image->end_decode();
  if (!ok)
    image.reset();
  return ok;
}

bool LogoCache::loop() {
  if (this->lock_ == nullptr)
    return false;
  this->retry_pinned_(millis());
  if (!this->downloaded_.exchange(false))
    return false;
  bool ready = false;
  while (true) {
    Entry *job = nullptr;
    Bytes body;
    {
      Locked guard(this->lock_);
      for (auto &e : this->entries_) {
        if (e->state == State::DOWNLOADED) {
          job = e.get();
          body = std::move(e->body);
          e->body = {};
          break;
        }
      }
    }
    if (job == nullptr)
      return ready;
    // Only the main loop drops entries, so job stays valid without the lock.
    bool ok = this->decode_(job->image, body);
    Locked guard(this->lock_);
    if (ok) {
      job->state = State::READY;
      ready = true;
    } else {
      job->state = State::FAILED;
      job->failed_ms = millis();
    }
  }
}

}  // namespace esphome::gameday
