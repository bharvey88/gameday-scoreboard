#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/select/select.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "espn_parse.h"

namespace esphome {
namespace gameday {

using ::espn::GameSnapshot;
using ::espn::GameState;
using ::espn::League;
using ::espn::Schedule;
using ::espn::TickerOptions;

// Exactly the fields the Home Assistant blueprint used to push to the page.
struct UpdateFields {
  std::string game_state;
  std::string team_abbr;
  int team_score{0};
  std::string opponent_abbr;
  int opponent_score{0};
  std::string status_text;
  std::string team_logo;
  std::string opponent_logo;
  int possession{0};
  int team_timeouts{0};
  int opponent_timeouts{0};
  std::string team_record;
  std::string opponent_record;
  std::string splash_text;
  uint32_t splash_color{0};
  std::string last_play;
  std::string clock_text;  // "5:36 - 3rd" while the game is live, else empty
  std::string down_distance;  // "3rd & 10" while live, else empty
  bool is_red_zone{false};
  uint32_t team_color{0xFFFFFF};
  uint32_t opponent_color{0xFFFFFF};
  std::string json;  // compact snapshot for the device web page
};

enum class SelectType : uint8_t { TEAM, TIMEZONE, MODE, FAVORITE };

enum class Mode : uint8_t { MY_TEAM = 0, LIVE_NFL = 1, LIVE_NCAA = 2, LIVE_ANY = 3 };

class GamedayComponent;

class GamedaySelect : public select::Select, public Component {
 public:
  void set_type(SelectType type) { this->type_ = type; }
  void set_slot(uint8_t slot) { this->slot_ = slot; }
  void set_parent(GamedayComponent *parent) { this->parent_ = parent; }

