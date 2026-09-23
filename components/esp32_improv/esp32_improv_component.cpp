// Local override of ESPHome 2026.8.2 esphome/components/esp32_improv
// (ESPHome is GPLv3 for C++, MIT for Python; https://github.com/esphome/esphome).
// Game Day change: answers Improv RPC 0x04 (Get Wi-Fi Networks) over BLE.
// Every change from upstream sits between "GAMEDAY: begin" and "GAMEDAY: end".

#include "esp32_improv_component.h"

#include "esphome/components/bytebuffer/bytebuffer.h"
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_2902.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_PROVISIONING
#include "esphome/components/provisioning/provisioning.h"
#endif

#ifdef USE_ESP32

// GAMEDAY: begin
#include <esp_wifi.h>

#include <algorithm>
// GAMEDAY: end

namespace esphome::esp32_improv {

using namespace bytebuffer;

static const char *const TAG = "esp32_improv.component";
static constexpr size_t IMPROV_MAX_LOG_BYTES = 128;
static const char *const ESPHOME_MY_LINK = "https://my.home-assistant.io/redirect/config_flow_start?domain=esphome";
static constexpr uint16_t STOP_ADVERTISING_DELAY =
    10000;  // Delay (ms) before stopping service to allow BLE clients to read the final state
static constexpr uint16_t NAME_ADVERTISING_INTERVAL = 60000;  // Advertise name every 60 seconds
static constexpr uint16_t NAME_ADVERTISING_DURATION = 1000;   // Advertise name for 1 second

// Improv service data constants
static constexpr uint8_t IMPROV_SERVICE_DATA_SIZE = 8;
static constexpr uint8_t IMPROV_PROTOCOL_ID_1 = 0x77;  // 'P' << 1 | 'R' >> 7
static constexpr uint8_t IMPROV_PROTOCOL_ID_2 = 0x46;  // 'I' << 1 | 'M' >> 7

// GAMEDAY: begin (Get Wi-Fi Networks, RPC 0x04)
// A scan that finished this recently is reused instead of starting another.
static constexpr uint32_t NETWORKS_FRESH_MS = 15000;
// Give up waiting for a scan after this long and send whatever results exist.
static constexpr uint32_t NETWORKS_SCAN_TIMEOUT_MS = 10000;
// Every result goes out on the same characteristic, so each notification
// replaces the last one. Leave the phone time to receive each before the next.
static constexpr uint32_t NETWORKS_SEND_INTERVAL_MS = 100;
static constexpr size_t NETWORKS_MAX = 20;
// GAMEDAY: end

ESP32ImprovComponent::ESP32ImprovComponent() { global_improv_component = this; }

void ESP32ImprovComponent::setup() {
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr) {
    this->authorizer_->add_on_state_callback([this](bool state) {
      if (state) {
        this->authorized_start_ = millis();
        this->identify_start_ = 0;
      }
    });
  }
#endif
  global_ble_server->on_disconnect([this](uint16_t conn_id) {
    this->set_error_(improv::ERROR_NONE);
    this->cancel_wifi_networks_();  // GAMEDAY
  });
  wifi::global_wifi_component->add_scan_results_listener(this);  // GAMEDAY

#ifdef USE_PROVISIONING
  if (provisioning::global_provisioning_manager != nullptr) {
    provisioning::global_provisioning_manager->add_on_closed_callback([this]() {
      ESP_LOGD(TAG, "Provisioning window closed; stopping Improv");
      this->stop();
    });
  }
#endif

  // Start with loop disabled - will be enabled by start() when needed
  this->disable_loop();
}

