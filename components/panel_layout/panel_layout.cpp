#include "panel_layout.h"

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace panel_layout {

static const char *const TAG = "panel_layout";

// HUB75Display keeps its Hub75Config protected and only reads it when the
// driver is created in setup(). The class is final, so no derived shim; this
// uses the explicit-instantiation rule instead: naming a protected member as
// a template argument of an explicit instantiation is exempt from access
// checks (C++ [temp.explicit]). The instantiation defines get(ConfigTag) to
// return the pointer-to-member. If a future ESPHome renames config_, makes
// the driver earlier, or otherwise moves things, this stops compiling and the
// runtime panel picker needs another route.
struct ConfigTag {
  using type = Hub75Config hub75::HUB75Display::*;
  friend type get(ConfigTag);
};
template<typename Tag, typename Tag::type M> struct Rob {
  friend typename Tag::type get(Tag) { return M; }
};
template struct Rob<ConfigTag, &hub75::HUB75Display::config_>;

static void set_layout_cols(hub75::HUB75Display *display, uint16_t cols) {
  (display->*get(ConfigTag())).layout_cols = cols;
}

void PanelLayout::setup() {
  this->pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("panel_layout_cols_v1"));
  uint8_t saved = 0;
  if (!this->pref_.load(&saved) || saved < 1 || saved > this->max_cols_)
    saved = this->default_cols_;
  this->cols_ = saved;
  if (this->display_ != nullptr)
    set_layout_cols(this->display_, this->cols_);
}

void PanelLayout::set_cols(uint8_t cols) {
  if (cols < 1 || cols > this->max_cols_) {
    ESP_LOGW(TAG, "Ignoring panel count %u (allowed 1-%u)", cols, this->max_cols_);
    return;
  }
  if (cols == this->cols_)
    return;
  ESP_LOGI(TAG, "Panel count %u -> %u, restarting", this->cols_, cols);
  this->cols_ = cols;
  this->pref_.save(&this->cols_);
  global_preferences->sync();
  App.scheduler.set_timeout(this, "reboot", 1000, []() { App.safe_reboot(); });
}

void PanelLayout::dump_config() {
  ESP_LOGCONFIG(TAG, "Panel layout:\n  Panels wide: %u (max %u)", this->cols_, this->max_cols_);
}

}  // namespace panel_layout
}  // namespace esphome