 protected:
  void control(const std::string &value) override;
  SelectType type_{SelectType::TEAM};
  uint8_t slot_{0};
  GamedayComponent *parent_{nullptr};
};

class GamedayComponent : public Component {
 public:
  void set_http(http_request::HttpRequestComponent *http) { this->http_ = http; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
  void set_team_select(select::Select *s) { this->team_select_ = s; }
  void set_timezone_select(select::Select *s) { this->timezone_select_ = s; }
  void set_mode_select(select::Select *s) { this->mode_select_ = s; }
  void set_favorite_select(uint8_t slot, select::Select *s) {
    if (slot >= 1 && slot <= 4)
      this->favorite_selects_[slot - 1] = s;
  }
  void add_on_update_callback(std::function<void(const UpdateFields &)> &&cb) {
    this->callbacks_.push_back(std::move(cb));
  }

  // Called by the selects and by template entities in YAML.
  void select_team(const std::string &option);
  void select_timezone(const std::string &option);
  void select_mode(const std::string &option);
  // Favorites: four team slots for the remote's numbered buttons.
  void select_favorite(uint8_t slot, const std::string &option);
  void press_favorite(uint8_t slot);
  void set_rotate_minutes(int minutes);
  int rotate_minutes() const { return this->prefs2_.rotate_minutes; }
  void refresh_now();

  bool ticker_clock() const { return this->flag_(FLAG_CLOCK); }
  bool ticker_down_distance() const { return this->flag_(FLAG_DOWN); }
  bool ticker_last_play() const { return this->flag_(FLAG_PLAY); }
  bool ticker_odds() const { return this->flag_(FLAG_ODDS); }
  bool opponent_splashes() const { return this->flag_(FLAG_OPP); }
  // Stored inverted (a "manual" bit) so panels flashed before this existed
  // come up with auto on without touching their saved preferences.
  bool tz_auto() const { return !this->flag_(FLAG_TZMANUAL); }
  void set_tz_auto(bool on) { this->set_flag_(FLAG_TZMANUAL, !on); }
  void set_ticker_clock(bool on) { this->set_flag_(FLAG_CLOCK, on); }
  void set_ticker_down_distance(bool on) { this->set_flag_(FLAG_DOWN, on); }
  void set_ticker_last_play(bool on) { this->set_flag_(FLAG_PLAY, on); }
  void set_ticker_odds(bool on) { this->set_flag_(FLAG_ODDS, on); }
  void set_opponent_splashes(bool on) { this->set_flag_(FLAG_OPP, on); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  static constexpr uint8_t FLAG_CLOCK = 1;
  static constexpr uint8_t FLAG_DOWN = 2;
  static constexpr uint8_t FLAG_PLAY = 4;
  static constexpr uint8_t FLAG_ODDS = 8;
  static constexpr uint8_t FLAG_OPP = 16;
  static constexpr uint8_t FLAG_TZMANUAL = 32;  // timezone was picked by hand; page must not override
  static constexpr uint8_t FLAGS_DEFAULT = FLAG_DOWN | FLAG_PLAY | FLAG_ODDS | FLAG_OPP;

  struct Prefs {
    uint8_t league;
    uint32_t team_id;
    uint8_t tz_index;
    uint8_t flags;
  } __attribute__((packed));
  // Kept separate so adding fields never invalidates the original blob.
  struct Prefs2 {
    uint8_t mode;
    uint8_t rotate_minutes;
  } __attribute__((packed));
  struct Prefs3 {
    uint8_t fav_league[4];
    uint32_t fav_id[4];  // 0 = slot empty
  } __attribute__((packed));

  bool flag_(uint8_t f) const { return (this->prefs_.flags & f) != 0; }
  void set_flag_(uint8_t f, bool on);
  void save_prefs_();
  void publish_selects_();
  void apply_timezone_();
  const ::espn::Team *current_team_() const;
  std::string team_option_() const;

  // One fetch cycle runs on its own FreeRTOS task so the display loop never
  // waits on the network. The main loop fills a Job, the worker performs the
  // HTTP requests and parsing into it, and the main loop applies the result.
  struct Job {
    uint32_t generation{0};
    const ::espn::Team *team{nullptr};
    // live-game modes: scan the day's games first, then follow one
    bool need_scan{false};
    std::vector<::espn::LiveGame> live;
    bool scan_ok{false};
    bool need_schedule{false};
    Schedule schedule;
    bool schedule_ok{false};
    bool no_event{false};
    GameSnapshot game;
    bool game_ok{false};
  };
  void start_job_();
  static void worker_(void *arg);
  void run_job_();
  void apply_job_();
  bool fetch_schedule_(const ::espn::Team *team, Schedule &out);
  bool fetch_game_(const ::espn::Team *team, const Schedule &schedule, GameSnapshot &out);
  bool fetch_live_games_(League league, std::vector<::espn::LiveGame> &out);
  bool live_mode_() const { return this->prefs2_.mode != (uint8_t) Mode::MY_TEAM; }
  uint32_t our_id_() const;  // team id the snapshot is oriented around
  std::shared_ptr<http_request::HttpContainer> open_(const std::string &url);
  void schedule_next_(uint32_t ms) { this->next_fetch_ms_ = millis() + ms; }
  uint32_t interval_for_phase_() const;
  void emit_(const ::espn::Splash &splash);
  void reset_game_();

  http_request::HttpRequestComponent *http_{nullptr};
  time::RealTimeClock *time_{nullptr};
  select::Select *team_select_{nullptr};
  select::Select *timezone_select_{nullptr};
  std::vector<std::function<void(const UpdateFields &)>> callbacks_;

  ESPPreferenceObject pref_;
  Prefs prefs_{};
  ESPPreferenceObject pref2_;
  Prefs2 prefs2_{};
  select::Select *mode_select_{nullptr};
  select::Select *favorite_selects_[4]{nullptr, nullptr, nullptr, nullptr};
  ESPPreferenceObject pref3_;
  Prefs3 prefs3_{};
  std::string favorite_option_(uint8_t slot) const;
  uint32_t live_away_id_{0};      // the followed live game's away team
  uint32_t live_started_ms_{0};   // when the current live game was picked
  bool live_none_{false};         // last scan found nothing in progress

  Schedule schedule_;
  uint32_t schedule_fetched_ms_{0};
  GameSnapshot game_;
  GameSnapshot prev_;
  uint32_t post_since_ms_{0};
  uint32_t next_fetch_ms_{0};
  uint8_t misses_{0};
  Job job_;
  uint32_t generation_{0};
  bool busy_{false};
  bool selects_published_{false};
  volatile bool job_done_{false};
};

class UpdateTrigger : public Trigger<const UpdateFields &> {
 public:
  explicit UpdateTrigger(GamedayComponent *parent) {
    parent->add_on_update_callback([this](const UpdateFields &f) { this->trigger(f); });
  }
};

}  // namespace gameday
}  // namespace esphome