void ESP32ImprovComponent::setup_characteristics() {
  this->status_ = this->service_->create_characteristic(
      improv::STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *status_descriptor = new BLE2902();
  this->status_->add_descriptor(status_descriptor);

  this->error_ = this->service_->create_characteristic(
      improv::ERROR_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *error_descriptor = new BLE2902();
  this->error_->add_descriptor(error_descriptor);

  this->rpc_ = this->service_->create_characteristic(improv::RPC_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  this->rpc_->on_write([this](std::span<const uint8_t> data, uint16_t id) {
    if (!data.empty()) {
      this->incoming_data_.insert(this->incoming_data_.end(), data.begin(), data.end());
    }
  });
  BLEDescriptor *rpc_descriptor = new BLE2902();
  this->rpc_->add_descriptor(rpc_descriptor);

  this->rpc_response_ = this->service_->create_characteristic(
      improv::RPC_RESULT_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  BLEDescriptor *rpc_response_descriptor = new BLE2902();
  this->rpc_response_->add_descriptor(rpc_response_descriptor);

  this->capabilities_ =
      this->service_->create_characteristic(improv::CAPABILITIES_UUID, BLECharacteristic::PROPERTY_READ);
  BLEDescriptor *capabilities_descriptor = new BLE2902();
  this->capabilities_->add_descriptor(capabilities_descriptor);
  uint8_t capabilities = 0x00;
#ifdef USE_OUTPUT
  if (this->status_indicator_ != nullptr)
    capabilities |= improv::CAPABILITY_IDENTIFY;
#endif
  this->capabilities_->set_value(ByteBuffer::wrap(capabilities));
  this->setup_complete_ = true;
}

void ESP32ImprovComponent::loop() {
  if (!global_ble_server->is_running()) {
    if (this->state_ != improv::STATE_STOPPED) {
      this->state_ = improv::STATE_STOPPED;
#ifdef USE_ESP32_IMPROV_STATE_CALLBACK
      this->state_callback_.call(this->state_, this->error_state_);
#endif
    }
    this->incoming_data_.clear();
    this->cancel_wifi_networks_();  // GAMEDAY
    return;
  }
  if (this->service_ == nullptr) {
    // Setup the service
    ESP_LOGD(TAG, "Creating Improv service");
    this->service_ = global_ble_server->create_service(ESPBTUUID::from_raw(improv::SERVICE_UUID), true);
    this->setup_characteristics();
  }

  if (!this->incoming_data_.empty())
    this->process_incoming_data_();
  uint32_t now = App.get_loop_component_start_time();

  // GAMEDAY: begin
  if (this->networks_wait_scan_ && now - this->networks_scan_start_ > NETWORKS_SCAN_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Wi-Fi scan did not finish; sending the last results");
    this->queue_wifi_networks_();
  }
  if (this->networks_pending_)
    this->send_next_wifi_network_(now);
  // GAMEDAY: end

  // Check if we need to update advertising type
  if (this->state_ != improv::STATE_STOPPED && this->state_ != improv::STATE_PROVISIONED) {
    this->update_advertising_type_();
  }

  switch (this->state_) {
    case improv::STATE_STOPPED:
      this->set_status_indicator_state_(false);

      if (this->should_start_ && this->setup_complete_) {
        if (this->service_->is_created()) {
          this->service_->start();
        } else if (this->service_->is_running()) {
          // Start by advertising the device name first BEFORE setting any state
          ESP_LOGV(TAG, "Starting with device name advertising");
          this->advertising_device_name_ = true;
          this->last_name_adv_time_ = App.get_loop_component_start_time();
          esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>{}, true);
          esp32_ble::global_ble->advertising_start();

          // Set initial state based on whether we have an authorizer
          this->set_state_(this->get_initial_state_(), false);
          this->set_error_(improv::ERROR_NONE);
          this->should_start_ = false;  // Clear flag after starting
          ESP_LOGD(TAG, "Service started!");
        }
      }
      break;
    case improv::STATE_AWAITING_AUTHORIZATION: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ == nullptr ||
          (this->authorized_start_ != 0 && ((now - this->authorized_start_) < this->authorized_duration_))) {
        this->set_state_(improv::STATE_AUTHORIZED);
      } else {
        if (!this->check_identify_())
          this->set_status_indicator_state_(true);
      }
#else
      this->set_state_(improv::STATE_AUTHORIZED);
#endif
      this->check_wifi_connection_();
      break;
    }
    case improv::STATE_AUTHORIZED: {
#ifdef USE_BINARY_SENSOR
      if (this->authorizer_ != nullptr && now - this->authorized_start_ > this->authorized_duration_) {
        ESP_LOGD(TAG, "Authorization timeout");
        this->set_state_(improv::STATE_AWAITING_AUTHORIZATION);
        return;
      }
#endif
      if (!this->check_identify_()) {
        this->set_status_indicator_state_((now % 1000) < 500);
      }
      this->check_wifi_connection_();
      break;
    }
    case improv::STATE_PROVISIONING: {
      this->set_status_indicator_state_((now % 200) < 100);
      this->check_wifi_connection_();
      break;
    }
    case improv::STATE_PROVISIONED: {
      this->incoming_data_.clear();
      this->cancel_wifi_networks_();  // GAMEDAY
      this->set_status_indicator_state_(false);
      // Provisioning complete, no further loop execution needed
      this->disable_loop();
      break;
    }
  }
}

void ESP32ImprovComponent::set_status_indicator_state_(bool state) {
#ifdef USE_OUTPUT
  if (this->status_indicator_ == nullptr)
    return;
  if (this->status_indicator_state_ == state)
    return;
  this->status_indicator_state_ = state;
  if (state) {
    this->status_indicator_->turn_on();
  } else {
    this->status_indicator_->turn_off();
  }
#endif
}

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
const char *ESP32ImprovComponent::state_to_string_(improv::State state) {
  switch (state) {
    case improv::STATE_STOPPED:
      return "STOPPED";
    case improv::STATE_AWAITING_AUTHORIZATION:
      return "AWAITING_AUTHORIZATION";
    case improv::STATE_AUTHORIZED:
      return "AUTHORIZED";
    case improv::STATE_PROVISIONING:
      return "PROVISIONING";
    case improv::STATE_PROVISIONED:
      return "PROVISIONED";
    default:
      return "UNKNOWN";
  }
}
#endif

bool ESP32ImprovComponent::check_identify_() {
  uint32_t now = millis();

  bool identify = this->identify_start_ != 0 && now - this->identify_start_ <= this->identify_duration_;

  if (identify) {
    uint32_t time = now % 1000;
    this->set_status_indicator_state_(time < 600 && time % 200 < 100);
  }
  return identify;
}

void ESP32ImprovComponent::set_state_(improv::State state, bool update_advertising) {
  // Skip if state hasn't changed
  if (this->state_ == state) {
    return;
  }

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  ESP_LOGD(TAG, "State transition: %s (0x%02X) -> %s (0x%02X)", this->state_to_string_(this->state_), this->state_,
           this->state_to_string_(state), state);
#endif
  this->state_ = state;
  if (this->status_ != nullptr && (this->status_->get_value().empty() || this->status_->get_value()[0] != state)) {
    this->status_->set_value(ByteBuffer::wrap(static_cast<uint8_t>(state)));
    if (state != improv::STATE_STOPPED)
      this->status_->notify();
  }
  // Only advertise valid Improv states (0x01-0x04).
  // STATE_STOPPED (0x00) is internal only and not part of the Improv spec.
  // Advertising 0x00 causes undefined behavior in some clients and makes them
  // repeatedly connect trying to determine the actual state.
  if (state != improv::STATE_STOPPED && update_advertising) {
    // State change always overrides name advertising and resets the timer
    this->advertising_device_name_ = false;
    // Reset the timer so we wait another 60 seconds before advertising name
    this->last_name_adv_time_ = App.get_loop_component_start_time();
    // Advertise the new state via service data
    this->advertise_service_data_();
  }
#ifdef USE_ESP32_IMPROV_STATE_CALLBACK
  this->state_callback_.call(this->state_, this->error_state_);
#endif
}

void ESP32ImprovComponent::set_error_(improv::Error error) {
  if (error != improv::ERROR_NONE) {
    ESP_LOGE(TAG, "Error: %d", error);
  }
  // The error_ characteristic is initialized in setup_characteristics() which is called
  // from the loop, while the BLE disconnect callback is registered in setup().
  // error_ can be nullptr if:
  // 1. A client connects/disconnects before setup_characteristics() is called
  // 2. The device is already provisioned so the service never starts (should_start_ is false)
  if (this->error_ != nullptr && (this->error_->get_value().empty() || this->error_->get_value()[0] != error)) {
    this->error_->set_value(ByteBuffer::wrap(static_cast<uint8_t>(error)));
    if (this->state_ != improv::STATE_STOPPED)
      this->error_->notify();
  }
}

void ESP32ImprovComponent::send_response_(std::vector<uint8_t> &&response) {
  this->rpc_response_->set_value(std::move(response));
  if (this->state_ != improv::STATE_STOPPED)
    this->rpc_response_->notify();
}

void ESP32ImprovComponent::start() {
  if (this->should_start_ || this->state_ != improv::STATE_STOPPED)
    return;

#ifdef USE_PROVISIONING
  // Don't (re)start advertising once the provisioning window has closed - e.g. when
  // wifi tries to restart Improv after the window expired at runtime.
  if (provisioning::global_provisioning_manager != nullptr && provisioning::global_provisioning_manager->closed()) {
    ESP_LOGD(TAG, "Provisioning window closed; not starting Improv");
    return;
  }
#endif

  ESP_LOGD(TAG, "Setting Improv to start");
  this->should_start_ = true;
  this->enable_loop();
}

void ESP32ImprovComponent::stop() {
  this->should_start_ = false;
  // Wait before stopping the service to ensure all BLE clients see the state change.
  // This prevents clients from repeatedly reconnecting and wasting resources by allowing
  // them to observe that the device is provisioned before the service disappears.
  this->set_timeout("end-service", STOP_ADVERTISING_DELAY, [this] {
    if (this->state_ == improv::STATE_STOPPED || this->service_ == nullptr)
      return;
    this->service_->stop();
    this->set_state_(improv::STATE_STOPPED);
  });
}

float ESP32ImprovComponent::get_setup_priority() const { return setup_priority::AFTER_BLUETOOTH; }

void ESP32ImprovComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP32 Improv:");
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Authorizer", this->authorizer_);
#endif
#ifdef USE_OUTPUT
  ESP_LOGCONFIG(TAG, "  Status Indicator: '%s'", YESNO(this->status_indicator_ != nullptr));
#endif
}

