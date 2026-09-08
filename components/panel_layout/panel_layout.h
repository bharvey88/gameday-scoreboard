#pragma once

#include "esphome/components/hub75/hub75_component.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace panel_layout {

// Applies a saved panel count to the HUB75 display before its driver starts,
// then restarts the device when the count changes. Runs just ahead of the
// display in setup order; LVGL sizes itself from the display after that.
class PanelLayout : public Component {
 public:
  void set_display(hub75::HUB75Display *display) { this->display_ = display; }
  void set_default_cols(uint8_t cols) { this->default_cols_ = cols; }
  void set_max_cols(uint8_t cols) { this->max_cols_ = cols; }

  uint8_t cols() const { return this->cols_; }
  // Saves the new count and reboots a second later so the page can answer.
  void set_cols(uint8_t cols);

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR + 10.0f; }

 protected:
  hub75::HUB75Display *display_{nullptr};
  uint8_t default_cols_{1};
  uint8_t max_cols_{2};
  uint8_t cols_{1};
  ESPPreferenceObject pref_;
};

}  // namespace panel_layout
}  // namespace esphome