void ESP32ImprovComponent::process_incoming_data_() {
  if (this->incoming_data_.size() < 3)
    return;
  uint8_t length = this->incoming_data_[1];

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex_buf[format_hex_pretty_size(IMPROV_MAX_LOG_BYTES)];
  ESP_LOGV(TAG, "Processing bytes - %s",
           format_hex_pretty_to(hex_buf, this->incoming_data_.data(), this->incoming_data_.size()));
#endif
  if (this->incoming_data_.size() - 3 == length) {
    this->set_error_(improv::ERROR_NONE);
    improv::ImprovCommand command = improv::parse_improv_data(this->incoming_data_);
    switch (command.command) {
      case improv::BAD_CHECKSUM:
        ESP_LOGW(TAG, "Error decoding Improv payload");
        this->set_error_(improv::ERROR_INVALID_RPC);
        this->incoming_data_.clear();
        break;
      case improv::WIFI_SETTINGS: {
        if (this->state_ != improv::STATE_AUTHORIZED) {
          ESP_LOGW(TAG, "Settings received, but not authorized");
          this->set_error_(improv::ERROR_NOT_AUTHORIZED);
          this->incoming_data_.clear();
          return;
        }
#ifdef USE_PROVISIONING
        if (provisioning::global_provisioning_manager != nullptr &&
            provisioning::global_provisioning_manager->closed()) {
          ESP_LOGW(TAG, "Provisioning window closed; refusing settings");
          this->set_error_(improv::ERROR_NOT_AUTHORIZED);
          this->incoming_data_.clear();
          return;
        }
#endif
        if (wifi::global_wifi_component->is_disabled()) {
          // Wi-Fi is disabled, so we can't provision. Respond immediately
          // instead of letting the client wait out its provisioning timeout.
          ESP_LOGW(TAG, "Wi-Fi is disabled; cannot provision");
          this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
          this->incoming_data_.clear();
          return;
        }
        this->cancel_wifi_networks_();  // GAMEDAY: the settings answer must not be overwritten
        wifi::WiFiAP sta{};
        sta.set_ssid(command.ssid.c_str());
        sta.set_password(command.password.c_str());
        this->connecting_sta_ = sta;

        wifi::global_wifi_component->set_sta(sta);
        wifi::global_wifi_component->start_connecting(sta);
        this->set_state_(improv::STATE_PROVISIONING);
        ESP_LOGD(TAG, "Received Improv Wi-Fi settings ssid=%s, password=" LOG_SECRET("%s"), command.ssid.c_str(),
                 command.password.c_str());

        this->set_timeout("wifi-connect-timeout", 30000, [this]() { this->on_wifi_connect_timeout_(); });
        this->incoming_data_.clear();
        break;
      }
      case improv::IDENTIFY:
        this->incoming_data_.clear();
        this->identify_start_ = millis();
        break;
      // GAMEDAY: begin
      case improv::GET_WIFI_NETWORKS:
        this->incoming_data_.clear();
        this->handle_get_wifi_networks_();
        break;
      // GAMEDAY: end
      default:
        ESP_LOGW(TAG, "Unknown Improv payload");
        this->set_error_(improv::ERROR_UNKNOWN_RPC);
        this->incoming_data_.clear();
    }
  } else if (this->incoming_data_.size() - 2 > length) {
    ESP_LOGV(TAG, "Too much data received or data malformed; resetting buffer");
    this->incoming_data_.clear();
  } else {
    ESP_LOGV(TAG, "Waiting for split data packets");
  }
}

void ESP32ImprovComponent::on_wifi_connect_timeout_() {
  this->set_error_(improv::ERROR_UNABLE_TO_CONNECT);
  this->set_state_(improv::STATE_AUTHORIZED);
#ifdef USE_BINARY_SENSOR
  if (this->authorizer_ != nullptr)
    this->authorized_start_ = millis();
#endif
  ESP_LOGW(TAG, "Timed out while connecting to Wi-Fi network");
  wifi::global_wifi_component->clear_sta();
}

void ESP32ImprovComponent::check_wifi_connection_() {
  if (!wifi::global_wifi_component->is_connected()) {
    return;
  }

  if (this->state_ == improv::STATE_PROVISIONING) {
    wifi::global_wifi_component->save_wifi_sta(this->connecting_sta_.get_ssid(), this->connecting_sta_.get_password());
    this->connecting_sta_ = {};
    this->cancel_timeout("wifi-connect-timeout");

    // Build URL list with minimal allocations
    // Maximum 3 URLs: custom next_url + ESPHOME_MY_LINK + webserver URL
    std::string url_strings[3];
    size_t url_count = 0;

#ifdef USE_ESP32_IMPROV_NEXT_URL
    // Add next_url if configured (should be first per Improv BLE spec)
    {
      char url_buffer[384];
      size_t len = this->get_formatted_next_url_(url_buffer, sizeof(url_buffer));
      if (len > 0) {
        url_strings[url_count++] = std::string(url_buffer, len);
      }
    }
#endif

    // Add default URLs for backward compatibility
    url_strings[url_count++] = ESPHOME_MY_LINK;
#ifdef USE_WEBSERVER
    for (auto &ip : wifi::global_wifi_component->wifi_sta_ip_addresses()) {
      if (ip.is_ip4()) {
        // "http://" (7) + IPv4 max (15) + ":" (1) + port max (5) + null = 29
        char url_buffer[32];
        memcpy(url_buffer, "http://", 7);  // NOLINT(bugprone-not-null-terminated-result) - str_to null-terminates
        ip.str_to(url_buffer + 7);
        size_t len = strlen(url_buffer);
        snprintf(url_buffer + len, sizeof(url_buffer) - len, ":%d", USE_WEBSERVER_PORT);
        url_strings[url_count++] = url_buffer;
        break;
      }
    }
#endif
    this->send_response_(improv::build_rpc_response(improv::WIFI_SETTINGS,
                                                    std::vector<std::string>(url_strings, url_strings + url_count)));
  } else if (this->is_active() && this->state_ != improv::STATE_PROVISIONED) {
    ESP_LOGD(TAG, "WiFi provisioned externally");
  }

  this->set_state_(improv::STATE_PROVISIONED);
  this->stop();
}

void ESP32ImprovComponent::advertise_service_data_() {
  uint8_t service_data[IMPROV_SERVICE_DATA_SIZE] = {};
  service_data[0] = IMPROV_PROTOCOL_ID_1;  // PR
  service_data[1] = IMPROV_PROTOCOL_ID_2;  // IM
  service_data[2] = static_cast<uint8_t>(this->state_);

  uint8_t capabilities = 0x00;
#ifdef USE_OUTPUT
  if (this->status_indicator_ != nullptr)
    capabilities |= improv::CAPABILITY_IDENTIFY;
#endif

  service_data[3] = capabilities;
  // service_data[4-7] are already 0 (Reserved)

  // Atomically set service data and disable name in advertising
  esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>(service_data), false);
}

void ESP32ImprovComponent::update_advertising_type_() {
  uint32_t now = App.get_loop_component_start_time();

  // If we're advertising the device name and it's been more than NAME_ADVERTISING_DURATION, switch back to service data
  if (this->advertising_device_name_) {
    if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_DURATION) {
      ESP_LOGV(TAG, "Switching back to service data advertising");
      this->advertising_device_name_ = false;
      // Restore service data advertising
      this->advertise_service_data_();
    }
    return;
  }

  // Check if it's time to advertise the device name (every NAME_ADVERTISING_INTERVAL)
  if (now - this->last_name_adv_time_ >= NAME_ADVERTISING_INTERVAL) {
    ESP_LOGV(TAG, "Switching to device name advertising");
    this->advertising_device_name_ = true;
    this->last_name_adv_time_ = now;

    // Atomically clear service data and enable name in advertising data
    esp32_ble::global_ble->advertising_set_service_data_and_name(std::span<const uint8_t>{}, true);
  }
}

improv::State ESP32ImprovComponent::get_initial_state_() const {
#ifdef USE_BINARY_SENSOR
  // If we have an authorizer, start in awaiting authorization state
  return this->authorizer_ == nullptr ? improv::STATE_AUTHORIZED : improv::STATE_AWAITING_AUTHORIZATION;
#else
  // No binary_sensor support = no authorizer possible, start as authorized
  return improv::STATE_AUTHORIZED;
#endif
}

// GAMEDAY: begin (Get Wi-Fi Networks, RPC 0x04)
//
// Answered the way improv_serial answers it: one RPC result per network,
// [0x04][len][ssid][rssi][YES|NO][checksum], then a result with no strings to
// end the list. Networks are deduplicated by SSID (strongest access point
// wins), sorted strongest first and capped at NETWORKS_MAX.
void ESP32ImprovComponent::handle_get_wifi_networks_() {
  if (this->networks_pending_ || this->networks_wait_scan_) {
    ESP_LOGD(TAG, "Wi-Fi network list already on its way");
    return;
  }
  const uint32_t now = millis();
  auto *wifi = wifi::global_wifi_component;
  bool fresh = this->last_scan_done_ != 0 && now - this->last_scan_done_ < NETWORKS_FRESH_MS &&
               !wifi->get_scan_result().empty();
  // Never scan in the middle of joining a network, or with Wi-Fi off.
  if (fresh || this->state_ == improv::STATE_PROVISIONING || wifi->is_disabled()) {
    this->queue_wifi_networks_();
    return;
  }
  // Start the scan in the driver directly rather than with start_scanning():
  // that would take over the Wi-Fi component's state machine, which may be in
  // the middle of a connection attempt. The component stores the results of
  // any scan (Improv keeps it storing all of them) and calls
  // on_wifi_scan_results() when it is done. Same settings as its own scans.
  wifi_scan_config_t config{};
  config.show_hidden = false;
  config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  config.scan_time.active.min = 100;
  config.scan_time.active.max = 300;
  esp_err_t err = esp_wifi_scan_start(&config, false);
  if (err != ESP_OK) {
    // Busy (a scan or connection attempt is running) or not in station mode.
    ESP_LOGD(TAG, "Wi-Fi scan not started (%s); sending the last results", esp_err_to_name(err));
    this->queue_wifi_networks_();
    return;
  }
  ESP_LOGD(TAG, "Scanning for Wi-Fi networks");
  this->networks_wait_scan_ = true;
  this->networks_scan_start_ = App.get_loop_component_start_time();
}

void ESP32ImprovComponent::on_wifi_scan_results(const wifi::wifi_scan_vector_t<wifi::WiFiScanResult> &results) {
  this->last_scan_done_ = std::max<uint32_t>(millis(), 1);
  if (this->networks_wait_scan_)
    this->queue_wifi_networks_();
}

void ESP32ImprovComponent::queue_wifi_networks_() {
  this->networks_wait_scan_ = false;
  this->networks_to_send_.clear();
  this->networks_sent_ = 0;

  const auto &results = wifi::global_wifi_component->get_scan_result();
  std::vector<const wifi::WiFiScanResult *> best;
  best.reserve(std::min<size_t>(results.size(), 32));
  for (const auto &res : results) {
    if (res.get_is_hidden() || res.get_ssid().empty())
      continue;
    bool seen = false;
    for (auto &kept : best) {
      if (strcmp(kept->get_ssid().c_str(), res.get_ssid().c_str()) == 0) {
        if (res.get_rssi() > kept->get_rssi())
          kept = &res;
        seen = true;
        break;
      }
    }
    if (!seen)
      best.push_back(&res);
  }
  std::sort(best.begin(), best.end(),
            [](const wifi::WiFiScanResult *a, const wifi::WiFiScanResult *b) { return a->get_rssi() > b->get_rssi(); });
  if (best.size() > NETWORKS_MAX)
    best.resize(NETWORKS_MAX);

  this->networks_to_send_.reserve(best.size());
  for (const auto *res : best) {
    char rssi_buf[5];  // int8_t: -128 to 127, max 4 chars + null
    *int8_to_str(rssi_buf, res->get_rssi()) = '\0';
    this->networks_to_send_.push_back(improv::build_rpc_response(
        improv::GET_WIFI_NETWORKS, {std::string(res->get_ssid().c_str()), rssi_buf, YESNO(res->get_with_auth())}));
  }
  ESP_LOGD(TAG, "Sending %u Wi-Fi networks (%u scan results)", (unsigned) this->networks_to_send_.size(),
           (unsigned) results.size());
  this->networks_pending_ = true;
}

void ESP32ImprovComponent::send_next_wifi_network_(uint32_t now) {
  if (now - this->networks_last_sent_ < NETWORKS_SEND_INTERVAL_MS)
    return;
  this->networks_last_sent_ = now;
  if (this->networks_sent_ < this->networks_to_send_.size()) {
    this->send_response_(std::move(this->networks_to_send_[this->networks_sent_++]));
    return;
  }
  // A result with no strings marks the end of the list.
  this->send_response_(improv::build_rpc_response(improv::GET_WIFI_NETWORKS, std::vector<std::string>{}));
  this->cancel_wifi_networks_();
}

void ESP32ImprovComponent::cancel_wifi_networks_() {
  this->networks_pending_ = false;
  this->networks_wait_scan_ = false;
  this->networks_sent_ = 0;
  std::vector<std::vector<uint8_t>>().swap(this->networks_to_send_);  // give the memory back
}
// GAMEDAY: end

ESP32ImprovComponent *global_improv_component = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::esp32_improv

#endif
